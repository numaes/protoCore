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
        if (!this->base || this->base->isEmpty)
        {
            return false;
        }
        return this->currentIndex < this->base->size;
    }

    const ProtoObject* ProtoListIteratorImplementation::implNext(ProtoContext* context) const
    {
        if (!implHasNext(context))
        {
            return PROTO_NONE;
        }
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

    void ProtoListIteratorImplementation::finalize(ProtoContext* context) const override {}

    void ProtoListIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const override
    {
        if (this->base)
        {
            method(context, self, this->base);
        }
    }

    unsigned long ProtoListIteratorImplementation::getHash(ProtoContext* context) const override
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

    // --- AVL Tree Logic ---
    namespace
    {
        int getHeight(const ProtoListImplementation* node) { return node ? node->height : 0; }
        int getBalance(const ProtoListImplementation* node) {
            if (!node) return 0;
            return getHeight(node->nextNode) - getHeight(node->previousNode);
        }

        const ProtoListImplementation* rightRotate(ProtoContext* context, const ProtoListImplementation* y) {
            const ProtoListImplementation* x = y->previousNode;
            const auto T2 = x->nextNode;
            const auto* new_y = new(context) ProtoListImplementation(context, y->value, false, T2, y->nextNode);
            return new(context) ProtoListImplementation(context, x->value, false, x->previousNode, new_y);
        }

        const ProtoListImplementation* leftRotate(ProtoContext* context, const ProtoListImplementation* x) {
            const ProtoListImplementation* y = x->nextNode;
            const ProtoListImplementation* T2 = y->previousNode;
            const auto* new_x = new(context) ProtoListImplementation(context, x->value, false, x->previousNode, T2);
            return new(context) ProtoListImplementation(context, y->value, false, new_x, y->nextNode);
        }

        const ProtoListImplementation* rebalance(ProtoContext* context, const ProtoListImplementation* node) {
            const int balance = getBalance(node);
            if (balance < -1) {
                if (getBalance(node->previousNode) > 0) {
                    const ProtoListImplementation* new_prev = leftRotate(context, node->previousNode);
                    const auto* new_node = new(context) ProtoListImplementation(context, node->value, false, new_prev, node->nextNode);
                    return rightRotate(context, new_node);
                }
                return rightRotate(context, node);
            }
            if (balance > 1) {
                if (getBalance(node->nextNode) < 0) {
                    const ProtoListImplementation* new_next = rightRotate(context, node->nextNode);
                    const auto* new_node = new(context) ProtoListImplementation(context, node->value, false, node->previousNode, new_next);
                    return leftRotate(context, new_node);
                }
                return leftRotate(context, node);
            }
            return node;
        }
    }

    // --- Public Interface Methods ---

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

    const ProtoObject* ProtoListImplementation::implGetFirst(ProtoContext* context) const {
        return implGetAt(context, 0);
    }

    const ProtoObject* ProtoListImplementation::implGetLast(ProtoContext* context) const {
        return implGetAt(context, -1);
    }

    unsigned long ProtoListImplementation::implGetSize(ProtoContext* context) const {
        return this->isEmpty ? 0 : this->size;
    }

    bool ProtoListImplementation::implHas(ProtoContext* context, const ProtoObject* targetValue) const {
        if (this->isEmpty) return false;
        if (this->value == targetValue) return true;
        if (this->previousNode && this->previousNode->implHas(context, targetValue)) return true;
        if (this->nextNode && this->nextNode->implHas(context, targetValue)) return true;
        return false;
    }

    const ProtoListImplementation* ProtoListImplementation::implAppendLast(ProtoContext* context, ProtoObject* newValue) const
    {
        if (this->isEmpty) {
            return new(context) ProtoListImplementation(context, newValue, false);
        }
        const ProtoListImplementation* new_next = this->nextNode ? this->nextNode->implAppendLast(context, newValue) : new(context) ProtoListImplementation(context, newValue, false);
        const auto* newNode = new(context) ProtoListImplementation(context, this->value, false, this->previousNode, new_next);
        return rebalance(context, newNode);
    }
    
    // ... (Other methods like removeAt, setAt, etc. would follow a similar pattern of creating new nodes and rebalancing)

    const ProtoList* ProtoListImplementation::asProtoList(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.listImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST;
        return p.list;
    }

    const ProtoObject* ProtoListImplementation::implAsObject(ProtoContext* context) const
    {
        return asProtoList(context)->asObject(context);
    }

    unsigned long ProtoListImplementation::getHash(ProtoContext* context) const override {
        return Cell::getHash(context);
    }

    const ProtoListIteratorImplementation* ProtoListImplementation::implGetIterator(ProtoContext* context) const {
        return new(context) ProtoListIteratorImplementation(context, this, 0);
    }

    void ProtoListImplementation::finalize(ProtoContext* context) const override {}

    void ProtoListImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const override
    {
        if (this->isEmpty) return;
        if (this->previousNode) method(context, self, this->previousNode);
        if (this->nextNode) method(context, self, this->nextNode);
        if (this->value && this->value->isCell(context)) {
            method(context, self, this->value->asCell(context));
        }
    }
}
