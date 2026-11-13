// ListRrbCheck.h — structural invariants of ProtoList's RRB form
// (core/ProtoListRrb.cpp), shared by the list tests.  An empty string means
// the list is well formed.
#pragma once

#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <string>

namespace proto_test {

using proto::Cell;
using proto::CellType;
using proto::ProtoList;
using proto::ProtoListRrbImplementation;
using proto::ProtoListRrbInner;
using proto::ProtoListRrbLeaf;
using proto::ProtoListRrbSizes;
using proto::ProtoObjectPointer;
using proto::proto_ulong;

// The RRB root behind a list handle, or nullptr when the list is in another form.
inline const ProtoListRrbImplementation* rrbOf(const ProtoList* l) {
    ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const proto::ProtoObject*>(l);
    if (pa.op.pointer_tag != POINTER_TAG_LIST) return nullptr;
    const Cell* c = reinterpret_cast<const Cell*>(reinterpret_cast<uintptr_t>(l) & ~static_cast<uintptr_t>(0x3F));
    return c->getType() == CellType::ListRrb ? static_cast<const ProtoListRrbImplementation*>(c) : nullptr;
}

inline bool isRrb(const ProtoList* l) { return rrbOf(l) != nullptr; }

struct RrbStats { unsigned maxExtraSteps = 0; unsigned relaxedNodes = 0; };

namespace detail {
constexpr unsigned W = ProtoListRrbLeaf::WIDTH;
inline proto_ulong cap(unsigned h) { proto_ulong p = W; while (h--) p *= W; return p; }

// Checks the subtree `ref` of height h holding `size` elements; returns "" or why not.
inline std::string checkTree(uintptr_t ref, unsigned h, proto_ulong size, RrbStats& st, const std::string& at) {
    if (!ref) return at + ": null child";
    const Cell* cell = reinterpret_cast<const Cell*>(ref & ~static_cast<uintptr_t>(1));
    if (h == 0) {
        if (ref & 1) return at + ": relaxed mark on a leaf";
        if (cell->getType() != CellType::ListRrbLeaf) return at + ": leaf of the wrong cell type";
        if (size < 1 || size > W) return at + ": leaf size " + std::to_string(size);
        const auto* lf = static_cast<const ProtoListRrbLeaf*>(cell);
        for (proto_ulong i = size; i < W; ++i) if (lf->slots[i]) return at + ": leaf slot past its length";
        return "";
    }
    if (size > cap(h)) return at + ": subtree over capacity";
    if (ref & 1) {
        if (cell->getType() != CellType::ListRrbSizes) return at + ": relaxed node of the wrong cell type";
        const auto* r = static_cast<const ProtoListRrbSizes*>(cell);
        ++st.relaxedNodes;
        if (r->count < 1 || r->count > W) return at + ": relaxed count";
        if (!r->kids || r->kids->getType() != CellType::ListRrbInner) return at + ": relaxed node without children";
        proto_ulong prev = 0;
        for (unsigned i = 0; i < r->count; ++i) {
            const proto_ulong c = r->cumAt(i);
            if (c <= prev) return at + ": size table not increasing";
            if (c - prev > cap(h - 1)) return at + ": child over capacity";
            const std::string w = checkTree(r->kids->slots[i], h - 1, c - prev, st, at + "/" + std::to_string(i));
            if (!w.empty()) return w;
            prev = c;
        }
        if (prev != size) return at + ": size table total " + std::to_string(prev) + " vs " + std::to_string(size);
        for (unsigned i = r->count; i < W; ++i) if (r->kids->slots[i]) return at + ": child slot past count";
        for (proto_ulong probe : {proto_ulong(0), size / 2, size - 1}) {
            proto_ulong q = probe; for (unsigned l = 0; l < h; ++l) q /= W;
            unsigned k = static_cast<unsigned>(q), steps = 0;
            while (r->cumAt(k) <= probe) { ++k; ++steps; }
            if (steps > st.maxExtraSteps) st.maxExtraSteps = steps;
        }
        return "";
    }
    if (cell->getType() != CellType::ListRrbInner) return at + ": strict node of the wrong cell type";
    if (size % W) return at + ": strict node over a partial leaf";
    const auto* in = static_cast<const ProtoListRrbInner*>(cell);
    const proto_ulong c = cap(h - 1);
    const unsigned n = static_cast<unsigned>((size + c - 1) / c);
    for (unsigned i = 0; i < W; ++i) if ((i < n) != (in->slots[i] != 0)) return at + ": strict slots";
    for (unsigned i = 0; i < n; ++i) {
        if (in->slots[i] & 1) return at + ": relaxed child under a strict node";
        const proto_ulong sz = i + 1 < n ? c : size - c * (n - 1);
        const std::string w = checkTree(in->slots[i], h - 1, sz, st, at + "/" + std::to_string(i));
        if (!w.empty()) return w;
    }
    return "";
}
}  // namespace detail

inline std::string checkRrb(const ProtoListRrbImplementation* r, RrbStats* stats = nullptr) {
    RrbStats local; RrbStats& st = stats ? *stats : local;
    if (r->size != r->treeSize + r->tailLen) return "size != treeSize + tailLen";
    if (r->size <= proto::ProtoListSmallImplementation::MAX_INLINE) return "RRB list no larger than MAX_INLINE";
    if (r->tailLen < 1 || r->tailLen > detail::W) return "tail length out of 1..6";
    if (!r->tail || r->tail->getType() != CellType::ListRrbLeaf) return "tail is not a leaf";
    for (unsigned i = r->tailLen; i < detail::W; ++i) if (r->tail->slots[i]) return "tail slot past tailLen";
    if (r->avlHeight != 0 || r->avlIsEmpty != 0 || r->reserved != 0) return "layout-contract bytes not zero";
    if (r->treeSize == 0) return r->tree ? "tree without elements" : "";
    if (r->height > 0) {
        // the root never has a single child
        unsigned kids;
        if (r->tree & 1) kids = reinterpret_cast<const ProtoListRrbSizes*>(r->tree & ~static_cast<uintptr_t>(1))->count;
        else { const proto_ulong c = detail::cap(r->height - 1u); kids = static_cast<unsigned>((r->treeSize + c - 1) / c); }
        if (kids < 2) return "root with a single child";
    }
    return detail::checkTree(r->tree, r->height, r->treeSize, st, "tree");
}

}  // namespace proto_test
