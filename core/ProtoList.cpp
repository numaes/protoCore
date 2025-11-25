/*
 * list.cpp
 *
 *  Created on: Aug 6, 2017
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <algorithm> // For std::max

namespace proto
{
    // --- ProtoListIteratorImplementation ---

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
        return this->base->implGetAt(context, static_cast<int>(this->currentIndex));
    }

    const ProtoListIteratorImplementation* ProtoListIteratorImplementation::implAdvance(ProtoContext* context) const
    {
        return new(context) ProtoListIteratorImplementation(context, this->base, this->currentIndex + 1);
    }

    const ProtoObject* ProtoListIteratorImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.listIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_ITERATOR;
        return p.oid.oid;
    }

    void ProtoListIteratorImplementation::finalize(ProtoContext* context) const {}

    void ProtoListIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const
    {
        if (this->base)
        {
            method(context, self, this->base);
        }
    }

    unsigned long ProtoListIteratorImplementation::getHash(ProtoContext* context) const
    {
        return Cell::getHash(context);
    }


    // --- ProtoListImplementation ---

    ProtoListImplementation::ProtoListImplementation(
        ProtoContext* context,
        const ProtoObject* value,
        bool isEmpty,
        const ProtoListImplementation* previousNode,
        const ProtoListImplementation* nextNode
    ) : Cell(context),
        value(value),
        previousNode(previousNode),
        nextNode(nextNode),
        isEmpty(isEmpty)
    {
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
            
            this->hash = (value ? value->getHash(context) : 0UL) ^
                         (previousNode ? previousNode->hash : 0UL) ^
                         (nextNode ? nextNode->hash : 0UL);
        }
    }

    ProtoListImplementation::~ProtoListImplementation() = default;

    // ... (AVL logic remains the same)

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

    // ... (Other method implementations)

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

    const ProtoList* ProtoListImplementation::asProtoList(ProtoContext* context) const {
        return (const ProtoList*)this->implAsObject(context);
    }
}
