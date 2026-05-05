/*
 * ProtoList.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <algorithm> // For std::max

namespace proto {

    namespace { // Anonymous namespace for file-local helpers

        /**
         * @brief Safely gets the size of a node.
         * Checks for null and 64-byte alignment before dereferencing.
         * A tagged handle passed by mistake will be unaligned and treated as size 0.
         */
        inline unsigned long get_node_size(const ProtoListImplementation* node) {
            if (!node || (reinterpret_cast<uintptr_t>(node) & 0x3F) != 0) {
                return 0;
            }
            return node->size;
        }

        /**
         * @brief Safely gets the height of a node.
         * Checks for null and 64-byte alignment before dereferencing.
         * A tagged handle passed by mistake will be unaligned and treated as height 0.
         */
        inline int get_node_height(const ProtoListImplementation* node) {
            if (!node || (reinterpret_cast<uintptr_t>(node) & 0x3F) != 0) {
                return 0;
            }
            return node->height;
        }

        int getBalance(const ProtoListImplementation* node) {
            if (!node || node->isEmpty) return 0;
            return get_node_height(node->previousNode) - get_node_height(node->nextNode);
        }

        const ProtoListImplementation* rightRotate(ProtoContext* context, const ProtoListImplementation* y) {
            const ProtoListImplementation* x = y->previousNode;
            const ProtoListImplementation* T2 = x ? x->nextNode : nullptr;
            auto* new_y = new (context) ProtoListImplementation(context, y->value, false, T2, y->nextNode);
            return new (context) ProtoListImplementation(context, x->value, false, x->previousNode, new_y);
        }

        const ProtoListImplementation* leftRotate(ProtoContext* context, const ProtoListImplementation* x) {
            const ProtoListImplementation* y = x->nextNode;
            const ProtoListImplementation* T2 = y ? y->previousNode : nullptr;
            auto* new_x = new (context) ProtoListImplementation(context, x->value, false, x->previousNode, T2);
            return new (context) ProtoListImplementation(context, y->value, false, new_x, y->nextNode);
        }

        const ProtoListImplementation* rebalance(ProtoContext* context, const ProtoListImplementation* node) {
            int balance = getBalance(node);

            // Left Left Case
            if (balance > 1 && getBalance(node->previousNode) >= 0)
                return rightRotate(context, node);

            // Left Right Case
            if (balance > 1 && getBalance(node->previousNode) < 0) {
                const ProtoListImplementation* new_prev = leftRotate(context, node->previousNode);
                return rightRotate(context, new (context) ProtoListImplementation(context, node->value, false, new_prev, node->nextNode));
            }

            // Right Right Case
            if (balance < -1 && getBalance(node->nextNode) <= 0)
                return leftRotate(context, node);

            // Right Left Case
            if (balance < -1 && getBalance(node->nextNode) > 0) {
                const ProtoListImplementation* new_next = rightRotate(context, node->nextNode);
                return leftRotate(context, new (context) ProtoListImplementation(context, node->value, false, node->previousNode, new_next));
            }

            return node;
        }

    } // end anonymous namespace

    //=========================================================================
    // ProtoListImplementation
    //=========================================================================

    ProtoListImplementation::ProtoListImplementation(
        ProtoContext* context,
        const ProtoObject* v,
        bool empty,
        const ProtoListImplementation* prev,
        const ProtoListImplementation* next
    ) : Cell(context), value(empty ? nullptr : v), previousNode(empty ? nullptr : prev), nextNode(empty ? nullptr : next),
        hash(0),
        size(empty ? 0 : get_node_size(prev) + get_node_size(next) + 1),
        height(empty ? 0 : 1 + std::max(get_node_height(prev), get_node_height(next))),
        isEmpty(empty)
    {
    }

    const ProtoObject* ProtoListImplementation::implGetAt(ProtoContext* context, int index) const {
        if (isEmpty) return nullptr;
        const unsigned long left_size = previousNode ? previousNode->size : 0;
        if (index < left_size) {
            return previousNode->implGetAt(context, index);
        }
        if (index == left_size) {
            return value;
        }
        return nextNode->implGetAt(context, index - left_size - 1);
    }

    bool ProtoListImplementation::implHas(proto::ProtoContext* context, const proto::ProtoObject* targetValue) const {
        if (!targetValue) return false;
        if (isEmpty) return false;
        
        if (value == targetValue) {
            return true;
        }
        if (value->isInteger(context) && targetValue->isInteger(context)) {
            if (value->asLong(context) == targetValue->asLong(context)) {
                return true;
            }
        } else if (value->isString(context) && targetValue->isString(context)) {
            if (value->asString(context)->cmp_to_string(context, targetValue->asString(context)) == 0) {
                return true;
            }
        }
        if (previousNode && previousNode->implHas(context, targetValue)) return true;
        if (nextNode && nextNode->implHas(context, targetValue)) return true;
        return false;
    }

    const ProtoListImplementation* ProtoListImplementation::implSetAt(ProtoContext* context, int index, const ProtoObject* newValue) const {
        if (isEmpty || index < 0 || index >= size) {
            return this;
        }

        const unsigned long left_size = previousNode ? previousNode->size : 0;

        if (index < left_size) {
            const ProtoListImplementation* new_prev = previousNode->implSetAt(context, index, newValue);
            return rebalance(context, new (context) ProtoListImplementation(context, value, false, new_prev, nextNode));
        }
        if (index == left_size) {
            return rebalance(context, new (context) ProtoListImplementation(context, newValue, false, previousNode, nextNode));
        }

        const ProtoListImplementation* new_next = nextNode->implSetAt(context, index - left_size - 1, newValue);
        return rebalance(context, new (context) ProtoListImplementation(context, value, false, previousNode, new_next));
    }

    const ProtoListImplementation* ProtoListImplementation::implInsertAt(ProtoContext* context, int index, const ProtoObject* newValue) const {
        if (isEmpty) {
            return new (context) ProtoListImplementation(context, newValue, false, nullptr, nullptr);
        }

        const unsigned long left_size = previousNode ? previousNode->size : 0;

        if (index <= left_size) {
            const ProtoListImplementation* new_prev = previousNode
                ? previousNode->implInsertAt(context, index, newValue)
                : new (context) ProtoListImplementation(context, newValue, false, nullptr, nullptr);
            return rebalance(context, new (context) ProtoListImplementation(context, value, false, new_prev, nextNode));
        } else {
            const ProtoListImplementation* new_next = nextNode
                ? nextNode->implInsertAt(context, index - left_size - 1, newValue)
                : new (context) ProtoListImplementation(context, newValue, false, nullptr, nullptr);
            return rebalance(context, new (context) ProtoListImplementation(context, value, false, previousNode, new_next));
        }
    }


    const ProtoListImplementation* ProtoListImplementation::implAppendLast(ProtoContext* context, const ProtoObject* newValue) const {
        return implInsertAt(context, size, newValue);
    }

    const ProtoListImplementation* ProtoListImplementation::implRemoveAt(ProtoContext* context, int index) const {
        if (isEmpty || index < 0 || index >= size) {
            return this;
        }

        const unsigned long left_size = previousNode ? previousNode->size : 0;

        if (index < left_size) {
            const ProtoListImplementation* new_prev = previousNode->implRemoveAt(context, index);
            if (new_prev && new_prev->size == 0) new_prev = nullptr;
            return rebalance(context, new (context) ProtoListImplementation(context, value, false, new_prev, nextNode));
        }
        else if (index == left_size) {
            // Remove current value
            if (!previousNode && !nextNode) {
                // Became empty
                return new (context) ProtoListImplementation(context, nullptr, true, nullptr, nullptr);
            }
            if (!previousNode) return rebalance(context, nextNode);
            if (!nextNode) return rebalance(context, previousNode);

            // Both exist. Promote from left.
            const ProtoObject* new_val = previousNode->implGetAt(context, previousNode->size - 1);
            const ProtoListImplementation* new_prev = previousNode->implRemoveAt(context, previousNode->size - 1);
            if (new_prev && new_prev->size == 0) new_prev = nullptr;
            return rebalance(context, new (context) ProtoListImplementation(context, new_val, false, new_prev, nextNode));
        }
        else {
            // index > left_size
            const ProtoListImplementation* new_next = nextNode->implRemoveAt(context, index - left_size - 1);
            if (new_next && new_next->size == 0) new_next = nullptr;
            return rebalance(context, new (context) ProtoListImplementation(context, value, false, previousNode, new_next));
        }
    }

    void ProtoListImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (ProtoObject::isCellPointer(value)) method(context, self, ProtoObject::asCellPointer(value));
        if (previousNode && ProtoObject::isCellPointer(reinterpret_cast<const ProtoObject*>(previousNode))) {
            method(context, self, ProtoObject::asCellPointer(reinterpret_cast<const ProtoObject*>(previousNode)));
        }
        if (nextNode && ProtoObject::isCellPointer(reinterpret_cast<const ProtoObject*>(nextNode))) {
            method(context, self, ProtoObject::asCellPointer(reinterpret_cast<const ProtoObject*>(nextNode)));
        }
    }

    const ProtoObject* ProtoListImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST;
        return p.oid;
    }

    const ProtoList* ProtoListImplementation::asProtoList(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST;
        return p.list;
    }

    const ProtoListIteratorImplementation* ProtoListImplementation::implGetIterator(ProtoContext* context) const {
        // Build the tagged base directly; the iterator dispatches on the tag.
        ProtoObjectPointer p;
        p.listImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST;
        return new (context) ProtoListIteratorImplementation(context, p.oid, 0);
    }

    //=========================================================================
    // ProtoListSmallImplementation
    //=========================================================================

    ProtoListSmallImplementation::ProtoListSmallImplementation(
        ProtoContext* context, unsigned n, const ProtoObject* const* items
    ) : Cell(context), size(n)
    {
        for (unsigned i = 0; i < MAX_INLINE; ++i) {
            slots[i] = (i < n) ? items[i] : nullptr;
        }
    }

    const ProtoObject* ProtoListSmallImplementation::implGetAt(ProtoContext* context, int index) const {
        if (index < 0 || static_cast<unsigned long>(index) >= size) return nullptr;
        return slots[index];
    }

    bool ProtoListSmallImplementation::implHas(ProtoContext* context, const ProtoObject* targetValue) const {
        if (!targetValue) return false;
        for (unsigned long i = 0; i < size; ++i) {
            const ProtoObject* v = slots[i];
            if (v == targetValue) return true;
            if (v && v->isInteger(context) && targetValue->isInteger(context)) {
                if (v->asLong(context) == targetValue->asLong(context)) return true;
            } else if (v && v->isString(context) && targetValue->isString(context)) {
                if (v->asString(context)->cmp_to_string(context, targetValue->asString(context)) == 0) return true;
            }
        }
        return false;
    }

    const ProtoObject* ProtoListSmallImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listSmallImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_SMALL;
        return p.oid;
    }

    const ProtoList* ProtoListSmallImplementation::asProtoList(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listSmallImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_SMALL;
        return p.list;
    }

    void ProtoListSmallImplementation::processReferences(
        ProtoContext* context, void* self,
        void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        for (unsigned long i = 0; i < size; ++i) {
            const ProtoObject* v = slots[i];
            if (ProtoObject::isCellPointer(v)) {
                method(context, self, ProtoObject::asCellPointer(v));
            }
        }
    }

    //=========================================================================
    // Output-form selection helpers (file-local)
    //=========================================================================
    namespace {
        // Bottom-up balanced AVL build from a flat array. Cheaper than
        // newList() + N appendLast: skips per-element rebalance walks.
        // Returns nullptr-equivalent (empty list cell) when n == 0.
        const ProtoListImplementation* buildBalancedFromArray(
            ProtoContext* context, const ProtoObject* const* items, unsigned n)
        {
            if (n == 0) {
                return new (context) ProtoListImplementation(context, nullptr, true, nullptr, nullptr);
            }
            unsigned mid = n / 2;
            const ProtoListImplementation* left =
                (mid > 0) ? buildBalancedFromArray(context, items, mid) : nullptr;
            const ProtoListImplementation* right =
                (mid + 1 < n) ? buildBalancedFromArray(context, items + mid + 1, n - mid - 1) : nullptr;
            return new (context) ProtoListImplementation(
                context, items[mid], false,
                (left && left->isEmpty) ? nullptr : left,
                (right && right->isEmpty) ? nullptr : right);
        }

        // Picks the inline form when the resulting size fits, AVL otherwise.
        // Used by every mutator output path so SmallList stays small as long
        // as possible without forcing a one-way upgrade.
        const ProtoList* makeOutputList(
            ProtoContext* context, const ProtoObject* const* items, unsigned n)
        {
            if (n <= ProtoListSmallImplementation::MAX_INLINE) {
                return (new (context) ProtoListSmallImplementation(context, n, items))
                    ->asProtoList(context);
            }
            return buildBalancedFromArray(context, items, n)->asProtoList(context);
        }
    }

    //=========================================================================
    // ProtoListIteratorImplementation
    //=========================================================================

    ProtoListIteratorImplementation::ProtoListIteratorImplementation(
        ProtoContext* context,
        const ProtoObject* b,
        unsigned long index
    ) : Cell(context), base(b), currentIndex(index)
    {
    }

    namespace {
        // Iterator helpers — read size / element from the tagged base
        // pointer without a virtual call.
        unsigned long iterBaseSize(const ProtoObject* base) {
            if (!base) return 0;
            ProtoObjectPointer pa{}; pa.oid = base;
            if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
                return toImpl<const ProtoListSmallImplementation>(base)->size;
            }
            return toImpl<const ProtoListImplementation>(base)->size;
        }
        const ProtoObject* iterBaseGetAt(ProtoContext* context, const ProtoObject* base, unsigned long index) {
            if (!base) return nullptr;
            ProtoObjectPointer pa{}; pa.oid = base;
            if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
                return toImpl<const ProtoListSmallImplementation>(base)
                    ->implGetAt(context, static_cast<int>(index));
            }
            return toImpl<const ProtoListImplementation>(base)
                ->implGetAt(context, static_cast<int>(index));
        }
    }

    int ProtoListIteratorImplementation::implHasNext() const {
        if (!this->base) return false;
        return this->currentIndex < iterBaseSize(this->base);
    }

    const ProtoObject* ProtoListIteratorImplementation::implNext(ProtoContext* context) const {
        return iterBaseGetAt(context, this->base, this->currentIndex);
    }

    const ProtoListIteratorImplementation* ProtoListIteratorImplementation::implAdvance(ProtoContext* context) const {
        if (this->currentIndex < iterBaseSize(this->base)) {
            return new (context) ProtoListIteratorImplementation(context, this->base, this->currentIndex + 1);
        }
        return this;
    }

    const ProtoListIterator* ProtoListIteratorImplementation::asProtoListIterator(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_ITERATOR;
        return p.listIterator;
    }

    const ProtoObject* ProtoListIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_ITERATOR;
        return p.oid;
    }

    void ProtoListIteratorImplementation::processReferences(ProtoContext* context, void* callback_data, void (*callback)(ProtoContext*, void*, const Cell*)) const {
        if (ProtoObject::isCellPointer(base)) {
            callback(context, callback_data, ProtoObject::asCellPointer(base));
        }
    }

    // ProtoList / ProtoListIterator external API trampolines
    //
    // Each method dispatches on POINTER_TAG_{LIST,LIST_SMALL} once at entry.
    // Read-only methods read directly from the relevant impl form (no
    // promotion). Mutator methods produce a fresh output whose form is
    // selected by the resulting size: ≤ MAX_INLINE → SmallList, > MAX_INLINE
    // → AVL. Only `appendLast` overflowing past MAX_INLINE triggers a
    // strict promotion (the result genuinely cannot fit inline).

    // GC critical sections on the immutable-collection mutator API.
    //
    // Every mutator builds a fresh tree of cells; the result is held in a
    // C++ local until the caller publishes it.  Without the guard a
    // concurrent STW root scan landing between two allocations would
    // observe the half-built tree as candidate-but-unreachable and a
    // sweep would free it.  Same discipline as
    // ProtoObject::setAttribute, ProtoMultiset::add, ProtoSet::add.
    //
    // The guard is per-thread, lock-free, single-int; it is cheap to
    // enter and callers that already wrap a CriticalSection (e.g.
    // setAttribute) just bump the depth counter — no nested STW
    // suppression issue.

    namespace {
        // Read a SmallList's slots into a stack-buffer so a mutator can
        // recompute the result.  Returns the count.
        unsigned smallToBuffer(const ProtoListSmallImplementation* small,
                               const ProtoObject** buf) {
            unsigned n = static_cast<unsigned>(small->size);
            for (unsigned i = 0; i < n; ++i) buf[i] = small->slots[i];
            return n;
        }

        // Pull every element of an AVL list into a flat heap vector.  Used
        // by the rare path where a SmallList is mixed with an AVL list in
        // a mutator (e.g. extend) and we want a single-pass output build.
        void avlToVector(ProtoContext* context, const ProtoListImplementation* avl,
                         std::vector<const ProtoObject*>& out) {
            unsigned long n = avl->size;
            out.reserve(out.size() + n);
            for (unsigned long i = 0; i < n; ++i) {
                out.push_back(avl->implGetAt(context, static_cast<int>(i)));
            }
        }
    }

    unsigned long ProtoList::getSize(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            return toImpl<const ProtoListSmallImplementation>(this)->size;
        }
        return toImpl<const ProtoListImplementation>(this)->size;
    }
    const ProtoObject* ProtoList::getAt(ProtoContext* context, int index) const {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        const ProtoObject* result;
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            result = toImpl<const ProtoListSmallImplementation>(this)->implGetAt(context, index);
        } else {
            result = toImpl<const ProtoListImplementation>(this)->implGetAt(context, index);
        }
        return result ? result : PROTO_NONE;
    }
    const ProtoObject* ProtoList::getFirst(ProtoContext* context) const {
        unsigned long size = getSize(context);
        if (size == 0) return PROTO_NONE;
        return getAt(context, 0);
    }
    const ProtoObject* ProtoList::getLast(ProtoContext* context) const {
        unsigned long size = getSize(context);
        if (size == 0) return PROTO_NONE;
        return getAt(context, size - 1);
    }
    bool ProtoList::has(ProtoContext* context, const ProtoObject* value) const {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            return toImpl<const ProtoListSmallImplementation>(this)->implHas(context, value);
        }
        return toImpl<const ProtoListImplementation>(this)->implHas(context, value);
    }
    const ProtoList* ProtoList::setAt(ProtoContext* context, int index, const ProtoObject* value) const {
        ProtoContext::CriticalSection cs(context);
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            const auto* small = toImpl<const ProtoListSmallImplementation>(this);
            if (index < 0 || static_cast<unsigned long>(index) >= small->size) {
                return const_cast<ProtoList*>(this);
            }
            const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE];
            unsigned n = smallToBuffer(small, buf);
            buf[index] = value;
            return makeOutputList(context, buf, n);
        }
        return toImpl<const ProtoListImplementation>(this)->implSetAt(context, index, value)->asProtoList(context);
    }
    const ProtoList* ProtoList::insertAt(ProtoContext* context, int index, const ProtoObject* value) const {
        ProtoContext::CriticalSection cs(context);
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            const auto* small = toImpl<const ProtoListSmallImplementation>(this);
            unsigned n = static_cast<unsigned>(small->size);
            if (index < 0) index = 0;
            if (static_cast<unsigned>(index) > n) index = static_cast<int>(n);
            // Resulting size = n + 1.  If it fits inline, stay SmallList;
            // otherwise build AVL via buildBalancedFromArray.
            const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE + 1];
            for (unsigned i = 0; i < static_cast<unsigned>(index); ++i) buf[i] = small->slots[i];
            buf[index] = value;
            for (unsigned i = static_cast<unsigned>(index); i < n; ++i) buf[i + 1] = small->slots[i];
            return makeOutputList(context, buf, n + 1);
        }
        return toImpl<const ProtoListImplementation>(this)->implInsertAt(context, index, value)->asProtoList(context);
    }
    const ProtoList* ProtoList::appendFirst(ProtoContext* context, const ProtoObject* value) const { return insertAt(context, 0, value); }
    const ProtoList* ProtoList::appendLast(ProtoContext* context, const ProtoObject* value) const {
        ProtoContext::CriticalSection cs(context);
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            const auto* small = toImpl<const ProtoListSmallImplementation>(this);
            const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE + 1];
            unsigned n = smallToBuffer(small, buf);
            buf[n] = value;
            return makeOutputList(context, buf, n + 1);
        }
        return toImpl<const ProtoListImplementation>(this)->implAppendLast(context, value)->asProtoList(context);
    }
    const ProtoList* ProtoList::extend(ProtoContext* context, const ProtoList* other) const {
        // The loop holds `result` (a fresh tree) in a C++ local across
        // every appendLast; the guard keeps it rooted between iterations.
        ProtoContext::CriticalSection cs(context);
        const ProtoList* result = const_cast<ProtoList*>(this);
        const ProtoListIterator* it = other->getIterator(context);
        while (it->hasNext(context)) {
            result = result->appendLast(context, it->next(context));
            it = it->advance(context);
        }
        return result;
    }
    const ProtoList* ProtoList::splitFirst(ProtoContext* context, int index) const {
        unsigned long size = getSize(context);
        if (index <= 0) return context->newList();
        if (index >= (int)size) return const_cast<ProtoList*>(this);
        return getSlice(context, 0, index);
    }
    const ProtoList* ProtoList::splitLast(ProtoContext* context, int index) const {
        unsigned long size = getSize(context);
        if (index <= 0) return context->newList();
        if (index >= (int)size) return const_cast<ProtoList*>(this);
        return getSlice(context, size - index, size);
    }
    const ProtoList* ProtoList::removeFirst(ProtoContext* context) const {
        unsigned long size = getSize(context);
        if (size == 0) return const_cast<ProtoList*>(this);
        return removeAt(context, 0);
    }
    const ProtoList* ProtoList::removeLast(ProtoContext* context) const {
        unsigned long size = getSize(context);
        if (size == 0) return const_cast<ProtoList*>(this);
        return removeAt(context, size - 1);
    }
    const ProtoList* ProtoList::removeAt(ProtoContext* context, int index) const {
        ProtoContext::CriticalSection cs(context);
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            const auto* small = toImpl<const ProtoListSmallImplementation>(this);
            unsigned n = static_cast<unsigned>(small->size);
            if (index < 0 || static_cast<unsigned>(index) >= n) {
                return const_cast<ProtoList*>(this);
            }
            const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE];
            unsigned out = 0;
            for (unsigned i = 0; i < n; ++i) {
                if (i == static_cast<unsigned>(index)) continue;
                buf[out++] = small->slots[i];
            }
            return makeOutputList(context, buf, out);
        }
        return toImpl<const ProtoListImplementation>(this)->implRemoveAt(context, index)->asProtoList(context);
    }
    const ProtoList* ProtoList::removeSlice(ProtoContext* context, int from, int to) const {
        // Loop holds `result` between removeAt calls.
        ProtoContext::CriticalSection cs(context);
        const ProtoList* result = const_cast<ProtoList*>(this);
        // Remove from end to start to preserve indices
        for (int i = to - 1; i >= from; --i) {
            result = result->removeAt(context, i);
        }
        return result;
    }
    const ProtoList* ProtoList::getSlice(ProtoContext* context, int start, int end) const {
        // Loop holds `list` between appendLast calls.
        ProtoContext::CriticalSection cs(context);
        const ProtoList* list = context->newList();
        for (int i = start; i < end; ++i) {
            list = list->appendLast(context, getAt(context, i));
        }
        return list;
    }
    const ProtoObject* ProtoList::asObject(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            return toImpl<const ProtoListSmallImplementation>(this)->implAsObject(context);
        }
        return toImpl<const ProtoListImplementation>(this)->implAsObject(context);
    }
    const ProtoListIterator* ProtoList::getIterator(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            // Use the tagged pointer (`this`) directly as the iterator base.
            return (new (context) ProtoListIteratorImplementation(
                        context, reinterpret_cast<const ProtoObject*>(this), 0))
                    ->asProtoListIterator(context);
        }
        return toImpl<const ProtoListImplementation>(this)->implGetIterator(context)->asProtoListIterator(context);
    }
    unsigned long ProtoList::getHash(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            return toImpl<const ProtoListSmallImplementation>(this)->getHash(context);
        }
        return toImpl<const ProtoListImplementation>(this)->getHash(context);
    }

    int ProtoListIterator::hasNext(ProtoContext* context) const { if (!this) return 0; return toImpl<const ProtoListIteratorImplementation>(this)->implHasNext(); }
    const ProtoObject* ProtoListIterator::next(ProtoContext* context) const { if (!this) return nullptr; return toImpl<const ProtoListIteratorImplementation>(this)->implNext(context); }
    const ProtoListIterator* ProtoListIterator::advance(ProtoContext* context) const { if (!this) return nullptr; return toImpl<const ProtoListIteratorImplementation>(this)->implAdvance(context)->asProtoListIterator(context); }
    const ProtoObject* ProtoListIterator::asObject(ProtoContext* context) const { if (!this) return nullptr; return toImpl<const ProtoListIteratorImplementation>(this)->implAsObject(context); }

    const ProtoList* ProtoList::multiply(ProtoContext* context, const ProtoObject* count) const {
        if (!count->isInteger(context)) return nullptr;
        long long n = count->asLong(context);
        if (n <= 0) return context->newList();
        if (n == 1) return const_cast<ProtoList*>(this);

        // Nested loop holds `result` across many appendLast calls.
        ProtoContext::CriticalSection cs(context);
        const ProtoList* result = context->newList();
        unsigned long self_size = getSize(context);
        for (long long i = 0; i < n; ++i) {
            for (unsigned long j = 0; j < self_size; ++j) {
                result = result->appendLast(context, getAt(context, static_cast<int>(j)));
            }
        }
        return result;
    }
}
