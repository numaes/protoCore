/*
 * ProtoAttributeList.cpp
 *
 * The list form of an object's own attributes and the AttributeStorage
 * operations that choose between it and the AVL form.  See the comments on
 * AttributeStorage and ProtoAttributeListImplementation in
 * headers/proto_internal.h for the layout and the form rules.
 */

#include "../headers/proto_internal.h"
#include "SparseListAlgorithms.h"
#include <algorithm>
#include <cstdlib>

namespace proto
{
    namespace {
        unsigned clampListMax(long v) {
            if (v < 0) return 0;
            if (v > static_cast<long>(kAttributeListMaxCeiling)) return kAttributeListMaxCeiling;
            return static_cast<unsigned>(v);
        }

        unsigned listMaxFromEnvironment() {
            if (const char* v = std::getenv("PROTOCORE_ATTR_LIST_MAX")) {
                char* end = nullptr;
                const long n = std::strtol(v, &end, 10);
                if (end != v) return clampListMax(n);
            }
            return kAttributeListMaxDefault;
        }

        // Exploration switch: 1 (default) completes a one-pair head with the
        // new attribute (a copy of the head cell); 0 always starts a new
        // one-pair head cell.  Both allocate one cell per new attribute.
        bool packFromEnvironment() {
            if (const char* v = std::getenv("PROTOCORE_ATTR_LIST_PACK")) return std::atoi(v) != 0;
            return true;
        }
        const bool s_packHead = packFromEnvironment();

        using List = ProtoAttributeListImplementation;
        using AVL = ProtoSparseListImplementation;

        struct Pair {
            proto_ulong key;
            const ProtoObject* value;
        };

        // Upper bound of the pairs a list form holds (count <= the ceiling)
        // plus room for the pairs a group write adds before promotion.
        constexpr unsigned kMaxPairs = kAttributeListMaxCeiling;

        // The cells for pairs[0 .. n) followed by `tail`.  pairs[0] is the
        // newest; when n is odd the head cell holds pairs[0] alone, so every
        // other cell is full.
        const List* buildPrefix(ProtoContext* context, const Pair* pairs, unsigned n, const List* tail) {
            const List* cur = tail;
            unsigned i = n;
            while (i >= 2) {
                cur = new(context) List(context, cur, pairs[i - 2].key, pairs[i - 2].value,
                                        pairs[i - 1].key, pairs[i - 1].value);
                i -= 2;
            }
            if (i == 1) cur = new(context) List(context, cur, pairs[0].key, pairs[0].value, 0, nullptr);
            return cur;
        }

        // A balanced AVL over `pairs` (any order, unique keys, non-null values).
        const AVL* buildAVL(ProtoContext* context, Pair* pairs, unsigned n) {
            if (n == 0) return sparse_avl::makeEmpty<AVL>(context);
            std::sort(pairs, pairs + n, [](const Pair& a, const Pair& b) { return a.key < b.key; });
            proto_ulong keys[kMaxPairs * 2 + 2];
            const ProtoObject* values[kMaxPairs * 2 + 2];
            for (unsigned i = 0; i < n; ++i) {
                keys[i] = pairs[i].key;
                values[i] = pairs[i].value;
            }
            return sparse_avl::buildSorted<AVL>(context, keys, values, 0, n);
        }

        // A list over `pairs` (newest first) with no tail; empty storage when n == 0.
        AttributeStorage buildList(ProtoContext* context, const Pair* pairs, unsigned n) {
            if (n == 0) return AttributeStorage();
            return AttributeStorage(buildPrefix(context, pairs, n, nullptr));
        }

        // Locates `key` in the list: the cell holding it and its depth
        // (0 = head).  nullptr when absent.
        const List* locate(const List* head, proto_ulong key, unsigned& depth, const ProtoObject*& value) {
            depth = 0;
            for (const List* n = head; n; n = n->next, ++depth) {
                if (n->key0 == key) { value = n->value0; return n; }
                if (n->value1 && n->key1 == key) { value = n->value1; return n; }
            }
            value = nullptr;
            return nullptr;
        }

        // The pairs of the cells from `head` up to and including `last`,
        // newest first, leaving out `skip` (0 skips nothing: no key is 0).
        unsigned gatherPrefix(const List* head, const List* last, proto_ulong skip, Pair* out) {
            unsigned n = 0;
            for (const List* c = head; ; c = c->next) {
                if (c->key0 != skip) out[n++] = {c->key0, c->value0};
                if (c->value1 && c->key1 != skip) out[n++] = {c->key1, c->value1};
                if (c == last) break;
            }
            return n;
        }

        // The list form's setAt (value != nullptr).
        AttributeStorage listSetAt(ProtoContext* context, AttributeStorage self, const List* head,
                                   proto_ulong key, const ProtoObject* value) {
            unsigned depth = 0;
            const ProtoObject* old = nullptr;
            const List* hit = locate(head, key, depth, old);
            const proto_ulong count = head ? head->count : 0;
            if (!hit) {
                if (count + 1 > attributeListMax()) {
                    // Promotion: the AVL over every pair plus the new one.
                    Pair pairs[kMaxPairs * 2 + 2];
                    unsigned n = 0;
                    for (const List* c = head; c; c = c->next) {
                        pairs[n++] = {c->key0, c->value0};
                        if (c->value1) pairs[n++] = {c->key1, c->value1};
                    }
                    pairs[n++] = {key, value};
                    return AttributeStorage(buildAVL(context, pairs, n));
                }
                if (s_packHead && head && !head->value1)
                    return AttributeStorage(new(context) List(context, head->next, key, value,
                                                              head->key0, head->value0));
                return AttributeStorage(new(context) List(context, head, key, value, 0, nullptr));
            }
            if (old == value) return self;
            // Overwrite in the head cell (the common case for a field written
            // repeatedly): one cell, the new pair first, the head's other pair
            // kept, the rest shared.  The general path below builds the same.
            if (hit == head) {
                if (!head->value1)
                    return AttributeStorage(new(context) List(context, head->next, key, value, 0, nullptr));
                const bool first = head->key0 == key;
                return AttributeStorage(new(context) List(context, head->next, key, value,
                                                          first ? head->key1 : head->key0,
                                                          first ? head->value1 : head->value0));
            }
            // Overwrite: copy the cells up to the old pair, drop it, put the
            // new pair at the head, share the rest.
            Pair pairs[kMaxPairs * 2 + 2];
            pairs[0] = {key, value};
            const unsigned n = 1 + gatherPrefix(head, hit, key, pairs + 1);
            return AttributeStorage(buildPrefix(context, pairs, n, hit->next));
        }

        AttributeStorage listRemoveAt(ProtoContext* context, AttributeStorage self, const List* head,
                                      proto_ulong key) {
            unsigned depth = 0;
            const ProtoObject* old = nullptr;
            const List* hit = locate(head, key, depth, old);
            if (!hit) return self;
            Pair pairs[kMaxPairs * 2 + 2];
            const unsigned n = gatherPrefix(head, hit, key, pairs);
            if (n == 0 && !hit->next) return AttributeStorage();
            return AttributeStorage(buildPrefix(context, pairs, n, hit->next));
        }

        // Demotion: the list over the AVL's pairs minus `skip`.
        AttributeStorage demote(ProtoContext* context, AttributeStorage avl, proto_ulong skip) {
            Pair pairs[kMaxPairs * 2 + 2];
            unsigned n = 0;
            avl.forEach([&](proto_ulong k, const ProtoObject* v) {
                if (k != skip) pairs[n++] = {k, v};
            });
            return buildList(context, pairs, n);
        }
    }

    std::atomic<unsigned> g_attributeListMax{listMaxFromEnvironment()};

    void setAttributeListMax(unsigned n) {
        g_attributeListMax.store(clampListMax(static_cast<long>(n)), std::memory_order_relaxed);
    }

    //=========================================================================
    // ProtoAttributeListImplementation
    //=========================================================================
    ProtoAttributeListImplementation::ProtoAttributeListImplementation(
        ProtoContext* context, const ProtoAttributeListImplementation* next,
        proto_ulong k0, const ProtoObject* v0, proto_ulong k1, const ProtoObject* v1)
        : Cell(context), next(next),
          count((v1 ? 2 : 1) + (next ? next->count : 0)),
          key0(k0), value0(v0), key1(v1 ? k1 : 0), value1(v1) {}

    const ProtoObject* ProtoAttributeListImplementation::implAsObject(ProtoContext*) const {
        // An internal implementation detail, never a public object (as
        // ParentLinkImplementation).
        return PROTO_NONE;
    }

    void ProtoAttributeListImplementation::processReferences(
        ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        if (next) method(context, self, next);
        if (const Cell* c = ProtoObject::asCellPointer(value0)) method(context, self, c);
        if (const Cell* c = ProtoObject::asCellPointer(value1)) method(context, self, c);
    }

    //=========================================================================
    // AttributeStorage
    //=========================================================================
    AttributeStorage AttributeStorage::empty(ProtoContext* context) {
        // The empty AVL cell, exactly as before the list form existed: the
        // cells an empty object costs do not change (the collector's tests
        // and pacing count on it).  The empty tree has no form of its own:
        // the first write builds whichever form the resulting size calls for.
        return AttributeStorage(context->newSparseListImpl());
    }

    bool AttributeStorage::isEmptyTree() const {
        if (isNull()) return true;
        if (isList()) return false;
        return avl()->isEmpty;
    }

    AttributeStorage AttributeStorage::setAt(ProtoContext* context, proto_ulong key,
                                             const ProtoObject* value) const {
        if (value == nullptr) return removeAt(context, key);
        if (isList()) return listSetAt(context, *this, list(), key, value);
        if (isEmptyTree()) {
            if (attributeListMax() == 0)
                return AttributeStorage(new(context) AVL(context, key, value, nullptr, nullptr, false));
            return listSetAt(context, *this, nullptr, key, value);
        }
        const AVL* t = avl();
        const AVL* r = t->implSetAt(context, key, value);
        return r == t ? *this : AttributeStorage(r);
    }

    AttributeStorage AttributeStorage::removeAt(ProtoContext* context, proto_ulong key) const {
        if (isEmptyTree()) return *this;
        if (isList()) return listRemoveAt(context, *this, list(), key);
        const AVL* t = avl();
        if (!t->implHas(context, key)) return *this;
        const unsigned max = attributeListMax();
        if (max > 0 && size() - 1 <= max / 2) return demote(context, *this, key);
        return AttributeStorage(t->implRemoveAt(context, key));
    }

    AttributeStorage AttributeStorage::setSorted(ProtoContext* context, const proto_ulong* keys,
                                                 const ProtoObject* const* values, size_t m) const {
        if (m == 0) return *this;
        const unsigned max = attributeListMax();
        if (!isList() && !(isEmptyTree() && max > 0)) {
            const AVL* t = avl();
            const AVL* r = sparse_avl::setSorted(context, t, keys, values, 0, m);
            if (r == t) return *this;
            if (!r) return max == 0 ? AttributeStorage(sparse_avl::makeEmpty<AVL>(context)) : AttributeStorage();
            AttributeStorage out(r);
            if (max > 0 && out.size() < size() && out.size() <= max / 2) return demote(context, out, 0);
            return out;
        }
        if (isNull() && max == 0) {   // unreachable with empty() above; kept for null storages
            const AVL* r = sparse_avl::buildSorted<AVL>(context, keys, values, 0, m);
            return AttributeStorage(r ? r : sparse_avl::makeEmpty<AVL>(context));
        }

        // List (or empty) form.  Each write is located once; `deepest` is the
        // depth of the deepest cell a write changes or empties.
        const List* head = list();   // nullptr for an empty tree
        Pair front[kMaxPairs * 2 + 2];   // the pairs that go to the head
        unsigned nFront = 0;
        proto_ulong count = head ? head->count : 0;
        int deepest = -1;
        const List* deepestCell = nullptr;
        bool changed = false;
        // A group larger than the ceiling always promotes; collect at most
        // what the buffers hold and promote through the AVL path otherwise.
        if (m > kMaxPairs) {
            const AVL* base = toAVL(context);
            const AVL* r = base ? sparse_avl::setSorted(context, base, keys, values, 0, m)
                                : sparse_avl::buildSorted<AVL>(context, keys, values, 0, m);
            if (r == base) return *this;
            if (!r) return AttributeStorage();
            AttributeStorage out(r);
            if (out.size() <= max) return demote(context, out, 0);
            return out;
        }
        for (size_t i = 0; i < m; ++i) {
            unsigned depth = 0;
            const ProtoObject* old = nullptr;
            const List* hit = locate(head, keys[i], depth, old);
            if (hit) {
                if (values[i] == old) continue;
                changed = true;
                if (static_cast<int>(depth) > deepest) { deepest = static_cast<int>(depth); deepestCell = hit; }
                if (values[i] == nullptr) --count;
                else front[nFront++] = {keys[i], values[i]};
            } else if (values[i] != nullptr) {
                changed = true;
                ++count;
                front[nFront++] = {keys[i], values[i]};
            }
        }
        if (!changed) return *this;
        if (count == 0) return AttributeStorage();
        if (count > max) {
            // Promotion: every surviving old pair plus the writes.
            Pair pairs[kMaxPairs * 2 + 2];
            unsigned n = 0;
            for (unsigned i = 0; i < nFront; ++i) pairs[n++] = front[i];
            for (const List* c = head; c; c = c->next) {
                const Pair cp[2] = {{c->key0, c->value0}, {c->key1, c->value1}};
                for (unsigned s = 0; s < c->pairsHere(); ++s) {
                    if (!std::binary_search(keys, keys + m, cp[s].key)) pairs[n++] = cp[s];
                }
            }
            return AttributeStorage(buildAVL(context, pairs, n));
        }
        // The new pairs at the head, then the untouched pairs of the cells up
        // to the deepest changed one; the cells after it are shared.
        unsigned n = nFront;
        Pair* pairs = front;
        const List* tail = head;
        // A one-pair head cannot stay behind the new pairs: only the head may
        // hold a single pair.
        if (!deepestCell && head && !head->value1) deepestCell = head;
        if (deepestCell) {
            for (const List* c = head; ; c = c->next) {
                const Pair cp[2] = {{c->key0, c->value0}, {c->key1, c->value1}};
                for (unsigned s = 0; s < c->pairsHere(); ++s) {
                    if (!std::binary_search(keys, keys + m, cp[s].key)) pairs[n++] = cp[s];
                    else {
                        // A write that leaves this key's value as it was keeps
                        // the old pair where it would otherwise be lost.
                        const size_t at = static_cast<size_t>(
                            std::lower_bound(keys, keys + m, cp[s].key) - keys);
                        if (values[at] == cp[s].value) pairs[n++] = cp[s];
                    }
                }
                if (c == deepestCell) break;
            }
            tail = deepestCell->next;
        }
        return AttributeStorage(buildPrefix(context, pairs, n, tail));
    }

    const ProtoSparseListImplementation* AttributeStorage::toAVL(ProtoContext* context) const {
        if (isNull()) return nullptr;
        if (!isList()) return avl();
        if (!isList()) return avl();
        Pair pairs[kMaxPairs * 2 + 2];
        unsigned n = 0;
        forEach([&](proto_ulong k, const ProtoObject* v) { pairs[n++] = {k, v}; });
        return buildAVL(context, pairs, n);
    }
}
