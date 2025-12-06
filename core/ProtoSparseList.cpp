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
        if (state == ITERATOR_NEXT_PREVIOUS && current && current->previous) return true;
        if (state == ITERATOR_NEXT_THIS && current && !current->isEmpty) return true;
        if (state == ITERATOR_NEXT_NEXT && current && current->next) return true;
        if (queue) return queue->implHasNext();
        return false;
    }

    unsigned long ProtoSparseListIteratorImplementation::implNextKey() const {
        return (state == ITERATOR_NEXT_THIS && current) ? current->key : 0;
    }

    const ProtoObject* ProtoSparseListIteratorImplementation::implNextValue() const {
        return (state == ITERATOR_NEXT_THIS && current) ? current->value : PROTO_NONE;
    }

    const ProtoSparseListIteratorImplementation* ProtoSparseListIteratorImplementation::implAdvance(ProtoContext* context) const {
        if (state == ITERATOR_NEXT_PREVIOUS) {
            return new(context) ProtoSparseListIteratorImplementation(context, ITERATOR_NEXT_THIS, current, queue);
        }
        if (state == ITERATOR_NEXT_THIS) {
            if (current && current->next) return current->next->implGetIterator(context);
            if (queue) return queue->implAdvance(context);
            return nullptr;
        }
        if (state == ITERATOR_NEXT_NEXT && queue) {
            return queue->implAdvance(context);
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
        if (current) method(context, self, current);
        if (queue) method(context, self, queue);
    }

    //=========================================================================
    // ProtoSparseListImplementation
    //=========================================================================
    ProtoSparseListImplementation::ProtoSparseListImplementation(ProtoContext* context, unsigned long k, const ProtoObject* v, const ProtoSparseListImplementation* p, const ProtoSparseListImplementation* n, bool empty)
        : Cell(context), key(k), value(v), previous(p), next(n),
          hash(empty ? 0 : k ^ (v ? v->getHash(context) : 0) ^ (p ? p->hash : 0) ^ (n ? n->hash : 0)),
          size(empty ? 0 : (v != PROTO_NONE) + (p ? p->size : 0) + (n ? n->size : 0)),
          height(empty ? 0 : 1 + std::max(p ? p->height : 0, n ? n->height : 0)),
          isEmpty(empty) {}

    namespace {
        int getHeight(const ProtoSparseListImplementation* node) { return node ? node->height : 0; }
        int getBalance(const ProtoSparseListImplementation* node) {
            if (!node || node->isEmpty) return 0;
            return getHeight(node->previous) - getHeight(node->next);
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
            if (balance > 1) {
                if (getBalance(node->previous) < 0) {
                    return rightRotate(context, new(context) ProtoSparseListImplementation(context, node->key, node->value, leftRotate(context, node->previous), node->next, false));
                }
                return rightRotate(context, node);
            }
            if (balance < -1) {
                if (getBalance(node->next) > 0) {
                    return leftRotate(context, new(context) ProtoSparseListImplementation(context, node->key, node->value, node->previous, rightRotate(context, node->next), false));
                }
                return leftRotate(context, node);
            }
            return node;
        }
    }

    bool ProtoSparseListImplementation::implHas(ProtoContext* context, unsigned long offset) const {
        return implGetAt(context, offset) != PROTO_NONE;
    }

    const ProtoObject* ProtoSparseListImplementation::implGetAt(ProtoContext* context, unsigned long offset) const {
        const auto* node = this;
        while (node && !node->isEmpty) {
            if (offset < node->key) node = node->previous;
            else if (offset > node->key) node = node->next;
            else return node->value;
        }
        return PROTO_NONE;
    }

    const ProtoSparseListImplementation* ProtoSparseListImplementation::implSetAt(ProtoContext* context, unsigned long offset, const ProtoObject* newValue) const {
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
        while (node && node->previous && !node->previous->isEmpty) node = node->previous;
        return node;
    }

    const ProtoSparseListImplementation* ProtoSparseListImplementation::implRemoveAt(ProtoContext* context, unsigned long offset) const {
        if (isEmpty) return this;
        const ProtoSparseListImplementation* newNode;
        if (offset < key) {
            if (!previous) return this;
            newNode = new(context) ProtoSparseListImplementation(context, key, value, previous->implRemoveAt(context, offset), next, false);
        } else if (offset > key) {
            if (!next) return this;
            newNode = new(context) ProtoSparseListImplementation(context, key, value, previous, next->implRemoveAt(context, offset), false);
        } else {
            if (!previous || previous->isEmpty) return next;
            if (!next || next->isEmpty) return previous;
            const ProtoSparseListImplementation* successor = findMin(next);
            newNode = new(context) ProtoSparseListImplementation(context, successor->key, successor->value, previous, next->implRemoveAt(context, successor->key), false);
        }
        return rebalance(context, newNode);
    }

    const ProtoSparseListIteratorImplementation* ProtoSparseListImplementation::implGetIterator(ProtoContext* context) const {
        const auto* node = this;
        const ProtoSparseListIteratorImplementation* queue = nullptr;
        while (node && !node->isEmpty) {
            queue = new(context) ProtoSparseListIteratorImplementation(context, ITERATOR_NEXT_NEXT, node, queue);
            node = node->previous;
        }
        if (queue) return new(context) ProtoSparseListIteratorImplementation(context, ITERATOR_NEXT_PREVIOUS, queue->current, queue->queue);
        return nullptr;
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
}
