/*
 * ProtoList.cpp
 *
 *  Created on: Aug 6, 2017
 *      Author: gamarino
 *
 *  This file implements the immutable list and its iterator. The list is
 *  implemented as a persistent, self-balancing AVL tree to ensure that
 *  all operations (append, insert, set, remove) have a time complexity
 *  of O(log n), regardless of the position in the list.
 */

#include "../headers/proto_internal.h"
#include <algorithm> // For std::max

namespace proto
{
    //=========================================================================
    // ProtoListIteratorImplementation
    //=========================================================================

    /**
     * @class ProtoListIteratorImplementation
     * @brief An iterator for traversing a `ProtoList`.
     *
     * This is a simple, forward-only iterator that keeps a reference to the
     * list it is iterating over and the current index.
     */

    ProtoListIteratorImplementation::ProtoListIteratorImplementation(
        ProtoContext* context,
        const ProtoListImplementation* base,
        const unsigned long currentIndex
    ) : Cell(context), base(base), currentIndex(currentIndex)
    {
    }

    ProtoListIteratorImplementation::~ProtoListIteratorImplementation() = default;

    int ProtoListIteratorImplementation::implHasNext(ProtoContext* context) const
    {
        if (!this->base || this->base->isEmpty) return false;
        return this->currentIndex < this->base->size;
    }

    const ProtoObject* ProtoListIteratorImplementation::implNext(ProtoContext* context) const
    {
        if (!implHasNext(context)) return PROTO_NONE;
        // Note: This is inefficient as it's an O(log n) lookup on each iteration.
        // A more optimized iterator would traverse the underlying tree directly.
        return this->base->implGetAt(context, static_cast<int>(this->currentIndex));
    }

    const ProtoListIteratorImplementation* ProtoListIteratorImplementation::implAdvance(ProtoContext* context) const
    {
        // Returns a new iterator pointing to the next index.
        return new(context) ProtoListIteratorImplementation(context, this->base, this->currentIndex + 1);
    }

    const ProtoObject* ProtoListIteratorImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.listIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_ITERATOR;
        return p.oid.oid;
    }

    const ProtoListIterator* ProtoListIteratorImplementation::asProtoListIterator(ProtoContext* context) const {
        return (const ProtoListIterator*)this->implAsObject(context);
    }

    void ProtoListIteratorImplementation::finalize(ProtoContext* context) const {}

    void ProtoListIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const
    {
        // The iterator must report its reference to the list to the GC.
        if (this->base)
        {
            method(context, self, this->base);
        }
    }

    unsigned long ProtoListIteratorImplementation::getHash(ProtoContext* context) const
    {
        return Cell::getHash(context);
    }


    //=========================================================================
    // ProtoListImplementation
    //=========================================================================
    
    /**
     * @class ProtoListImplementation
     * @brief An immutable, persistent list implemented as an AVL tree.
     *
     * Each node in the tree represents an element in the list. The `previousNode`
     * acts as the "left" child and `nextNode` as the "right" child. This structure
     * guarantees that all modification operations (which create new lists) are
     * O(log n) and that memory is shared efficiently between list versions.
     */

    ProtoListImplementation::ProtoListImplementation(
        ProtoContext* context,
        const ProtoObject* value,
        bool isEmpty,
        const ProtoListImplementation* previousNode,
        const ProtoListImplementation* nextNode
    ) : Cell(context),
        value(value),
        previousNode(previousNode),
        nextNode(nextNode)
    {
        this->isEmpty = isEmpty;
        if (isEmpty)
        {
            this->size = 0;
            this->height = 0;
            this->hash = 0;
        }
        else
        {
            const unsigned long prev_size = previousNode ? previousNode->size : 0;
            const unsigned long next_size = nextNode ? nextNode->size : 0;
            this->size = 1 + prev_size + next_size;

            const unsigned long prev_height = previousNode ? previousNode->height : 0;
            const unsigned long next_height = nextNode ? nextNode->height : 0;
            this->height = 1 + std::max(prev_height, next_height);
            
            // The hash is a combination of the value and child hashes.
            this->hash = (value ? value->getHash(context) : 0UL) ^
                         (previousNode ? previousNode->hash : 0UL) ^
                         (nextNode ? nextNode->hash : 0UL);
        }
    }

    ProtoListImplementation::~ProtoListImplementation() = default;

    const ProtoObject* ProtoListImplementation::implGetAt(ProtoContext* context, int index) const
    {
        if (this->isEmpty) return PROTO_NONE;
        if (index < 0) index += this->size;
        if (index < 0 || static_cast<unsigned long>(index) >= this->size) return PROTO_NONE;

        const ProtoListImplementation* node = this;
        while (node) {
            const unsigned long left_size = node->previousNode ? node->previousNode->size : 0;
            if (static_cast<unsigned long>(index) < left_size) {
                node = node->previousNode;
            } else if (static_cast<unsigned long>(index) > left_size) {
                node = node->nextNode;
                index -= (left_size + 1);
            } else {
                return node->value;
            }
        }
        return PROTO_NONE;
    }

    static int getHeight(const ProtoListImplementation* node) {
        return node ? node->height : 0;
    }

    static int getBalance(const ProtoListImplementation* node) {
        if (!node) return 0;
        return getHeight(node->previousNode) - getHeight(node->nextNode);
    }

    static const ProtoListImplementation* rotateRight(ProtoContext* context, const ProtoListImplementation* y) {
        const ProtoListImplementation* x = y->previousNode;
        const ProtoListImplementation* T2 = x->nextNode;
        const ProtoListImplementation* newX = new (context) ProtoListImplementation(context, x->value, false, x->previousNode, T2);
        return new (context) ProtoListImplementation(context, y->value, false, newX, y->nextNode);
    }

    static const ProtoListImplementation* rotateLeft(ProtoContext* context, const ProtoListImplementation* x) {
        const ProtoListImplementation* y = x->nextNode;
        const ProtoListImplementation* T2 = y->previousNode;
        const ProtoListImplementation* newY = new (context) ProtoListImplementation(context, y->value, false, T2, y->nextNode);
        return new (context) ProtoListImplementation(context, x->value, false, x->previousNode, newY);
    }

    static const ProtoListImplementation* balance(ProtoContext* context, const ProtoListImplementation* node) {
        int balanceFactor = getBalance(node);
        if (balanceFactor > 1) {
            if (getBalance(node->previousNode) < 0) {
                const ProtoListImplementation* newPrev = rotateLeft(context, node->previousNode);
                node = new (context) ProtoListImplementation(context, node->value, false, newPrev, node->nextNode);
            }
            return rotateRight(context, node);
        }
        if (balanceFactor < -1) {
            if (getBalance(node->nextNode) > 0) {
                const ProtoListImplementation* newNext = rotateRight(context, node->nextNode);
                node = new (context) ProtoListImplementation(context, node->value, false, node->previousNode, newNext);
            }
            return rotateLeft(context, node);
        }
        return node;
    }

    const ProtoListImplementation* ProtoListImplementation::implSetAt(ProtoContext* context, int index, const ProtoObject* value) const {
        if (this->isEmpty || index < 0 || (unsigned)index >= this->size) {
            return this; // Or throw an error
        }

        unsigned long left_size = this->previousNode ? this->previousNode->size : 0;
        if ((unsigned)index < left_size) {
            const ProtoListImplementation* new_left = this->previousNode->implSetAt(context, index, value);
            return new (context) ProtoListImplementation(context, this->value, false, new_left, this->nextNode);
        } else if ((unsigned)index > left_size) {
            const ProtoListImplementation* new_right = this->nextNode->implSetAt(context, index - (left_size + 1), value);
            return new (context) ProtoListImplementation(context, this->value, false, this->previousNode, new_right);
        } else {
            // This is the node to update
            return new (context) ProtoListImplementation(context, value, false, this->previousNode, this->nextNode);
        }
    }

    const ProtoListImplementation* ProtoListImplementation::implInsertAt(ProtoContext* context, int index, const ProtoObject* value) const {
        if (this->isEmpty) {
            return new (context) ProtoListImplementation(context, value, false);
        }

        unsigned long left_size = this->previousNode ? this->previousNode->size : 0;
        const ProtoListImplementation* new_node;

        if ((unsigned)index <= left_size) {
            const ProtoListImplementation* new_left = this->previousNode ? this->previousNode->implInsertAt(context, index, value) : new (context) ProtoListImplementation(context, value, false);
            new_node = new (context) ProtoListImplementation(context, this->value, false, new_left, this->nextNode);
        } else {
            const ProtoListImplementation* new_right = this->nextNode ? this->nextNode->implInsertAt(context, index - (left_size + 1), value) : new (context) ProtoListImplementation(context, value, false);
            new_node = new (context) ProtoListImplementation(context, this->value, false, this->previousNode, new_right);
        }
        return balance(context, new_node);
    }

    const ProtoListImplementation* ProtoListImplementation::implAppendLast(ProtoContext* context, const ProtoObject* value) const {
        return implInsertAt(context, this->size, value);
    }

    const ProtoListImplementation* findMin(const ProtoListImplementation* node) {
        while (node && node->previousNode) {
            node = node->previousNode;
        }
        return node;
    }

    const ProtoListImplementation* ProtoListImplementation::implRemoveAt(ProtoContext* context, int index) const {
        if (this->isEmpty || index < 0 || (unsigned)index >= this->size) {
            return this;
        }

        unsigned long left_size = this->previousNode ? this->previousNode->size : 0;
        const ProtoListImplementation* new_node;

        if ((unsigned)index < left_size) {
            const ProtoListImplementation* new_left = this->previousNode->implRemoveAt(context, index);
            new_node = new (context) ProtoListImplementation(context, this->value, false, new_left, this->nextNode);
        } else if ((unsigned)index > left_size) {
            const ProtoListImplementation* new_right = this->nextNode->implRemoveAt(context, index - (left_size + 1));
            new_node = new (context) ProtoListImplementation(context, this->value, false, this->previousNode, new_right);
        } else {
            // Node to be deleted is this one
            if (!this->previousNode) return this->nextNode;
            if (!this->nextNode) return this->previousNode;

            const ProtoListImplementation* successor = findMin(this->nextNode);
            const ProtoListImplementation* new_right = this->nextNode->implRemoveAt(context, 0);
            new_node = new (context) ProtoListImplementation(context, successor->value, false, this->previousNode, new_right);
        }

        return balance(context, new_node);
    }


    const ProtoListImplementation* ProtoListImplementation::implExtend(
        ProtoContext* context,
        const ProtoListImplementation* other
    ) const {
        const ProtoList* result = this->asProtoList(context);
        const ProtoListIterator* iter = other->implGetIterator(context)->asProtoListIterator(context);

        while (iter->hasNext(context)) {
            result = result->appendLast(context, iter->next(context));
            iter = iter->advance(context);
        }
        return toImpl<const ProtoListImplementation>(result);
    }

    unsigned long ProtoListImplementation::implGetSize(ProtoContext* context) const {
        return this->size;
    }

    const ProtoObject* ProtoListImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.listImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST;
        return p.oid.oid;
    }

    const ProtoListIteratorImplementation* ProtoListImplementation::implGetIterator(ProtoContext* context) const {
        return new(context) ProtoListIteratorImplementation(context, this, 0);
    }

    const ProtoList* ProtoListImplementation::asProtoList(ProtoContext* context) const {
        return (const ProtoList*)this->implAsObject(context);
    }

    unsigned long ProtoListImplementation::getHash(ProtoContext* context) const
    {
        return this->hash;
    }

    const ProtoObject* ProtoListImplementation::implGetFirst(ProtoContext* context) const
    {
        return implGetAt(context, 0);
    }

    const ProtoObject* ProtoListImplementation::implGetLast(ProtoContext* context) const
    {
        if (this->isEmpty) return PROTO_NONE;
        return implGetAt(context, this->size - 1);
    }

    const ProtoListImplementation* ProtoListImplementation::implGetSlice(ProtoContext* context, int from, int to) const
    {
        if (this->isEmpty) return this;
        // Placeholder for a more efficient rope-based slice
        const ProtoList* newList = context->newList();
        for (int i = from; i < to; ++i) {
            newList = newList->appendLast(context, this->implGetAt(context, i));
        }
        return toImpl<const ProtoListImplementation>(newList);
    }

    bool ProtoListImplementation::implHas(ProtoContext* context, const ProtoObject* targetValue) const
    {
        // Placeholder for a more efficient search
        for (unsigned long i = 0; i < this->size; ++i) {
            if (this->implGetAt(context, i) == targetValue) return true;
        }
        return false;
    }

    void ProtoListImplementation::finalize(ProtoContext* context) const {}

    void ProtoListImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const
    {
        if (this->isEmpty) return;
        if (this->previousNode) method(context, self, this->previousNode);
        if (this->nextNode) method(context, self, this->nextNode);
        if (this->value && this->value->isCell(context)) {
            method(context, self, this->value->asCell(context));
        }
    }

    // --- Public API Trampolines ---

    const ProtoList* ProtoList::setAt(ProtoContext* context, int index, const ProtoObject* value) const {
        return toImpl<const ProtoListImplementation>(this)->implSetAt(context, index, value)->asProtoList(context);
    }

    const ProtoList* ProtoList::insertAt(ProtoContext* context, int index, const ProtoObject* value) const {
        return toImpl<const ProtoListImplementation>(this)->implInsertAt(context, index, value)->asProtoList(context);
    }

    const ProtoList* ProtoList::removeAt(ProtoContext* context, int index) const {
        return toImpl<const ProtoListImplementation>(this)->implRemoveAt(context, index)->asProtoList(context);
    }
}
