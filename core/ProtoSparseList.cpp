/*
 * ProtoSparseList.cpp
 *
 *  Created on: 2017-05-01
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include "SparseListAlgorithms.h"
#include <algorithm> // For std::max

namespace proto
{
    //=========================================================================
    // ProtoSparseListIteratorImplementation
    //=========================================================================
    ProtoSparseListIteratorImplementation::ProtoSparseListIteratorImplementation(ProtoContext* context, int s, const ProtoSparseListImplementation* c, const ProtoSparseListIteratorImplementation* q)
        : Cell(context), state(s), current(c), queue(q) {}

    int ProtoSparseListIteratorImplementation::implHasNext() const {
        return state == ITERATOR_NEXT_THIS && current && !current->isEmpty;
    }

    unsigned long ProtoSparseListIteratorImplementation::implNextKey() const {
        return (state == ITERATOR_NEXT_THIS && current) ? current->key : 0;
    }

    const ProtoObject* ProtoSparseListIteratorImplementation::implNextValue() const {
        return (state == ITERATOR_NEXT_THIS && current) ? current->value : nullptr;
    }

    const ProtoSparseListIteratorImplementation* ProtoSparseListIteratorImplementation::implAdvance(ProtoContext* context) const {
        if (state == ITERATOR_NEXT_THIS) {
            // After yielding 'current', we should descend into 'current->next' (if any)
            // and then continue with the 'queue'.
            if (current && current->next && !current->next->isEmpty) {
                return current->next->implGetIteratorWithQueue(context, queue);
            }
            return queue;
        }
        return nullptr;
    }

    const ProtoObject* ProtoSparseListIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.voidPointer = const_cast<ProtoSparseListIteratorImplementation*>(this);
        p.op.pointer_tag = POINTER_TAG_SPARSE_LIST_ITERATOR;
        return p.oid;
    }

    void ProtoSparseListIteratorImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (current) {
            method(context, self, current);
        }
        if (queue) {
            method(context, self, queue);
        }
    }

    //=========================================================================
    // ProtoSparseListImplementation
    //=========================================================================
    ProtoSparseListImplementation::ProtoSparseListImplementation(ProtoContext* context, unsigned long k, const ProtoObject* v, const ProtoSparseListImplementation* p, const ProtoSparseListImplementation* n, bool empty)
        : Cell(context), key(k), value(v), previous(p), next(n),
          // P8 — `hash` was only used to propagate up the tree during
          // construction; it is never read externally and never queried
          // for SparseList equality.  Computing it required a virtual
          // `v->getHash(context)` per node (which inside ProtoObject::getHash
          // triggers the isString chain probe — 3.78 % of bench CPU
          // before this change).  Set to 0; the field is retained for
          // ABI / cell-layout stability but no longer drives a virtual.
          hash(0),
          size(empty ? 0 : (v != nullptr) + sparse_avl::nodeSize(p) + sparse_avl::nodeSize(n)),
          height(empty ? 0 : 1 + std::max(sparse_avl::nodeHeight(p), sparse_avl::nodeHeight(n))),
          isEmpty(empty) {}

    bool ProtoSparseListImplementation::implHas(ProtoContext* context, unsigned long offset) const {
        return implGetAt(context, offset) != nullptr;
    }

    const ProtoObject* ProtoSparseListImplementation::implGetAt(ProtoContext*, unsigned long offset) const {
        return sparse_avl::getAt(this, offset);
    }

    const ProtoSparseListImplementation* ProtoSparseListImplementation::implSetAt(ProtoContext* context, unsigned long offset, const ProtoObject* newValue) const {
        return sparse_avl::setAt(context, this, offset, newValue);
    }

    // Kept as an out-of-line function: it has external linkage today and
    // removing the symbol is not part of this change.
    const ProtoSparseListImplementation* findMin(const ProtoSparseListImplementation* node) {
        return sparse_avl::findMin(node);
    }

    const ProtoSparseListImplementation* ProtoSparseListImplementation::implRemoveAt(ProtoContext* context, unsigned long offset) const {
        return sparse_avl::removeAt(context, this, offset);
    }


    const ProtoSparseListIteratorImplementation* ProtoSparseListImplementation::implGetIterator(ProtoContext* context) const {
        return implGetIteratorWithQueue(context, nullptr);
    }

    const ProtoSparseListIteratorImplementation* ProtoSparseListImplementation::implGetIteratorWithQueue(ProtoContext* context, const ProtoSparseListIteratorImplementation* queue) const {
        return sparse_avl::iteratorWithQueue(context, this, queue);
    }

    void ProtoSparseListImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (const Cell* c = ProtoObject::asCellPointer(value)) method(context, self, c);
        if (previous) {
            method(context, self, previous);
        }
        if (next) {
            method(context, self, next);
        }
    }

    const ProtoObject* ProtoSparseListImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.voidPointer = const_cast<ProtoSparseListImplementation*>(this);
        p.op.pointer_tag = POINTER_TAG_SPARSE_LIST;
        return p.oid;
    }

    const ProtoSparseList* ProtoSparseListImplementation::asSparseList(ProtoContext* context) const {
        return reinterpret_cast<const ProtoSparseList*>(implAsObject(context));
    }

    //=========================================================================
    // ProtoSparseListSmallImplementation
    //=========================================================================
    //
    // Single-cell inline sparse list with up to MAX_INLINE = 3 (key, value)
    // pairs.  Used by ProtoContext::newSparseList() so every fresh sparse
    // list starts as a Small; setAt promotes to the AVL form when the
    // result would exceed MAX_INLINE.  removeAt does NOT degrade an AVL
    // back to Small — keeping AVL is fine, the asymmetry simplifies the
    // hot path.
    //
    // **Empty-slot sentinel: keys[i] == 0.**  Reading a slot is a single
    // compare on the key array.  Storing key 0 is therefore not
    // representable in this form: setAt(offset=0, ...) promotes the entire
    // list to the AVL form, which has no such reservation.  The AVL form
    // can store key 0 transparently; only the inline form bans it.
    //
    // values[i] is also kept nullptr in unused slots so that
    // processReferences can skip them safely without re-reading the key.
    //
    // Slots are NOT kept dense after removeAt — implCount() does a
    // 3-element linear scan over keys to ignore cleared slots.  All public
    // reads / writes preserve key-ascending order among used slots so
    // iterator output matches the AVL form bit-for-bit.

    ProtoSparseListSmallImplementation::ProtoSparseListSmallImplementation(ProtoContext* context)
        : Cell(context)
    {
        for (unsigned i = 0; i < MAX_INLINE; ++i) {
            keys[i] = 0;            // 0 = empty-slot sentinel
            values[i] = nullptr;
        }
    }

    ProtoSparseListSmallImplementation::ProtoSparseListSmallImplementation(
        ProtoContext* context, unsigned n,
        const unsigned long* ks, const ProtoObject* const* vs
    ) : Cell(context)
    {
        // Caller guarantees n ≤ MAX_INLINE, no zero key in the n-prefix
        // (zero-key entries must go via the AVL form), and key-asc ordering
        // of the n-prefix.  Unused slots are zeroed.
        for (unsigned i = 0; i < MAX_INLINE; ++i) {
            if (i < n) {
                keys[i] = ks[i];
                values[i] = vs[i];
            } else {
                keys[i] = 0;
                values[i] = nullptr;
            }
        }
    }

    unsigned long ProtoSparseListSmallImplementation::implCount() const {
        return sparse_avl::smallCount(this);
    }

    bool ProtoSparseListSmallImplementation::implHas(ProtoContext*, unsigned long offset) const {
        return sparse_avl::smallHas(this, offset);
    }

    const ProtoObject* ProtoSparseListSmallImplementation::implGetAt(ProtoContext*, unsigned long offset) const {
        return sparse_avl::smallGetAt(this, offset);
    }

    bool ProtoSparseListSmallImplementation::implPairAt(unsigned i, unsigned long* outKey, const ProtoObject** outValue) const {
        return sparse_avl::smallPairAt(this, i, outKey, outValue);
    }

    const ProtoSparseListImplementation*
    ProtoSparseListSmallImplementation::promoteToAVL(ProtoContext* context) const {
        return sparse_avl::smallPromote<ProtoSparseListSmallImplementation, ProtoSparseListImplementation>(context, this);
    }

    const ProtoObject* ProtoSparseListSmallImplementation::implAsObject(ProtoContext*) const {
        ProtoObjectPointer p{};
        p.sparseListSmallImplementation = this;
        p.op.pointer_tag = POINTER_TAG_SPARSE_LIST_SMALL;
        return p.oid;
    }

    const ProtoSparseList* ProtoSparseListSmallImplementation::asSparseList(ProtoContext* context) const {
        return reinterpret_cast<const ProtoSparseList*>(implAsObject(context));
    }

    void ProtoSparseListSmallImplementation::processReferences(
        ProtoContext* context, void* self,
        void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        for (unsigned i = 0; i < MAX_INLINE; ++i) {
            // keys[i] == 0 marks an empty slot; values[i] is also nullptr
            // there per the construction contract.  Skip empties.
            if (keys[i] == 0) continue;
            if (const Cell* c = ProtoObject::asCellPointer(values[i])) {
                method(context, self, c);
            }
        }
    }

    //=========================================================================
    // Tag-dispatch helpers (file-local) for the public trampolines
    //=========================================================================
    namespace {
        inline bool isSparseListSmall(const ProtoSparseList* sl) {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(sl);
            return pa.op.pointer_tag == POINTER_TAG_SPARSE_LIST_SMALL;
        }

        // setAt on a Small.  Returns a fresh Small (size <= MAX_INLINE) or
        // promotes to the AVL form when the resulting size would exceed
        // MAX_INLINE or offset == 0 (the Small's empty-slot sentinel).
        // Caller wraps in CriticalSection.
        const ProtoSparseList* setAtSmall(
            ProtoContext* context,
            const ProtoSparseListSmallImplementation* small,
            unsigned long offset,
            const ProtoObject* value)
        {
            return reinterpret_cast<const ProtoSparseList*>(
                sparse_avl::smallSetAt<ProtoSparseListSmallImplementation, ProtoSparseListImplementation>(
                    context, small, offset, value));
        }

        // removeAt on a Small.  Stays Small (size only shrinks).  Caller
        // wraps in CriticalSection.
        const ProtoSparseList* removeAtSmall(
            ProtoContext* context,
            const ProtoSparseListSmallImplementation* small,
            unsigned long offset)
        {
            return setAtSmall(context, small, offset, nullptr);
        }

        // Return the size (used-slot count) regardless of form.
        inline unsigned long sparseListSize(const ProtoSparseList* sl) {
            if (!sl) return 0;
            if (isSparseListSmall(sl)) {
                return toImpl<const ProtoSparseListSmallImplementation>(sl)->implCount();
            }
            return toImpl<const ProtoSparseListImplementation>(sl)->size;
        }
    } // anonymous namespace

    // ProtoSparseList / ProtoSparseListIterator external API trampolines
    bool ProtoSparseList::has(ProtoContext* context, unsigned long offset) const {
        if (isSparseListSmall(this)) {
            return toImpl<const ProtoSparseListSmallImplementation>(this)->implHas(context, offset);
        }
        return toImpl<const ProtoSparseListImplementation>(this)->implHas(context, offset);
    }
    const ProtoObject* ProtoSparseList::getAt(ProtoContext* context, unsigned long offset) const {
        const ProtoObject* result;
        if (isSparseListSmall(this)) {
            result = toImpl<const ProtoSparseListSmallImplementation>(this)->implGetAt(context, offset);
        } else {
            result = toImpl<const ProtoSparseListImplementation>(this)->implGetAt(context, offset);
        }
        return result ? result : PROTO_NONE;
    }
    const ProtoSparseList* ProtoSparseList::setAt(ProtoContext* context, unsigned long offset, const ProtoObject* value) const {
        // GC critical section: setAtSmall / implSetAt may build several
        // new cells; the result is reachable only via this C++ frame's
        // return value until the caller publishes it.  Same discipline as
        // ProtoList::setAt and ProtoObject::setAttribute.
        ProtoContext::CriticalSection cs(context);
        if (isSparseListSmall(this)) {
            return setAtSmall(context, toImpl<const ProtoSparseListSmallImplementation>(this), offset, value);
        }
        return toImpl<const ProtoSparseListImplementation>(this)->implSetAt(context, offset, value)->asSparseList(context);
    }
    const ProtoSparseList* ProtoSparseList::removeAt(ProtoContext* context, unsigned long offset) const {
        ProtoContext::CriticalSection cs(context);
        if (isSparseListSmall(this)) {
            return removeAtSmall(context, toImpl<const ProtoSparseListSmallImplementation>(this), offset);
        }
        return toImpl<const ProtoSparseListImplementation>(this)->implRemoveAt(context, offset)->asSparseList(context);
    }
    unsigned long ProtoSparseList::getSize(ProtoContext* context) const {
        return sparseListSize(this);
    }
    const ProtoObject* ProtoSparseList::asObject(ProtoContext* context) const {
        if (isSparseListSmall(this)) {
            return toImpl<const ProtoSparseListSmallImplementation>(this)->implAsObject(context);
        }
        return toImpl<const ProtoSparseListImplementation>(this)->implAsObject(context);
    }
    const ProtoSparseListIterator* ProtoSparseList::getIterator(ProtoContext* context) const {
        // For Small we promote to AVL once and reuse the existing iterator.
        // This matches the closure-cell hot path: writes dominate; reads /
        // iterations on the Small form are rare and short.
        ProtoContext::CriticalSection cs(context);
        const ProtoSparseListIteratorImplementation* impl;
        if (isSparseListSmall(this)) {
            const auto* avl = toImpl<const ProtoSparseListSmallImplementation>(this)->promoteToAVL(context);
            impl = avl->implGetIterator(context);
        } else {
            impl = toImpl<const ProtoSparseListImplementation>(this)->implGetIterator(context);
        }
        return impl ? reinterpret_cast<const ProtoSparseListIterator*>(impl->implAsObject(context)) : nullptr;
    }

    void ProtoSparseList::processElements(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, unsigned long, const ProtoObject*)) const {
        if (isSparseListSmall(this)) {
            // Small fast path: walk the inline pairs in key-asc order
            // without allocating an iterator chain.
            const auto* small = toImpl<const ProtoSparseListSmallImplementation>(this);
            unsigned long n = small->implCount();
            for (unsigned i = 0; i < n; ++i) {
                unsigned long k;
                const ProtoObject* v;
                if (small->implPairAt(i, &k, &v)) {
                    method(context, self, k, v);
                }
            }
            return;
        }
        const auto* impl = toImpl<const ProtoSparseListImplementation>(this);
        ProtoContext::CriticalSection cs(context);
        const ProtoSparseListIteratorImplementation* it = impl->implGetIterator(context);
        while (it && it->implHasNext()) {
            method(context, self, it->implNextKey(), it->implNextValue());
            it = it->implAdvance(context);
        }
    }

    int ProtoSparseListIterator::hasNext(ProtoContext* context) const { if (!this) return 0; return toImpl<const ProtoSparseListIteratorImplementation>(this)->implHasNext(); }
    unsigned long ProtoSparseListIterator::nextKey(ProtoContext* context) const { if (!this) return 0; return toImpl<const ProtoSparseListIteratorImplementation>(this)->implNextKey(); }
    const ProtoObject* ProtoSparseListIterator::nextValue(ProtoContext* context) const { if (!this) return nullptr; return toImpl<const ProtoSparseListIteratorImplementation>(this)->implNextValue(); }
    const ProtoSparseListIterator* ProtoSparseListIterator::advance(ProtoContext* context) {
        if (!this) return nullptr;
        const auto* nextImpl = toImpl<const ProtoSparseListIteratorImplementation>(this)->implAdvance(context);
        return nextImpl ? reinterpret_cast<const ProtoSparseListIterator*>(nextImpl->implAsObject(context)) : nullptr;
    }
    const ProtoObject* ProtoSparseListIterator::asObject(ProtoContext* context) const { if (!this) return nullptr; return toImpl<const ProtoSparseListIteratorImplementation>(this)->implAsObject(context); }
}
