/*
 * ProtoSparseListObject.cpp
 *
 * Persistent AVL map keyed by a GC-traced `const ProtoObject*` word.
 * Identical to ProtoSparseList (core/ProtoSparseList.cpp) in algorithm and
 * forms; it differs only in the key type and in processReferences, which
 * reports the key to the collector when it is a cell pointer.
 * Specification: protoScala/docs/platform/PSLO-SPEC.md.
 */

#include "../headers/proto_internal.h"
#include "SparseListAlgorithms.h"

namespace proto
{
    //=========================================================================
    // ProtoSparseListObjectImplementation (AVL node)
    //=========================================================================
    ProtoSparseListObjectImplementation::ProtoSparseListObjectImplementation(
        ProtoContext* context, const ProtoObject* k, const ProtoObject* v,
        const ProtoSparseListObjectImplementation* p, const ProtoSparseListObjectImplementation* n, bool empty)
        : Cell(context), key(k), value(v), previous(p), next(n),
          size(empty ? 0 : (v != nullptr) + sparse_avl::nodeSize(p) + sparse_avl::nodeSize(n)),
          height(empty ? 0 : 1 + std::max(sparse_avl::nodeHeight(p), sparse_avl::nodeHeight(n))),
          isEmpty(empty) {}

    const ProtoObject* ProtoSparseListObjectImplementation::implAsObject(ProtoContext*) const {
        ProtoObjectPointer p{};
        p.sparseListObjectImplementation = this;
        p.op.pointer_tag = POINTER_TAG_SPARSE_LIST_OBJECT;
        return p.oid;
    }

    void ProtoSparseListObjectImplementation::processReferences(
        ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        // The key is a reference exactly like the value: an object whose only
        // reference is this key must survive.  Embedded keys (SmallInteger,
        // booleans, chars, None, inline strings) carry no cell and are skipped.
        if (const Cell* c = ProtoObject::asCellPointer(key)) method(context, self, c);
        if (const Cell* c = ProtoObject::asCellPointer(value)) method(context, self, c);
        if (previous) method(context, self, previous);
        if (next) method(context, self, next);
    }

    //=========================================================================
    // ProtoSparseListObjectSmallImplementation (inline form, up to 3 pairs)
    //=========================================================================
    ProtoSparseListObjectSmallImplementation::ProtoSparseListObjectSmallImplementation(ProtoContext* context)
        : Cell(context)
    {
        for (unsigned i = 0; i < MAX_INLINE; ++i) {
            keys[i] = nullptr;
            values[i] = nullptr;
        }
    }

    ProtoSparseListObjectSmallImplementation::ProtoSparseListObjectSmallImplementation(
        ProtoContext* context, unsigned n, const ProtoObject* const* ks, const ProtoObject* const* vs)
        : Cell(context)
    {
        for (unsigned i = 0; i < MAX_INLINE; ++i) {
            keys[i] = i < n ? ks[i] : nullptr;
            values[i] = i < n ? vs[i] : nullptr;
        }
    }

    const ProtoObject* ProtoSparseListObjectSmallImplementation::implAsObject(ProtoContext*) const {
        ProtoObjectPointer p{};
        p.sparseListObjectSmallImplementation = this;
        p.op.pointer_tag = POINTER_TAG_SPARSE_LIST_OBJECT;
        return p.oid;
    }

    void ProtoSparseListObjectSmallImplementation::processReferences(
        ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        for (unsigned i = 0; i < MAX_INLINE; ++i) {
            if (keys[i] == nullptr) continue;   // empty slot
            if (const Cell* c = ProtoObject::asCellPointer(keys[i])) method(context, self, c);
            if (const Cell* c = ProtoObject::asCellPointer(values[i])) method(context, self, c);
        }
    }

    //=========================================================================
    // Public trampolines
    //=========================================================================
    namespace {
        using Node = ProtoSparseListObjectImplementation;
        using Small = ProtoSparseListObjectSmallImplementation;

        inline const Cell* cellOf(const ProtoSparseListObject* m) {
            return reinterpret_cast<const Cell*>(reinterpret_cast<uintptr_t>(m) & ~0x3FUL);
        }

        // One tag serves both forms (PSLO-SPEC §3 rule 2): the form is the
        // cell's CellType.
        inline bool isSmall(const ProtoSparseListObject* m) {
            return cellOf(m)->getType() == CellType::SparseListObjectSmall;
        }

        inline const Small* smallOf(const ProtoSparseListObject* m) { return toImpl<const Small>(m); }
        inline const Node* avlOf(const ProtoSparseListObject* m) { return toImpl<const Node>(m); }

        inline const ProtoSparseListObject* handleOf(const ProtoObject* o) {
            return reinterpret_cast<const ProtoSparseListObject*>(o);
        }

        template<class Fn>
        void forEachPair(const ProtoSparseListObject* m, Fn&& fn) {
            if (isSmall(m)) {
                const Small* s = smallOf(m);
                const unsigned long n = sparse_avl::smallCount(s);
                for (unsigned i = 0; i < n; ++i) {
                    const ProtoObject* k;
                    const ProtoObject* v;
                    if (sparse_avl::smallPairAt(s, i, &k, &v)) fn(k, v);
                }
                return;
            }
            sparse_avl::inorder(avlOf(m), fn);
        }

        // 64-bit finalizer (MurmurHash3 fmix64).
        inline unsigned long mixWord(unsigned long x) {
            x ^= x >> 33;
            x *= 0xff51afd7ed558ccdUL;
            x ^= x >> 33;
            x *= 0xc4ceb9fe1a85ec53UL;
            x ^= x >> 33;
            return x;
        }
    }

    // D3 (PSLO-SPEC §7): a nullptr key is ignored silently.
    bool ProtoSparseListObject::has(ProtoContext* context, const ProtoObject* key) const {
        return getAt(context, key) != nullptr;
    }

    const ProtoObject* ProtoSparseListObject::getAt(ProtoContext*, const ProtoObject* key) const {
        if (!key) return nullptr;
        if (isSmall(this)) return sparse_avl::smallGetAt(smallOf(this), key);
        return sparse_avl::getAt(avlOf(this), key);
    }

    const ProtoSparseListObject* ProtoSparseListObject::setAt(ProtoContext* context, const ProtoObject* key, const ProtoObject* value) const {
        if (!key) return this;
        // GC critical section: the new path of cells is reachable only from
        // this C++ frame until the caller publishes the result.
        ProtoContext::CriticalSection cs(context);
        if (isSmall(this))
            return handleOf(sparse_avl::smallSetAt<Small, Node>(context, smallOf(this), key, value));
        return handleOf(sparse_avl::setAt(context, avlOf(this), key, value)->implAsObject(context));
    }

    const ProtoSparseListObject* ProtoSparseListObject::removeAt(ProtoContext* context, const ProtoObject* key) const {
        if (!key) return this;
        ProtoContext::CriticalSection cs(context);
        if (isSmall(this))
            return handleOf(sparse_avl::smallSetAt<Small, Node>(context, smallOf(this), key, nullptr));
        return handleOf(sparse_avl::removeAt(context, avlOf(this), key)->implAsObject(context));
    }

    unsigned long ProtoSparseListObject::getSize(ProtoContext*) const {
        if (isSmall(this)) return sparse_avl::smallCount(smallOf(this));
        return avlOf(this)->size;
    }

    const ProtoObject* ProtoSparseListObject::asObject(ProtoContext*) const {
        return reinterpret_cast<const ProtoObject*>(this);   // the handle is already tagged
    }

    // D4 (PSLO-SPEC §7): values compared by word identity.
    bool ProtoSparseListObject::isEqual(ProtoContext* context, const ProtoSparseListObject* other) const {
        if (this == other) return true;
        if (!other || getSize(context) != other->getSize(context)) return false;
        bool equal = true;
        forEachPair(this, [&](const ProtoObject* k, const ProtoObject* v) {
            if (equal && other->getAt(context, k) != v) equal = false;
        });
        return equal;
    }

    // Order-independent over (key word, value hash) pairs.
    unsigned long ProtoSparseListObject::getHash(ProtoContext* context) const {
        unsigned long h = mixWord(getSize(context));
        forEachPair(this, [&](const ProtoObject* k, const ProtoObject* v) {
            h += mixWord(sparse_avl::keyWord(k) ^ mixWord(v->getHash(context)));
        });
        return h;
    }

    void ProtoSparseListObject::processElements(ProtoContext* context, void* self,
        void (*method)(ProtoContext*, void*, const ProtoObject*, const ProtoObject*)) const
    {
        forEachPair(this, [&](const ProtoObject* k, const ProtoObject* v) { method(context, self, k, v); });
    }

    void ProtoSparseListObject::processValues(ProtoContext* context, void* self,
        void (*method)(ProtoContext*, void*, const ProtoObject*)) const
    {
        forEachPair(this, [&](const ProtoObject*, const ProtoObject* v) { method(context, self, v); });
    }

    //=========================================================================
    // ProtoSparseListObjectIteratorImplementation
    //=========================================================================
    ProtoSparseListObjectIteratorImplementation::ProtoSparseListObjectIteratorImplementation(
        ProtoContext* context, int s, const ProtoSparseListObjectImplementation* c,
        const ProtoSparseListObjectIteratorImplementation* q)
        : Cell(context), state(s), current(c), queue(q) {}

    int ProtoSparseListObjectIteratorImplementation::implHasNext() const {
        return state == ITERATOR_NEXT_THIS && current && !current->isEmpty;
    }

    const ProtoObject* ProtoSparseListObjectIteratorImplementation::implNextKey() const {
        return (state == ITERATOR_NEXT_THIS && current) ? current->key : nullptr;
    }

    const ProtoObject* ProtoSparseListObjectIteratorImplementation::implNextValue() const {
        return (state == ITERATOR_NEXT_THIS && current) ? current->value : nullptr;
    }

    const ProtoSparseListObjectIteratorImplementation*
    ProtoSparseListObjectIteratorImplementation::implAdvance(ProtoContext* context) const {
        if (state != ITERATOR_NEXT_THIS) return nullptr;
        if (current && current->next && !current->next->isEmpty) {
            ProtoContext::CriticalSection cs(context);
            return sparse_avl::iteratorWithQueue(context, current->next, queue);
        }
        return queue;
    }

    void ProtoSparseListObjectIteratorImplementation::processReferences(
        ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        if (current) method(context, self, current);
        if (queue) method(context, self, queue);
    }

    // D1 = (a): the iterator handle is the raw cell address and is never a
    // ProtoObject word.  This function exists only because Cell requires it;
    // it is used solely to form the C++ handle above.  No ProtoObject API
    // (getPrototype, getAttribute, asObject) is offered for this handle, so
    // no tag-0 non-ProtoObjectCell word can reach the attribute chain
    // (proto_internal.h, "Pointer tag layout", #92).
    const ProtoObject* ProtoSparseListObjectIteratorImplementation::implAsObject(ProtoContext*) const {
        return reinterpret_cast<const ProtoObject*>(this);
    }

    const ProtoSparseListObjectIterator* ProtoSparseListObject::getIterator(ProtoContext* context) const {
        ProtoContext::CriticalSection cs(context);
        const Node* root = isSmall(this)
            ? sparse_avl::smallPromote<Small, Node>(context, smallOf(this))   // as ProtoSparseList does
            : avlOf(this);
        const ProtoSparseListObjectIteratorImplementation* impl =
            sparse_avl::iteratorWithQueue(context, root,
                static_cast<const ProtoSparseListObjectIteratorImplementation*>(nullptr));
        return impl ? reinterpret_cast<const ProtoSparseListObjectIterator*>(impl->implAsObject(context)) : nullptr;
    }

    namespace {
        inline const ProtoSparseListObjectIteratorImplementation* iterImpl(const ProtoSparseListObjectIterator* it) {
            return toImpl<const ProtoSparseListObjectIteratorImplementation>(it);
        }
    }

    int ProtoSparseListObjectIterator::hasNext(ProtoContext*) const { if (!this) return 0; return iterImpl(this)->implHasNext(); }
    const ProtoObject* ProtoSparseListObjectIterator::nextKey(ProtoContext*) const { if (!this) return nullptr; return iterImpl(this)->implNextKey(); }
    const ProtoObject* ProtoSparseListObjectIterator::nextValue(ProtoContext*) const { if (!this) return nullptr; return iterImpl(this)->implNextValue(); }
    const ProtoSparseListObjectIterator* ProtoSparseListObjectIterator::advance(ProtoContext* context) const {
        if (!this) return nullptr;
        const auto* n = iterImpl(this)->implAdvance(context);
        return n ? reinterpret_cast<const ProtoSparseListObjectIterator*>(n->implAsObject(context)) : nullptr;
    }
}
