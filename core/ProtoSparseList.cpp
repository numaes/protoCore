/*
 * ProtoSparseList.cpp
 *
 *  Created on: 2017-05-01
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
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
    namespace { // Anonymous namespace for file-local helpers

        inline unsigned long get_node_size(const ProtoSparseListImplementation* node) {
            if (!node || (reinterpret_cast<uintptr_t>(node) & 0x3F) != 0) return 0;
            return node->size;
        }

        inline int get_node_height(const ProtoSparseListImplementation* node) {
            if (!node || (reinterpret_cast<uintptr_t>(node) & 0x3F) != 0) return 0;
            return node->height;
        }

        int getBalance(const ProtoSparseListImplementation* node) {
            if (!node || node->isEmpty) return 0;
            return get_node_height(node->previous) - get_node_height(node->next);
        }

        const ProtoSparseListImplementation* rightRotate(ProtoContext* context, const ProtoSparseListImplementation* y) {
            const ProtoSparseListImplementation* x = y->previous;
            const ProtoSparseListImplementation* T2 = x->next;
            auto* new_y = new(context) ProtoSparseListImplementation(context, y->key, y->value, T2, y->next, false);
            return new(context) ProtoSparseListImplementation(context, x->key, x->value, x->previous, new_y, false);
        }

        const ProtoSparseListImplementation* leftRotate(ProtoContext* context, const ProtoSparseListImplementation* x) {
            const ProtoSparseListImplementation* y = x->next;
            const ProtoSparseListImplementation* T2 = y->previous;
            auto* new_x = new(context) ProtoSparseListImplementation(context, x->key, x->value, x->previous, T2, false);
            return new(context) ProtoSparseListImplementation(context, y->key, y->value, new_x, y->next, false);
        }

        const ProtoSparseListImplementation* rebalance(ProtoContext* context, const ProtoSparseListImplementation* node) {
            int balance = getBalance(node);
            if (balance > 1) { // Left heavy
                if (getBalance(node->previous) < 0) { // Left-Right case
                    auto* new_prev = leftRotate(context, node->previous);
                    return rightRotate(context, new(context) ProtoSparseListImplementation(context, node->key, node->value, new_prev, node->next, false));
                }
                // Left-Left case
                return rightRotate(context, node);
            }
            if (balance < -1) { // Right heavy
                if (getBalance(node->next) > 0) { // Right-Left case
                    auto* new_next = rightRotate(context, node->next);
                    return leftRotate(context, new(context) ProtoSparseListImplementation(context, node->key, node->value, node->previous, new_next, false));
                }
                // Right-Right case
                return leftRotate(context, node);
            }
            return node;
        }
    } // end anonymous namespace

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
          size(empty ? 0 : (v != nullptr) + get_node_size(p) + get_node_size(n)),
          height(empty ? 0 : 1 + std::max(get_node_height(p), get_node_height(n))),
          isEmpty(empty) {}

    bool ProtoSparseListImplementation::implHas(ProtoContext* context, unsigned long offset) const {
        return implGetAt(context, offset) != nullptr;
    }

    const ProtoObject* ProtoSparseListImplementation::implGetAt(ProtoContext* context, unsigned long offset) const {
        const auto* node = this;
        while (node) {
            if (node->isEmpty) break;
            if (offset < node->key) node = node->previous;
            else if (offset > node->key) node = node->next;
            else return node->value;
        }
        return nullptr;
    }

    const ProtoSparseListImplementation* ProtoSparseListImplementation::implSetAt(ProtoContext* context, unsigned long offset, const ProtoObject* newValue) const {
        if (newValue == nullptr) {
            return implRemoveAt(context, offset);
        }

        if (isEmpty) {
            return new(context) ProtoSparseListImplementation(context, offset, newValue, nullptr, nullptr, false);
        }

        const ProtoSparseListImplementation* newNode;
        if (offset < key) {
            const auto* new_prev = previous ? previous->implSetAt(context, offset, newValue) : new(context) ProtoSparseListImplementation(context, offset, newValue, nullptr, nullptr, false);
            newNode = new(context) ProtoSparseListImplementation(context, key, value, new_prev, next, false);
        } else if (offset > key) {
            const auto* new_next = next ? next->implSetAt(context, offset, newValue) : new(context) ProtoSparseListImplementation(context, offset, newValue, nullptr, nullptr, false);
            newNode = new(context) ProtoSparseListImplementation(context, key, value, previous, new_next, false);
        } else {
            if (value == newValue) return this;
            newNode = new(context) ProtoSparseListImplementation(context, key, newValue, previous, next, false);
        }
        return rebalance(context, newNode);
    }

    const ProtoSparseListImplementation* findMin(const ProtoSparseListImplementation* node) {
        while (node && node->previous && !node->previous->isEmpty) {
            node = node->previous;
        }
        return node;
    }

    const ProtoSparseListImplementation* ProtoSparseListImplementation::implRemoveAt(ProtoContext* context, unsigned long offset) const {
        if (isEmpty) {
            return this;
        }

        const ProtoSparseListImplementation* newNode;
        if (offset < key) {
            if (!previous) return this; // Not found, return unchanged
            auto* new_prev = previous->implRemoveAt(context, offset);
            if (new_prev == previous) return this; // No change was made
            newNode = new(context) ProtoSparseListImplementation(context, key, value, new_prev, next, false);
        } else if (offset > key) {
            if (!next) return this; // Not found, return unchanged
            auto* new_next = next->implRemoveAt(context, offset);
            if (new_next == next) return this; // No change was made
            newNode = new(context) ProtoSparseListImplementation(context, key, value, previous, new_next, false);
        } else {
            // Node to delete found
            if (!previous || previous->isEmpty) {
                if (!next || next->isEmpty) {
                    return new(context) ProtoSparseListImplementation(context, 0, nullptr, nullptr, nullptr, true);
                }
                return next; // No left child, promote right child
            }
            if (!next || next->isEmpty) {
                return previous; // No right child, promote left child
            }

            // Node with two children: Get the inorder successor (smallest in the right subtree)
            const ProtoSparseListImplementation* successor = findMin(next);
            // The successor's key and value replace this node's
            // Then, we recursively delete the successor from the right subtree
            auto* new_next = next->implRemoveAt(context, successor->key);
            newNode = new(context) ProtoSparseListImplementation(context, successor->key, successor->value, previous, new_next, false);
        }

        if (!newNode) {
            // This can happen if the last node is removed. Return an empty list.
            return new(context) ProtoSparseListImplementation(context, 0, nullptr, nullptr, nullptr, true);
        }

        return rebalance(context, newNode);
    }


    const ProtoSparseListIteratorImplementation* ProtoSparseListImplementation::implGetIterator(ProtoContext* context) const {
        return implGetIteratorWithQueue(context, nullptr);
    }

    const ProtoSparseListIteratorImplementation* ProtoSparseListImplementation::implGetIteratorWithQueue(ProtoContext* context, const ProtoSparseListIteratorImplementation* queue) const {
        if (isEmpty) return queue;
        const ProtoSparseListImplementation* node = this;
        const ProtoSparseListIteratorImplementation* stack = queue;
        while (node && !node->isEmpty) {
            stack = new(context) ProtoSparseListIteratorImplementation(context, ITERATOR_NEXT_THIS, node, stack);
            node = node->previous;
        }
        return stack;
    }

    void ProtoSparseListImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (ProtoObject::isCellPointer(value)) method(context, self, ProtoObject::asCellPointer(value));
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
        unsigned long c = 0;
        for (unsigned i = 0; i < MAX_INLINE; ++i)
            if (keys[i] != 0) ++c;
        return c;
    }

    bool ProtoSparseListSmallImplementation::implHas(ProtoContext*, unsigned long offset) const {
        if (offset == 0) return false;   // 0 cannot be stored in Small form
        for (unsigned i = 0; i < MAX_INLINE; ++i)
            if (keys[i] == offset) return true;
        return false;
    }

    const ProtoObject* ProtoSparseListSmallImplementation::implGetAt(ProtoContext*, unsigned long offset) const {
        if (offset == 0) return nullptr;
        for (unsigned i = 0; i < MAX_INLINE; ++i)
            if (keys[i] == offset) return values[i];
        return nullptr;
    }

    bool ProtoSparseListSmallImplementation::implPairAt(unsigned i, unsigned long* outKey, const ProtoObject** outValue) const {
        // Returns the i-th used pair in key-ascending order.  Caller
        // iterates i = 0 .. implCount()-1.  Used slots are detected by
        // keys[j] != 0 (the empty-slot sentinel).
        if (i >= MAX_INLINE) return false;
        unsigned long  ks[MAX_INLINE];
        const ProtoObject* vs[MAX_INLINE];
        unsigned n = 0;
        for (unsigned j = 0; j < MAX_INLINE; ++j) {
            if (keys[j] != 0) {
                // Insertion sort by key; n stays ≤ MAX_INLINE so the cost is
                // bounded (≤ 3 comparisons per insertion).
                unsigned k = n;
                while (k > 0 && ks[k - 1] > keys[j]) {
                    ks[k] = ks[k - 1];
                    vs[k] = vs[k - 1];
                    --k;
                }
                ks[k] = keys[j];
                vs[k] = values[j];
                ++n;
            }
        }
        if (i >= n) return false;
        if (outKey)   *outKey   = ks[i];
        if (outValue) *outValue = vs[i];
        return true;
    }

    const ProtoSparseListImplementation*
    ProtoSparseListSmallImplementation::promoteToAVL(ProtoContext* context) const {
        // Build an AVL whose in-order traversal reproduces this Small's
        // key-asc order.  Used on overflow promotion (setAt that would push
        // size > MAX_INLINE) and on getIterator to reuse the AVL iterator.
        unsigned long n = implCount();
        if (n == 0) {
            return new(context) ProtoSparseListImplementation(context, 0, nullptr, nullptr, nullptr, true);
        }
        // Build a node-by-node insertion sequence.  For n ≤ 3 the rebalance
        // overhead is trivial and bounded.
        const ProtoSparseListImplementation* avl =
            new(context) ProtoSparseListImplementation(context, 0, nullptr, nullptr, nullptr, true);
        for (unsigned i = 0; i < n; ++i) {
            unsigned long k;
            const ProtoObject* v;
            if (implPairAt(i, &k, &v)) {
                avl = avl->implSetAt(context, k, v);
            }
        }
        return avl;
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
            const ProtoObject* v = values[i];
            if (ProtoObject::isCellPointer(v)) {
                method(context, self, ProtoObject::asCellPointer(v));
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

        // Build a Small from up to MAX_INLINE (key, value) pairs already in
        // key-asc order.  Caller guarantees n ≤ MAX_INLINE.
        const ProtoSparseList* makeSmallSparseList(
            ProtoContext* context, unsigned n,
            const unsigned long* ks, const ProtoObject* const* vs)
        {
            return (new(context) ProtoSparseListSmallImplementation(context, n, ks, vs))
                ->asSparseList(context);
        }

        // setAt on a Small.  Returns a fresh Small (size ≤ MAX_INLINE) or
        // promotes to the AVL form when:
        //   (a) the resulting size would exceed MAX_INLINE, or
        //   (b) the caller passed offset == 0 — the Small uses key 0 as
        //       its empty-slot sentinel and cannot represent that key.
        // Caller wraps in CriticalSection.
        const ProtoSparseList* setAtSmall(
            ProtoContext* context,
            const ProtoSparseListSmallImplementation* small,
            unsigned long offset,
            const ProtoObject* value)
        {
            if (offset == 0 && value != nullptr) {
                // Key 0 is the empty-slot sentinel in the Small form.  We
                // cannot represent it inline.  Promote the existing
                // contents to AVL, then add the (0, value) pair there.
                const ProtoSparseListImplementation* avl =
                    small->promoteToAVL(context);
                avl = avl->implSetAt(context, 0, value);
                return avl->asSparseList(context);
            }

            if (value == nullptr) {
                // Spec: setAt(k, nullptr) ≡ removeAt(k).  Mirror the AVL
                // contract via a fresh Small with the slot cleared.  If
                // offset == 0 the call is also a no-op (key 0 is never
                // stored in the Small form).
                unsigned long  ks[ProtoSparseListSmallImplementation::MAX_INLINE];
                const ProtoObject* vs[ProtoSparseListSmallImplementation::MAX_INLINE];
                unsigned n = 0;
                for (unsigned i = 0; i < ProtoSparseListSmallImplementation::MAX_INLINE; ++i) {
                    if (small->keys[i] != 0 && small->keys[i] != offset) {
                        ks[n] = small->keys[i];
                        vs[n] = small->values[i];
                        ++n;
                    }
                }
                // Sort n-prefix by key (insertion sort; bounded).
                for (unsigned i = 1; i < n; ++i) {
                    unsigned long ki = ks[i];
                    const ProtoObject* vi = vs[i];
                    unsigned j = i;
                    while (j > 0 && ks[j - 1] > ki) {
                        ks[j] = ks[j - 1]; vs[j] = vs[j - 1]; --j;
                    }
                    ks[j] = ki; vs[j] = vi;
                }
                return makeSmallSparseList(context, n, ks, vs);
            }

            // value != nullptr, offset != 0: insert / update.  Collect
            // pairs, replacing the entry at `offset` if present, else
            // appending if there is room.  Promote to AVL on overflow.
            unsigned long  ks[ProtoSparseListSmallImplementation::MAX_INLINE + 1];
            const ProtoObject* vs[ProtoSparseListSmallImplementation::MAX_INLINE + 1];
            unsigned n = 0;
            bool replaced = false;
            for (unsigned i = 0; i < ProtoSparseListSmallImplementation::MAX_INLINE; ++i) {
                if (small->keys[i] == 0) continue;          // empty slot
                if (small->keys[i] == offset) {
                    ks[n] = offset;
                    vs[n] = value;
                    ++n;
                    replaced = true;
                } else {
                    ks[n] = small->keys[i];
                    vs[n] = small->values[i];
                    ++n;
                }
            }
            if (!replaced) {
                ks[n] = offset;
                vs[n] = value;
                ++n;
            }
            // Sort by key (insertion sort, bounded).
            for (unsigned i = 1; i < n; ++i) {
                unsigned long ki = ks[i];
                const ProtoObject* vi = vs[i];
                unsigned j = i;
                while (j > 0 && ks[j - 1] > ki) {
                    ks[j] = ks[j - 1]; vs[j] = vs[j - 1]; --j;
                }
                ks[j] = ki; vs[j] = vi;
            }
            if (n <= ProtoSparseListSmallImplementation::MAX_INLINE) {
                return makeSmallSparseList(context, n, ks, vs);
            }
            // Overflow: promote.  Build AVL via implSetAt for each pair.
            const ProtoSparseListImplementation* avl =
                new(context) ProtoSparseListImplementation(context, 0, nullptr, nullptr, nullptr, true);
            for (unsigned i = 0; i < n; ++i) {
                avl = avl->implSetAt(context, ks[i], vs[i]);
            }
            return avl->asSparseList(context);
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
