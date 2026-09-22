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
}
