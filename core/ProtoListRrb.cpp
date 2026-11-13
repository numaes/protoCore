// ProtoListRrb.cpp — the RRB-tree form of ProtoList: a relaxed radix
// balanced tree (Bagwell & Rompf 2011; L'orange 2014) with six slots per
// 64-byte cell and an open tail.  Design, the alternatives it beat and the
// measurements behind every parameter: docs/LIST_RRB_DESIGN.md.
//
// Shapes:
//   leaf      ProtoListRrbLeaf: up to 6 elements; length known from the parent
//   strict    ProtoListRrbInner alone: dense (every child full but the last,
//             every leaf below full); indexed by base-6 digits
//   relaxed   ProtoListRrbSizes (cumulative sizes) -> ProtoListRrbInner
//   root      ProtoListRrbImplementation: tree, its height and size, the tail
// A child reference is a tagged address; bit 0 marks a relaxed child.
//
// Balance: concatenation repacks the nodes on its seam to the optimum
// (ceil(slots / 6) nodes, seam slack E = 0: with six slots a larger slack
// lets nodes stay half full and the height grow like log3 n).  Insertion and
// removal anywhere are native, B-tree style: one path copied; a full node
// splits, first sharing its slots with an emptier neighbour (B*); a node at
// half capacity or less merges with a neighbour when they fit.

#include "ProtoListRrb.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace proto {

    //=========================================================================
    // Cells
    //=========================================================================

    ProtoListRrbLeaf::ProtoListRrbLeaf(ProtoContext* context, unsigned n, const ProtoObject* const* items)
        : Cell(context)
    {
        for (unsigned i = 0; i < WIDTH; ++i) slots[i] = i < n ? items[i] : nullptr;
    }

    const ProtoObject* ProtoListRrbLeaf::implAsObject(ProtoContext*) const {
        // Internal cell, never handed to the object API: the raw address.
        return reinterpret_cast<const ProtoObject*>(this);
    }

    void ProtoListRrbLeaf::processReferences(ProtoContext* context, void* self,
                                             void (*method)(ProtoContext*, void*, const Cell*)) const {
        for (unsigned i = 0; i < WIDTH; ++i) {
            if (const Cell* c = ProtoObject::asCellPointer(slots[i])) method(context, self, c);
        }
    }

    ProtoListRrbInner::ProtoListRrbInner(ProtoContext* context, unsigned n, const uintptr_t* refs)
        : Cell(context)
    {
        for (unsigned i = 0; i < WIDTH; ++i) slots[i] = i < n ? refs[i] : 0;
    }

    const ProtoObject* ProtoListRrbInner::implAsObject(ProtoContext*) const {
        return reinterpret_cast<const ProtoObject*>(this);
    }

    void ProtoListRrbInner::processReferences(ProtoContext* context, void* self,
                                              void (*method)(ProtoContext*, void*, const Cell*)) const {
        for (unsigned i = 0; i < WIDTH; ++i) {
            if (slots[i]) method(context, self, reinterpret_cast<const Cell*>(slots[i] & ~static_cast<uintptr_t>(1)));
        }
    }

    ProtoListRrbSizes::ProtoListRrbSizes(ProtoContext* context, unsigned n, const proto_ulong* childSizes,
                                         const ProtoListRrbInner* k)
        : Cell(context), cum{}, count(static_cast<unsigned char>(n)), pad{}, kids(k)
    {
        proto_ulong acc = 0;
        for (unsigned i = 0; i < n; ++i) {
            acc += childSizes[i];
            for (unsigned b = 0; b < 6; ++b) cum[6 * i + b] = static_cast<unsigned char>(acc >> (8 * b));
        }
    }

    const ProtoObject* ProtoListRrbSizes::implAsObject(ProtoContext*) const {
        return reinterpret_cast<const ProtoObject*>(this);
    }

    void ProtoListRrbSizes::processReferences(ProtoContext* context, void* self,
                                              void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (kids) method(context, self, kids);
    }

    ProtoListRrbImplementation::ProtoListRrbImplementation(
        ProtoContext* context, uintptr_t t, unsigned h, proto_ulong ts, const ProtoListRrbLeaf* tl, unsigned tlen)
        : Cell(context), tree(t), tail(tl), treeSize(ts), reserved(0), size(ts + tlen),
          avlHeight(0), avlIsEmpty(0), height(static_cast<unsigned char>(h)),
          tailLen(static_cast<unsigned char>(tlen))
    {
    }

    const ProtoObject* ProtoListRrbImplementation::implAsObject(ProtoContext*) const {
        ProtoObjectPointer p{};
        p.oid = reinterpret_cast<const ProtoObject*>(this);
        p.op.pointer_tag = POINTER_TAG_LIST;
        return p.oid;
    }

    const ProtoList* ProtoListRrbImplementation::asProtoList(ProtoContext* context) const {
        return reinterpret_cast<const ProtoList*>(implAsObject(context));
    }

    void ProtoListRrbImplementation::processReferences(ProtoContext* context, void* self,
                                                       void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (tree) method(context, self, reinterpret_cast<const Cell*>(tree & ~static_cast<uintptr_t>(1)));
        if (tail) method(context, self, tail);
    }

namespace rrb {

    //=========================================================================
    // Telling the forms apart (layout contract, see proto_internal.h)
    //=========================================================================

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
    constexpr std::size_t kAvlHeightOffset = offsetof(ProtoListImplementation, height);
    constexpr std::size_t kAvlIsEmptyOffset = offsetof(ProtoListImplementation, isEmpty);
    static_assert(offsetof(ProtoListRrbImplementation, avlHeight) == kAvlHeightOffset,
                  "RRB root: avlHeight must overlay ProtoListImplementation::height");
    static_assert(offsetof(ProtoListRrbImplementation, avlIsEmpty) == kAvlIsEmptyOffset,
                  "RRB root: avlIsEmpty must overlay ProtoListImplementation::isEmpty");
    static_assert(offsetof(ProtoListRrbImplementation, size) == offsetof(ProtoListImplementation, size),
                  "RRB root: size must overlay ProtoListImplementation::size");
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    const Root* rootOf(const ProtoList* list) {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(list);
        if (pa.op.pointer_tag != POINTER_TAG_LIST) return nullptr;
        const auto* bytes = reinterpret_cast<const unsigned char*>(
            reinterpret_cast<uintptr_t>(list) & ~static_cast<uintptr_t>(0x3F));
        // A non-empty AVL node has height >= 1; an empty one has isEmpty set.
        if (bytes[kAvlHeightOffset] != 0 || bytes[kAvlIsEmptyOffset] != 0) return nullptr;
        return reinterpret_cast<const Root*>(bytes);
    }

    //=========================================================================
    // Engine
    //=========================================================================

    namespace {
        using Inner = ProtoListRrbInner;
        using Sizes = ProtoListRrbSizes;

        // cap[h]: elements a full subtree of height h holds, 6^(h+1).
        struct Caps {
            proto_ulong v[24];
            Caps() { v[0] = M; for (int i = 1; i < 24; ++i) v[i] = v[i - 1] * M; }
        };
        const Caps kCap;
        inline proto_ulong cap(unsigned h) { return kCap.v[h]; }

        constexpr unsigned kInv = 1;       // nodes with >= M - kInv slots are not redistributed

        inline bool isRel(uintptr_t r) { return (r & 1u) != 0; }
        inline const Leaf* leafOf(uintptr_t r) { return reinterpret_cast<const Leaf*>(r); }
        inline const Inner* innerOf(uintptr_t r) { return reinterpret_cast<const Inner*>(r); }
        inline const Sizes* sizesOf(uintptr_t r) { return reinterpret_cast<const Sizes*>(r & ~static_cast<uintptr_t>(1)); }

        struct Sub { uintptr_t ref; proto_ulong size; };
        struct Kids {
            Sub v[16];
            unsigned n = 0;
            void push(Sub s) { v[n++] = s; }
        };

        // The mutable picture of a root while an operation builds it.
        struct Shape {
            uintptr_t tree = 0;
            proto_ulong treeSize = 0;
            unsigned h = 0;
            const Leaf* tail = nullptr;
            unsigned tailLen = 0;
        };
        Shape shapeOf(const Root* r) {
            Shape s;
            s.tree = r->tree; s.treeSize = r->treeSize; s.h = r->height; s.tail = r->tail; s.tailLen = r->tailLen;
            return s;
        }
        const Root* make(ProtoContext* c, const Shape& s) {
            if (s.treeSize + s.tailLen == 0) return nullptr;
            return new (c) Root(c, s.treeSize ? s.tree : 0, s.treeSize ? s.h : 0, s.treeSize, s.tail, s.tailLen);
        }

        // ------------------------------------------------------------ builders
        Sub mkLeaf(ProtoContext* c, const ProtoObject* const* v, unsigned n) {
            return {reinterpret_cast<uintptr_t>(new (c) Leaf(c, n, v)), n};
        }
        const Leaf* newLeaf(ProtoContext* c, const ProtoObject* const* v, unsigned n) {
            return new (c) Leaf(c, n, v);
        }
        // An inner node of height h over k[0..n) (height h - 1): strict when it can be.
        Sub mkInner(ProtoContext* c, const Sub* k, unsigned n, unsigned h) {
            const proto_ulong full = cap(h - 1);
            bool strict = true; proto_ulong total = 0;
            uintptr_t refs[M]; proto_ulong sizes[M];
            for (unsigned i = 0; i < n; ++i) {
                total += k[i].size;
                refs[i] = k[i].ref; sizes[i] = k[i].size;
                if (isRel(k[i].ref)) strict = false;
                if (i + 1 < n && k[i].size != full) strict = false;
                if (h == 1 && k[i].size != M) strict = false;       // leaves under a strict node are full
            }
            const Inner* in = new (c) Inner(c, n, refs);
            if (strict) return {reinterpret_cast<uintptr_t>(in), total};
            const Sizes* sz = new (c) Sizes(c, n, sizes, in);
            return {reinterpret_cast<uintptr_t>(sz) | 1u, total};
        }
        Sub path(ProtoContext* c, unsigned h, Sub leaf) {
            for (unsigned l = 1; l <= h; ++l) leaf = mkInner(c, &leaf, 1, l);
            return leaf;
        }

        // ------------------------------------------------------------ reading
        Kids children(Sub s, unsigned h) {
            Kids k;
            if (isRel(s.ref)) {
                const Sizes* r = sizesOf(s.ref); proto_ulong prev = 0;
                for (unsigned i = 0; i < r->count; ++i) {
                    const proto_ulong cu = r->cumAt(i);
                    k.push({r->kids->slots[i], cu - prev}); prev = cu;
                }
            } else {
                const Inner* in = innerOf(s.ref); const proto_ulong cc = cap(h - 1);
                proto_ulong rem = s.size;
                for (unsigned i = 0; rem > 0; ++i) {
                    const proto_ulong t = std::min(cc, rem);
                    k.push({in->slots[i], t}); rem -= t;
                }
            }
            return k;
        }
        unsigned slotCount(Sub s, unsigned h) {
            if (h == 0) return static_cast<unsigned>(s.size);
            if (isRel(s.ref)) return sizesOf(s.ref)->count;
            return static_cast<unsigned>((s.size + cap(h - 1) - 1) / cap(h - 1));
        }
        const ProtoObject* getTree(uintptr_t ref, unsigned h, proto_ulong i) {
            while (h > 0) {
                if (isRel(ref)) {
                    const Sizes* r = sizesOf(ref);
                    // i / 6^h is a lower bound of the child: none holds more than 6^h.
                    proto_ulong q = i;
                    for (unsigned l = 0; l < h; ++l) q /= M;
                    unsigned k = static_cast<unsigned>(q);
                    while (r->cumAt(k) <= i) ++k;
                    if (k) i -= r->cumAt(k - 1);
                    ref = r->kids->slots[k];
                    --h;
                } else {
                    unsigned d[24]; proto_ulong x = i;
                    for (unsigned l = 0; l <= h; ++l) { d[l] = static_cast<unsigned>(x % M); x /= M; }
                    const Inner* n = innerOf(ref);
                    for (unsigned l = h; l > 1; --l) n = innerOf(n->slots[d[l]]);
                    return leafOf(n->slots[d[1]])->slots[d[0]];
                }
            }
            return leafOf(ref)->slots[i];
        }
        // The leaf holding element i of a tree, and the index of its first element.
        Sub locateIn(Sub s, unsigned h, proto_ulong i, proto_ulong& start) {
            start = 0;
            while (h > 0) {
                Kids k = children(s, h);
                unsigned j = 0;
                while (i >= k.v[j].size) { i -= k.v[j].size; start += k.v[j].size; ++j; }
                s = k.v[j]; --h;
            }
            return s;
        }
        bool walk(Sub s, unsigned h, bool (*fn)(void*, const ProtoObject*), void* data) {
            if (h == 0) {
                const Leaf* lf = leafOf(s.ref);
                for (proto_ulong i = 0; i < s.size; ++i) if (fn(data, lf->slots[i])) return true;
                return false;
            }
            Kids k = children(s, h);
            for (unsigned i = 0; i < k.n; ++i) if (walk(k.v[i], h - 1, fn, data)) return true;
            return false;
        }

        // ------------------------------------------------------------ edge operations
        // Hangs a full leaf at the right edge; ref 0 when the subtree has no room.
        Sub push(ProtoContext* c, Sub s, unsigned h, Sub leaf) {
            if (h == 0) return {0, 0};
            if (!isRel(s.ref) && s.size == cap(h)) return {0, 0};
            Kids k = children(s, h);
            if (h > 1) {
                const Sub r = push(c, k.v[k.n - 1], h - 1, leaf);
                if (r.ref) { k.v[k.n - 1] = r; return mkInner(c, k.v, k.n, h); }
            }
            if (k.n == M) return {0, 0};
            k.push(path(c, h - 1, leaf));
            return mkInner(c, k.v, k.n, h);
        }

        // ------------------------------------------------------------ concatenation
        // Streams the slots of the nodes of `all` (height lvl) and cuts them
        // into n nodes of plan[k] slots; a cut that is one original node reuses it.
        Kids cut(ProtoContext* c, const Kids& all, unsigned lvl, const unsigned* plan, unsigned n) {
            Kids out;
            if (lvl == 0) {
                const ProtoObject* items[16 * M]; unsigned origStart[16], at = 0;
                for (unsigned i = 0; i < all.n; ++i) {
                    origStart[i] = at;
                    const Leaf* lf = leafOf(all.v[i].ref);
                    for (proto_ulong e = 0; e < all.v[i].size; ++e) items[at++] = lf->slots[e];
                }
                unsigned pos = 0;
                for (unsigned k = 0; k < n; ++k) {
                    Sub reuse{0, 0};
                    for (unsigned i = 0; i < all.n; ++i)
                        if (origStart[i] == pos && all.v[i].size == plan[k]) reuse = all.v[i];
                    out.push(reuse.ref ? reuse : mkLeaf(c, items + pos, plan[k]));
                    pos += plan[k];
                }
            } else {
                Sub items[16 * M]; unsigned origStart[16], origLen[16], at = 0;
                for (unsigned i = 0; i < all.n; ++i) {
                    origStart[i] = at;
                    const Kids k = children(all.v[i], lvl);
                    origLen[i] = k.n;
                    for (unsigned e = 0; e < k.n; ++e) items[at++] = k.v[e];
                }
                unsigned pos = 0;
                for (unsigned k = 0; k < n; ++k) {
                    Sub reuse{0, 0};
                    for (unsigned i = 0; i < all.n; ++i)
                        if (origStart[i] == pos && origLen[i] == plan[k]) reuse = all.v[i];
                    out.push(reuse.ref ? reuse : mkInner(c, items + pos, plan[k], lvl));
                    pos += plan[k];
                }
            }
            return out;
        }
        // Repacks the nodes of a seam to the optimum, ceil(slots / M) nodes:
        // L'orange's plan (the first node with room is poured into the ones
        // after it) while it can, else the full prefix is kept and the rest
        // repacked into full nodes.
        Kids rebalance(ProtoContext* c, const Kids& all, unsigned lvl) {
            unsigned plan[16]; unsigned n = all.n; proto_ulong S = 0;
            for (unsigned i = 0; i < n; ++i) { plan[i] = slotCount(all.v[i], lvl); S += plan[i]; }
            const unsigned opt = static_cast<unsigned>((S + M - 1) / M);
            if (n <= opt) return all;
            while (n > opt) {
                unsigned i = 0;
                while (i < n && plan[i] >= M - kInv) ++i;
                const unsigned r0 = i < n ? plan[i] : 0;
                unsigned room = 0;
                for (unsigned j = i + 1; j < n; ++j) room += M - plan[j];
                if (i >= n || room < r0) {
                    unsigned f = 0; proto_ulong rest = S;
                    while (f < n && plan[f] == M) { rest -= M; ++f; }
                    n = f;
                    while (rest > 0) {
                        const unsigned t = static_cast<unsigned>(std::min<proto_ulong>(rest, M));
                        plan[n++] = t; rest -= t;
                    }
                    break;
                }
                unsigned r = r0;
                for (unsigned j = i + 1; r > 0; ++j) {
                    const unsigned t = std::min(r, M - plan[j]);
                    plan[j] += t; r -= t;
                }
                for (unsigned j = i; j + 1 < n; ++j) plan[j] = plan[j + 1];
                --n;
            }
            return cut(c, all, lvl, plan, n);
        }
        Kids pack(ProtoContext* c, const Kids& all, unsigned h) {   // height h - 1 nodes into 1 or 2 of height h
            Kids out;
            if (all.n <= M) { out.push(mkInner(c, all.v, all.n, h)); return out; }
            out.push(mkInner(c, all.v, M, h));
            out.push(mkInner(c, all.v + M, all.n - M, h));
            return out;
        }
        // left ++ right as one or two nodes of height max(hl, hr).
        Kids merge(ProtoContext* c, Sub L, unsigned hl, Sub R, unsigned hr) {
            if (hl > hr) {
                const Kids kl = children(L, hl);
                const Kids mid = merge(c, kl.v[kl.n - 1], hl - 1, R, hr);
                Kids all;
                for (unsigned i = 0; i + 1 < kl.n; ++i) all.push(kl.v[i]);
                for (unsigned i = 0; i < mid.n; ++i) all.push(mid.v[i]);
                return pack(c, rebalance(c, all, hl - 1), hl);
            }
            if (hl < hr) {
                const Kids kr = children(R, hr);
                const Kids mid = merge(c, L, hl, kr.v[0], hr - 1);
                Kids all;
                for (unsigned i = 0; i < mid.n; ++i) all.push(mid.v[i]);
                for (unsigned i = 1; i < kr.n; ++i) all.push(kr.v[i]);
                return pack(c, rebalance(c, all, hr - 1), hr);
            }
            Kids out;
            if (hl == 0) {
                if (L.size + R.size <= M) {
                    const ProtoObject* b[M];
                    for (proto_ulong i = 0; i < L.size; ++i) b[i] = leafOf(L.ref)->slots[i];
                    for (proto_ulong i = 0; i < R.size; ++i) b[L.size + i] = leafOf(R.ref)->slots[i];
                    out.push(mkLeaf(c, b, static_cast<unsigned>(L.size + R.size)));
                } else {
                    out.push(L); out.push(R);
                }
                return out;
            }
            const Kids kl = children(L, hl), kr = children(R, hr);
            const Kids mid = merge(c, kl.v[kl.n - 1], hl - 1, kr.v[0], hr - 1);
            Kids all;
            for (unsigned i = 0; i + 1 < kl.n; ++i) all.push(kl.v[i]);
            for (unsigned i = 0; i < mid.n; ++i) all.push(mid.v[i]);
            for (unsigned i = 1; i < kr.n; ++i) all.push(kr.v[i]);
            return pack(c, rebalance(c, all, hl - 1), hl);
        }
        void concatTrees(ProtoContext* c, Shape& s, Sub R, unsigned hr) {
            const Kids v = merge(c, {s.tree, s.treeSize}, s.h, R, hr);
            const unsigned h = std::max(s.h, hr);
            if (v.n == 1) { s.tree = v.v[0].ref; s.treeSize = v.v[0].size; s.h = h; return; }
            const Sub top = mkInner(c, v.v, v.n, h + 1);
            s.tree = top.ref; s.treeSize = top.size; s.h = h + 1;
        }
        // The tree grows by `leaf` at its right edge (full or not).
        void pushLeaf(ProtoContext* c, Shape& s, Sub leaf) {
            if (s.treeSize == 0) { s.tree = leaf.ref; s.h = 0; s.treeSize = leaf.size; return; }
            if (leaf.size == M) {
                const Sub t = push(c, {s.tree, s.treeSize}, s.h, leaf);
                if (t.ref) { s.tree = t.ref; s.treeSize = t.size; return; }
                const Sub two[2] = {{s.tree, s.treeSize}, path(c, s.h, leaf)};
                const Sub t2 = mkInner(c, two, 2, s.h + 1);
                s.tree = t2.ref; s.treeSize = t2.size; s.h += 1;
                return;
            }
            concatTrees(c, s, leaf, 0);
        }
        void shrink(Shape& s) {
            while (s.treeSize && s.h > 0) {
                const Kids k = children({s.tree, s.treeSize}, s.h);
                if (k.n != 1) break;
                s.tree = k.v[0].ref; s.h -= 1;
            }
            if (s.treeSize == 0) { s.tree = 0; s.h = 0; }
        }
        // The tree without its last leaf, and that leaf.
        void popLast(ProtoContext* c, Sub s, unsigned h, Sub& rest, Sub& leaf) {
            if (h == 0) { rest = {0, 0}; leaf = s; return; }
            Kids k = children(s, h);
            Sub r;
            popLast(c, k.v[k.n - 1], h - 1, r, leaf);
            if (r.size == 0) --k.n; else k.v[k.n - 1] = r;
            rest = k.n == 0 ? Sub{0, 0} : mkInner(c, k.v, k.n, h);
        }
        void treeToTail(ProtoContext* c, Shape& s) {   // the tree's last leaf becomes the (empty) tail
            Sub rest, leaf;
            popLast(c, {s.tree, s.treeSize}, s.h, rest, leaf);
            s.tree = rest.ref; s.treeSize = rest.size;
            s.tail = leafOf(leaf.ref); s.tailLen = static_cast<unsigned>(leaf.size);
            shrink(s);
        }
        Sub setIn(ProtoContext* c, Sub s, unsigned h, proto_ulong i, const ProtoObject* v) {
            if (h == 0) {
                const ProtoObject* b[M];
                for (unsigned j = 0; j < M; ++j) b[j] = leafOf(s.ref)->slots[j];
                b[i] = v;
                return mkLeaf(c, b, static_cast<unsigned>(s.size));
            }
            Kids k = children(s, h);
            unsigned j = 0;
            while (i >= k.v[j].size) { i -= k.v[j].size; ++j; }
            k.v[j] = setIn(c, k.v[j], h - 1, i, v);
            return mkInner(c, k.v, k.n, h);
        }

        // ------------------------------------------------------------ slicing
        Sub takeTree(ProtoContext* c, Sub s, unsigned h, proto_ulong k) {   // first k; k on a leaf boundary
            Kids kd = children(s, h); proto_ulong start = 0; unsigned j = 0;
            while (start + kd.v[j].size < k) { start += kd.v[j].size; ++j; }
            kd.n = j + 1;
            if (k - start < kd.v[j].size) kd.v[j] = takeTree(c, kd.v[j], h - 1, k - start);
            return mkInner(c, kd.v, kd.n, h);
        }
        Sub dropTree(ProtoContext* c, Sub s, unsigned h, proto_ulong k) {   // without the first k
            if (h == 0) return mkLeaf(c, leafOf(s.ref)->slots + k, static_cast<unsigned>(s.size - k));
            const Kids kd = children(s, h); proto_ulong start = 0; unsigned j = 0;
            while (start + kd.v[j].size <= k) { start += kd.v[j].size; ++j; }
            Kids out;
            out.push(k > start ? dropTree(c, kd.v[j], h - 1, k - start) : kd.v[j]);
            for (unsigned i = j + 1; i < kd.n; ++i) out.push(kd.v[i]);
            return mkInner(c, out.v, out.n, h);
        }

        // ------------------------------------------------------------ insertion and removal
        Kids splitInto(ProtoContext* c, const Sub* all, unsigned n, unsigned h, unsigned at) {   // n == M + 1
            const unsigned l = at == 0 ? 1 : at == n - 1 ? n - 1 : (n + 1) / 2;
            Kids out;
            out.push(mkInner(c, all, l, h));
            out.push(mkInner(c, all + l, n - l, h));
            return out;
        }
        Kids insTree(ProtoContext* c, Sub s, unsigned h, proto_ulong i, const ProtoObject* v) {
            Kids out;
            if (h == 0) {
                const Leaf* lf = leafOf(s.ref); const unsigned len = static_cast<unsigned>(s.size);
                const ProtoObject* b[M + 1];
                for (unsigned j = 0, o = 0; j <= len; ++j) b[j] = (j == i) ? v : lf->slots[o++];
                if (len < M) { out.push(mkLeaf(c, b, len + 1)); return out; }
                // Lean away from an insertion at either end: runs of prepends
                // or of inserts at one place leave full leaves behind them.
                const unsigned l = i == 0 ? 1 : i == len ? len : (len + 2) / 2;
                out.push(mkLeaf(c, b, l));
                out.push(mkLeaf(c, b + l, len + 1 - l));
                return out;
            }
            Kids k = children(s, h); unsigned j = 0; proto_ulong start = 0;
            while (j + 1 < k.n && i >= start + k.v[j].size) { start += k.v[j].size; ++j; }
            const Kids r = insTree(c, k.v[j], h - 1, i - start, v);
            if (r.n == 2) {
                // B*: the emptier neighbour takes part of the split when the
                // three fit in two nodes, and this node gains no child.
                int best = -1; unsigned bestSlots = 2 * M + 1;
                for (int nb : {static_cast<int>(j) + 1, static_cast<int>(j) - 1}) {
                    if (nb < 0 || nb >= static_cast<int>(k.n)) continue;
                    const unsigned sl = slotCount(k.v[nb], h - 1);
                    if (sl < bestSlots) { bestSlots = sl; best = nb; }
                }
                if (best >= 0) {
                    Kids three;
                    if (best < static_cast<int>(j)) { three.push(k.v[best]); three.push(r.v[0]); three.push(r.v[1]); }
                    else { three.push(r.v[0]); three.push(r.v[1]); three.push(k.v[best]); }
                    const unsigned total = slotCount(three.v[0], h - 1) + slotCount(three.v[1], h - 1)
                                         + slotCount(three.v[2], h - 1);
                    if (total <= 2 * M) {
                        const unsigned plan[2] = {(total + 1) / 2, total / 2};
                        const Kids two = cut(c, three, h - 1, plan, 2);
                        const unsigned first = static_cast<unsigned>(std::min(best, static_cast<int>(j)));
                        k.v[first] = two.v[0]; k.v[first + 1] = two.v[1];
                        out.push(mkInner(c, k.v, k.n, h));
                        return out;
                    }
                }
            }
            Kids all;
            for (unsigned q = 0; q < j; ++q) all.push(k.v[q]);
            for (unsigned q = 0; q < r.n; ++q) all.push(r.v[q]);
            for (unsigned q = j + 1; q < k.n; ++q) all.push(k.v[q]);
            if (all.n <= M) { out.push(mkInner(c, all.v, all.n, h)); return out; }
            const unsigned at = (r.v[0].size <= r.v[1].size) ? j : j + 1;
            return splitInto(c, all.v, all.n, h, at);
        }
        Sub mergeNodes(ProtoContext* c, Sub a, Sub b, unsigned h) {   // neighbours that fit in one node
            if (h == 0) {
                const ProtoObject* buf[M];
                for (proto_ulong i = 0; i < a.size; ++i) buf[i] = leafOf(a.ref)->slots[i];
                for (proto_ulong i = 0; i < b.size; ++i) buf[a.size + i] = leafOf(b.ref)->slots[i];
                return mkLeaf(c, buf, static_cast<unsigned>(a.size + b.size));
            }
            Kids ka = children(a, h);
            const Kids kb = children(b, h);
            for (unsigned q = 0; q < kb.n; ++q) ka.push(kb.v[q]);
            return mkInner(c, ka.v, ka.n, h);
        }
        Sub remTree(ProtoContext* c, Sub s, unsigned h, proto_ulong i) {   // ref 0 when the subtree empties
            if (h == 0) {
                if (s.size == 1) return {0, 0};
                const ProtoObject* b[M]; const Leaf* lf = leafOf(s.ref);
                for (unsigned j = 0, o = 0; j < s.size; ++j) if (j != i) b[o++] = lf->slots[j];
                return mkLeaf(c, b, static_cast<unsigned>(s.size - 1));
            }
            Kids k = children(s, h); unsigned j = 0; proto_ulong start = 0;
            while (i >= start + k.v[j].size) { start += k.v[j].size; ++j; }
            const Sub r = remTree(c, k.v[j], h - 1, i - start);
            if (!r.ref) {
                for (unsigned q = j; q + 1 < k.n; ++q) k.v[q] = k.v[q + 1];
                --k.n;
            } else {
                k.v[j] = r;
                if (slotCount(r, h - 1) <= M / 2) {
                    for (int nb : {static_cast<int>(j) - 1, static_cast<int>(j) + 1}) {
                        if (nb < 0 || nb >= static_cast<int>(k.n)) continue;
                        const unsigned a = std::min(j, static_cast<unsigned>(nb)), b = a + 1;
                        if (slotCount(k.v[a], h - 1) + slotCount(k.v[b], h - 1) > M) continue;
                        k.v[a] = mergeNodes(c, k.v[a], k.v[b], h - 1);
                        for (unsigned q = b; q + 1 < k.n; ++q) k.v[q] = k.v[q + 1];
                        --k.n;
                        break;
                    }
                }
            }
            if (k.n == 0) return {0, 0};
            return mkInner(c, k.v, k.n, h);
        }
    }  // namespace

    //=========================================================================
    // Public engine API
    //=========================================================================

    const Root* fromArray(ProtoContext* c, const ProtoObject* const* items, proto_ulong n) {
        if (n == 0) return nullptr;
        // The shape a run of appends produces: full leaves under strict
        // nodes, the last 1..6 elements in the tail.
        const proto_ulong tailLen = n - ((n - 1) / M) * M;
        const proto_ulong off = n - tailLen;
        Shape s;
        s.tail = newLeaf(c, items + off, static_cast<unsigned>(tailLen));
        s.tailLen = static_cast<unsigned>(tailLen);
        if (off > 0) {
            std::vector<Sub> level;
            level.reserve(off / M);
            for (proto_ulong i = 0; i < off; i += M) level.push_back(mkLeaf(c, items + i, M));
            unsigned h = 0;
            while (level.size() > 1) {
                std::vector<Sub> up;
                up.reserve((level.size() + M - 1) / M);
                for (std::size_t i = 0; i < level.size(); i += M) {
                    const unsigned k = static_cast<unsigned>(std::min<std::size_t>(M, level.size() - i));
                    up.push_back(mkInner(c, level.data() + i, k, h + 1));
                }
                level.swap(up);
                ++h;
            }
            s.tree = level[0].ref; s.treeSize = off; s.h = h;
        }
        return make(c, s);
    }

    const ProtoObject* getAt(const Root* r, proto_ulong i) {
        if (i >= r->treeSize) return r->tail->slots[i - r->treeSize];
        return getTree(r->tree, r->height, i);
    }

    const Leaf* locate(const Root* r, proto_ulong i, proto_ulong& start, proto_ulong& end) {
        if (i >= r->treeSize) { start = r->treeSize; end = r->size; return r->tail; }
        const Sub lf = locateIn({r->tree, r->treeSize}, r->height, i, start);
        end = start + lf.size;
        return leafOf(lf.ref);
    }

    bool forEach(const Root* r, bool (*fn)(void*, const ProtoObject*), void* data) {
        if (r->treeSize && walk({r->tree, r->treeSize}, r->height, fn, data)) return true;
        for (unsigned i = 0; i < r->tailLen; ++i) if (fn(data, r->tail->slots[i])) return true;
        return false;
    }

    proto_ulong toBuffer(const Root* r, const ProtoObject** out) {
        struct Acc { const ProtoObject** out; proto_ulong at; } acc{out, 0};
        forEach(r, [](void* d, const ProtoObject* e) {
            auto* a = static_cast<Acc*>(d); a->out[a->at++] = e; return false;
        }, &acc);
        return acc.at;
    }

    const Root* setAt(ProtoContext* c, const Root* r, proto_ulong i, const ProtoObject* v) {
        Shape s = shapeOf(r);
        if (i >= r->treeSize) {
            const ProtoObject* b[M];
            for (unsigned j = 0; j < M; ++j) b[j] = r->tail->slots[j];
            b[i - r->treeSize] = v;
            s.tail = newLeaf(c, b, r->tailLen);
        } else {
            s.tree = setIn(c, {r->tree, r->treeSize}, r->height, i, v).ref;
        }
        return make(c, s);
    }

    const Root* appendLast(ProtoContext* c, const Root* r, const ProtoObject* v) {
        if (!r) { Shape s; s.tail = newLeaf(c, &v, 1); s.tailLen = 1; return make(c, s); }
        Shape s = shapeOf(r);
        if (r->tailLen < M) {
            const ProtoObject* b[M];
            for (unsigned j = 0; j < r->tailLen; ++j) b[j] = r->tail->slots[j];
            b[r->tailLen] = v;
            s.tail = newLeaf(c, b, r->tailLen + 1u); s.tailLen = r->tailLen + 1u;
            return make(c, s);
        }
        pushLeaf(c, s, {reinterpret_cast<uintptr_t>(r->tail), M});
        s.tail = newLeaf(c, &v, 1); s.tailLen = 1;
        return make(c, s);
    }

    const Root* removeLast(ProtoContext* c, const Root* r) {
        if (r->size <= 1) return nullptr;
        Shape s = shapeOf(r);
        if (r->tailLen > 1) {
            s.tail = newLeaf(c, r->tail->slots, r->tailLen - 1u); s.tailLen = r->tailLen - 1u;
            return make(c, s);
        }
        treeToTail(c, s);
        return make(c, s);
    }

    const Root* concat(ProtoContext* c, const Root* a, const Root* b) {
        if (!a) return b;
        if (!b) return a;
        Shape s = shapeOf(a);
        if (b->treeSize == 0) {
            // A short right operand joins a's tail: one tail, or a full leaf
            // pushed (as appends would) and the rest as the new tail.  The
            // tree is not relaxed by a partial leaf.
            const ProtoObject* buf[2 * M];
            const unsigned n = a->tailLen + b->tailLen;
            for (unsigned j = 0; j < a->tailLen; ++j) buf[j] = a->tail->slots[j];
            for (unsigned j = 0; j < b->tailLen; ++j) buf[a->tailLen + j] = b->tail->slots[j];
            if (n <= M) { s.tail = newLeaf(c, buf, n); s.tailLen = n; return make(c, s); }
            pushLeaf(c, s, mkLeaf(c, buf, M));
            s.tail = newLeaf(c, buf + M, n - M); s.tailLen = n - M;
            return make(c, s);
        }
        pushLeaf(c, s, {reinterpret_cast<uintptr_t>(a->tail), a->tailLen});
        if (b->treeSize) concatTrees(c, s, {b->tree, b->treeSize}, b->height);
        s.tail = b->tail; s.tailLen = b->tailLen;
        shrink(s);
        return make(c, s);
    }

    const Root* take(ProtoContext* c, const Root* r, proto_ulong k) {
        if (k >= r->size) return r;
        if (k == 0) return nullptr;
        Shape s = shapeOf(r);
        if (k > r->treeSize) {
            s.tail = newLeaf(c, r->tail->slots, static_cast<unsigned>(k - r->treeSize));
            s.tailLen = static_cast<unsigned>(k - r->treeSize);
            return make(c, s);
        }
        if (k == r->treeSize) { treeToTail(c, s); return make(c, s); }
        proto_ulong leafStart;
        const Sub lf = locateIn({r->tree, r->treeSize}, r->height, k - 1, leafStart);
        s.tail = k - leafStart == lf.size ? leafOf(lf.ref)
                                          : newLeaf(c, leafOf(lf.ref)->slots, static_cast<unsigned>(k - leafStart));
        s.tailLen = static_cast<unsigned>(k - leafStart);
        if (leafStart == 0) { s.tree = 0; s.treeSize = 0; s.h = 0; return make(c, s); }
        const Sub t = takeTree(c, {r->tree, r->treeSize}, r->height, leafStart);
        s.tree = t.ref; s.treeSize = t.size;
        shrink(s);
        return make(c, s);
    }

    const Root* drop(ProtoContext* c, const Root* r, proto_ulong k) {
        if (k == 0) return r;
        if (k >= r->size) return nullptr;
        Shape s = shapeOf(r);
        if (k >= r->treeSize) {
            s.tree = 0; s.treeSize = 0; s.h = 0;
            s.tail = newLeaf(c, r->tail->slots + (k - r->treeSize), static_cast<unsigned>(r->size - k));
            s.tailLen = static_cast<unsigned>(r->size - k);
            return make(c, s);
        }
        const Sub t = dropTree(c, {r->tree, r->treeSize}, r->height, k);
        s.tree = t.ref; s.treeSize = t.size;
        shrink(s);
        return make(c, s);
    }

    const Root* insertAt(ProtoContext* c, const Root* r, proto_ulong i, const ProtoObject* v) {
        if (!r || i >= r->size) return appendLast(c, r, v);
        Shape s = shapeOf(r);
        if (i >= r->treeSize) {                                   // inside the tail
            const unsigned ti = static_cast<unsigned>(i - r->treeSize), len = r->tailLen;
            const ProtoObject* b[M + 1];
            for (unsigned j = 0, o = 0; j <= len; ++j) b[j] = (j == ti) ? v : r->tail->slots[o++];
            if (len < M) { s.tail = newLeaf(c, b, len + 1); s.tailLen = len + 1; return make(c, s); }
            pushLeaf(c, s, mkLeaf(c, b, M));
            s.tail = newLeaf(c, b + M, 1); s.tailLen = 1;
            return make(c, s);
        }
        const Kids t = insTree(c, {r->tree, r->treeSize}, r->height, i, v);
        if (t.n == 1) { s.tree = t.v[0].ref; s.treeSize = t.v[0].size; return make(c, s); }
        const Sub top = mkInner(c, t.v, 2, r->height + 1u);
        s.tree = top.ref; s.treeSize = top.size; s.h = r->height + 1u;
        return make(c, s);
    }

    const Root* removeAt(ProtoContext* c, const Root* r, proto_ulong i) {
        if (i + 1 >= r->size) return removeLast(c, r);
        Shape s = shapeOf(r);
        if (i >= r->treeSize) {
            const ProtoObject* b[M]; const unsigned ti = static_cast<unsigned>(i - r->treeSize);
            for (unsigned j = 0, o = 0; j < r->tailLen; ++j) if (j != ti) b[o++] = r->tail->slots[j];
            s.tail = newLeaf(c, b, r->tailLen - 1u); s.tailLen = r->tailLen - 1u;
            return make(c, s);
        }
        const Sub t = remTree(c, {r->tree, r->treeSize}, r->height, i);
        s.tree = t.ref; s.treeSize = t.size;
        shrink(s);
        return make(c, s);
    }

}  // namespace rrb
}  // namespace proto
