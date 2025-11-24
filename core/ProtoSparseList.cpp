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
    // --- ProtoSparseListIteratorImplementation ---

    ProtoSparseListIteratorImplementation::ProtoSparseListIteratorImplementation(
        ProtoContext* context,
        int state,
        const ProtoSparseListImplementation* current,
        const ProtoSparseListIteratorImplementation* queue
    ) : Cell(context), state(state), current(current), queue(queue)
    {
    }
 
    ProtoSparseListIteratorImplementation::~ProtoSparseListIteratorImplementation() = default;

    int ProtoSparseListIteratorImplementation::implHasNext(ProtoContext* context) const
    {
        if (this->state == ITERATOR_NEXT_PREVIOUS && this->current && this->current->previous) return true;
        if (this->state == ITERATOR_NEXT_THIS && this->current && this->current->value != PROTO_NONE) return true;
        if (this->state == ITERATOR_NEXT_NEXT && this->current && this->current->next) return true;
        if (this->queue) return this->queue->implHasNext(context);
        return false;
    }

    unsigned long ProtoSparseListIteratorImplementation::implNextKey(ProtoContext* context) const
    {
        if (this->state == ITERATOR_NEXT_THIS && this->current)
        {
            return this->current->key;
        }
        return 0;
    }

    const ProtoObject* ProtoSparseListIteratorImplementation::implNextValue(ProtoContext* context) const
    {
        if (this->state == ITERATOR_NEXT_THIS && this->current)
        {
            return this->current->value;
        }
        return PROTO_NONE;
    }

    const ProtoSparseListIteratorImplementation* ProtoSparseListIteratorImplementation::implAdvance(ProtoContext* context) const
    {
        if (this->state == ITERATOR_NEXT_PREVIOUS)
        {
            return new(context) ProtoSparseListIteratorImplementation(context, ITERATOR_NEXT_THIS, this->current, this->queue);
        }
        if (this->state == ITERATOR_NEXT_THIS)
        {
            if (this->current && this->current->next)
            {
                return this->current->next->implGetIterator(context);
            }
            if (this->queue)
            {
                return this->queue->implAdvance(context);
            }
            return nullptr;
        }
        if (this->state == ITERATOR_NEXT_NEXT && this->queue)
        {
            return this->queue->implAdvance(context);
        }
        return nullptr;
    }

    const ProtoObject* ProtoSparseListIteratorImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.sparseListIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_SPARSE_LIST_ITERATOR;
        return p.oid.oid;
    }

    void ProtoSparseListIteratorImplementation::finalize(ProtoContext* context) const override {}

    void ProtoSparseListIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const override
    {
        if (this->current) method(context, self, this->current);
        if (this->queue) method(context, self, this->queue);
    }

    unsigned long ProtoSparseListIteratorImplementation::getHash(ProtoContext* context) const override
    {
        return Cell::getHash(context);
    }

    // --- ProtoSparseListImplementation ---

    ProtoSparseListImplementation::ProtoSparseListImplementation(
        ProtoContext* context,
        unsigned long key,
        const ProtoObject* value,
        const ProtoSparseListImplementation* previous,
        const ProtoSparseListImplementation* next
    ) : Cell(context), key(key), value(value), previous(previous), next(next)
    {
        this->count = (value != PROTO_NONE ? 1 : 0) + (previous ? previous->count : 0) + (next ? next->count : 0);
        const unsigned long previous_height = previous ? previous->height : 0;
        const unsigned long next_height = next ? next->height : 0;
        this->height = 1 + std::max(previous_height, next_height);
        this->hash = key ^ (value ? value->getHash(context) : 0) ^ (previous ? previous->hash : 0) ^ (next ? next->hash : 0);
    }

    ProtoSparseListImplementation::~ProtoSparseListImplementation() = default;

    namespace {
        // AVL Tree helper functions
        int getHeight(const ProtoSparseListImplementation* node) { return node ? node->height : 0; }
        int getBalance(const ProtoSparseListImplementation* node) {
            if (!node) return 0;
            return getHeight(node->next) - getHeight(node->previous);
        }

        const ProtoSparseListImplementation* rightRotate(ProtoContext* context, const ProtoSparseListImplementation* y) {
            const ProtoSparseListImplementation* x = y->previous;
            const ProtoSparseListImplementation* T2 = x->next;
            auto* new_y = new(context) ProtoSparseListImplementation(context, y->key, y->value, T2, y->next);
            return new(context) ProtoSparseListImplementation(context, x->key, x->value, x->previous, new_y);
        }

        const ProtoSparseListImplementation* leftRotate(ProtoContext* context, const ProtoSparseListImplementation* x) {
            const ProtoSparseListImplementation* y = x->next;
            const ProtoSparseListImplementation* T2 = y->previous;
            auto* new_x = new(context) ProtoSparseListImplementation(context, x->key, x->value, x->previous, T2);
            return new(context) ProtoSparseListImplementation(context, y->key, y->value, new_x, y->next);
        }

        const ProtoSparseListImplementation* rebalance(ProtoContext* context, const ProtoSparseListImplementation* node) {
            const int balance = getBalance(node);
            if (balance < -1) {
                if (getBalance(node->previous) > 0) {
                    const ProtoSparseListImplementation* new_prev = leftRotate(context, node->previous);
                    const auto* new_node = new(context) ProtoSparseListImplementation(context, node->key, node->value, new_prev, node->next);
                    return rightRotate(context, new_node);
                }
                return rightRotate(context, node);
            }
            if (balance > 1) {
                if (getBalance(node->next) < 0) {
                    const ProtoSparseListImplementation* new_next = rightRotate(context, node->next);
                    const auto* new_node = new(context) ProtoSparseListImplementation(context, node->key, node->value, node->previous, new_next);
                    return leftRotate(context, new_node);
                }
                return leftRotate(context, node);
            }
            return node;
        }
    }

    bool ProtoSparseListImplementation::implHas(ProtoContext* context, const unsigned long offset) const {
        return implGetAt(context, offset) != PROTO_NONE;
    }

    const ProtoObject* ProtoSparseListImplementation::implGetAt(ProtoContext* context, const unsigned long offset) const {
        const auto* node = this;
        while (node) {
            if (offset < node->key) {
                node = node->previous;
            } else if (offset > node->key) {
                node = node->next;
            } else {
                return node->value;
            }
        }
        return PROTO_NONE;
    }

    const ProtoSparseListImplementation* ProtoSparseListImplementation::implSetAt(ProtoContext* context, const unsigned long offset, const ProtoObject* newValue) const {
        if (this->value == PROTO_NONE && this->count == 0) {
            return new(context) ProtoSparseListImplementation(context, offset, newValue);
        }

        const ProtoSparseListImplementation* newNode;
        if (offset < this->key) {
            const ProtoSparseListImplementation* new_prev = this->previous ? this->previous->implSetAt(context, offset, newValue) : new(context) ProtoSparseListImplementation(context, offset, newValue);
            newNode = new(context) ProtoSparseListImplementation(context, this->key, this->value, new_prev, this->next);
        } else if (offset > this->key) {
            const ProtoSparseListImplementation* new_next = this->next ? this->next->implSetAt(context, offset, newValue) : new(context) ProtoSparseListImplementation(context, offset, newValue);
            newNode = new(context) ProtoSparseListImplementation(context, this->key, this->value, this->previous, new_next);
        } else {
            if (this->value == newValue) return this;
            newNode = new(context) ProtoSparseListImplementation(context, this->key, newValue, this->previous, this->next);
        }
        return rebalance(context, newNode);
    }

    const ProtoSparseListImplementation* ProtoSparseListImplementation::implRemoveAt(ProtoContext* context, const unsigned long offset) const {
        if (this->value == PROTO_NONE && this->count == 0) return this;

        const ProtoSparseListImplementation* newNode;
        if (offset < this->key) {
            if (!this->previous) return this;
            auto* new_prev = this->previous->implRemoveAt(context, offset);
            newNode = new(context) ProtoSparseListImplementation(context, this->key, this->value, new_prev, this->next);
        } else if (offset > this->key) {
            if (!this->next) return this;
            auto* new_next = this->next->implRemoveAt(context, offset);
            newNode = new(context) ProtoSparseListImplementation(context, this->key, this->value, this->previous, new_next);
        } else {
            if (!this->previous) return this->next;
            if (!this->next) return this->previous;

            const ProtoSparseListImplementation* successor = this->next;
            while (successor->previous) successor = successor->previous;
            
            auto* new_next = this->next->implRemoveAt(context, successor->key);
            newNode = new(context) ProtoSparseListImplementation(context, successor->key, successor->value, this->previous, new_next);
        }
        return rebalance(context, newNode);
    }

    unsigned long ProtoSparseListImplementation::implGetSize(ProtoContext* context) const { return this->count; }

    bool ProtoSparseListImplementation::implIsEqual(ProtoContext* context, const ProtoSparseListImplementation* otherDict) const {
        if (this->count != otherDict->count) return false;
        auto it = this->implGetIterator(context);
        while (it->implHasNext(context)) {
            const auto key = it->implNextKey(context);
            if (otherDict->implGetAt(context, key) != this->implGetAt(context, key)) return false;
            it = it->implAdvance(context);
        }
        return true;
    }

    void ProtoSparseListImplementation::implProcessElements(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, unsigned long, const ProtoObject*)) const {
        if (this->previous) this->previous->implProcessElements(context, self, method);
        if (this->value != PROTO_NONE) method(context, self, this->key, this->value);
        if (this->next) this->next->implProcessElements(context, self, method);
    }

    void ProtoSparseListImplementation::implProcessValues(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const ProtoObject*)) const {
        if (this->previous) this->previous->implProcessValues(context, self, method);
        if (this->value != PROTO_NONE) method(context, self, this->value);
        if (this->next) this->next->implProcessValues(context, self, method);
    }

    void ProtoSparseListImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override {
        if (this->previous) method(context, self, this->previous);
        if (this->next) method(context, self, this->next);
        if (this->value && this->value->isCell(context)) method(context, self, this->value->asCell(context));
    }

    const ProtoObject* ProtoSparseListImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.sparseListImplementation = this;
        p.op.pointer_tag = POINTER_TAG_SPARSE_LIST;
        return p.oid.oid;
    }

    const ProtoSparseListIteratorImplementation* ProtoSparseListImplementation::implGetIterator(ProtoContext* context) const {
        const auto* node = this;
        const ProtoSparseListIteratorImplementation* queue = nullptr;
        while (node && node->previous) {
            queue = new(context) ProtoSparseListIteratorImplementation(context, ITERATOR_NEXT_NEXT, node, queue);
            node = node->previous;
        }
        return new(context) ProtoSparseListIteratorImplementation(context, ITERATOR_NEXT_THIS, node, queue);
    }

    unsigned long ProtoSparseListImplementation::getHash(ProtoContext* context) const override { return this->hash; }

    void ProtoSparseListImplementation::finalize(ProtoContext* context) const override {}
}
