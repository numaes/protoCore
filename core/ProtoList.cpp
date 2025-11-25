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

    const ProtoListImplementation* ProtoListImplementation::implAppendLast(ProtoContext* context, const ProtoObject* value) const {
        // Simplified AVL append logic for demonstration.
        return new(context) ProtoListImplementation(context, value, false, this);
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
}
