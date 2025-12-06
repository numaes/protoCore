/*
 * ProtoSparseList.cpp
 *
 *  Created on: 2017-05-01
 *      Author: gamarino
 *
 *  This file implements the immutable sparse list (dictionary) and its iterator.
 *  The structure is implemented as a persistent, self-balancing AVL tree, where
 *  the `key` is used for ordering. This guarantees that all operations
 *  (set, get, remove) have a time complexity of O(log n).
 */

#include "../headers/proto_internal.h"
#include <algorithm> // For std::max

namespace proto
{
    //=========================================================================
    // ProtoSparseListIteratorImplementation
    //=========================================================================

    /**
     * @class ProtoSparseListIteratorImplementation
     * @brief An iterator for traversing a `ProtoSparseList`.
     *
     * This iterator performs an in-order traversal of the underlying AVL tree.
     */
    ProtoSparseListIteratorImplementation::ProtoSparseListIteratorImplementation(
        ProtoContext* context,
        const ProtoSparseListImplementation* current,
        const ProtoSparseListIteratorImplementation* queue
    ) : Cell(context), current(current), queue(queue) {}


    ProtoSparseListIteratorImplementation::~ProtoSparseListIteratorImplementation() = default;

    int ProtoSparseListIteratorImplementation::implHasNext(ProtoContext* context) const
    {
        return this->current != nullptr;
    }

    unsigned long ProtoSparseListIteratorImplementation::implNextKey(ProtoContext* context) const
    {
        return this->current ? this->current->key : 0;
    }

    const ProtoObject* ProtoSparseListIteratorImplementation::implNextValue(ProtoContext* context) const
    {
        return this->current ? this->current->value : PROTO_NONE;
    }

    const ProtoSparseListIteratorImplementation* ProtoSparseListIteratorImplementation::implAdvance(ProtoContext* context) const
    {
        return this->queue;
    }

    const ProtoObject* ProtoSparseListIteratorImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.sparseListIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_SPARSE_LIST_ITERATOR;
        return p.oid.oid;
    }

    void ProtoSparseListIteratorImplementation::finalize(ProtoContext* context) const {}

    void ProtoSparseListIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const
    {
        if (this->current) method(context, self, this->current);
        if (this->queue) method(context, self, this->queue);
    }

    unsigned long ProtoSparseListIteratorImplementation::getHash(ProtoContext* context) const
    {
        return Cell::getHash(context);
    }


    //=========================================================================
    // ProtoSparseListImplementation
    //=========================================================================

    /**
     * @class ProtoSparseListImplementation
     * @brief An immutable, persistent dictionary (key-value map) implemented as an AVL tree.
     */

    ProtoSparseListImplementation::ProtoSparseListImplementation(
        ProtoContext* context,
        unsigned long key,
        const ProtoObject* value,
        const ProtoSparseListImplementation* previous,
        const ProtoSparseListImplementation* next,
        bool isEmpty
    ) : Cell(context),
        key(key),
        value(value),
        previous(previous),
        next(next),
        hash(isEmpty ? 0 : key ^ (value ? value->getHash(context) : 0) ^ (previous ? previous->hash : 0) ^ (next ? next->hash : 0)),
        size(isEmpty ? 0 : 1 + (previous ? previous->size : 0) + (next ? next->size : 0)),
        height(isEmpty ? 0 : 1 + std::max(previous ? previous->height : 0, next ? next->height : 0)),
        isEmpty(isEmpty)
    {
    }

    ProtoSparseListImplementation::~ProtoSparseListImplementation() = default;

    // --- Anonymous Namespace for AVL Tree Helpers ---
    namespace {
        int getHeight(const ProtoSparseListImplementation* node) { return node ? node->height : 0; }
        int getBalance(const ProtoSparseListImplementation* node) {
            if (!node || node->isEmpty) return 0;
            return getHeight(node->previous) - getHeight(node->next);
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
            if (balance > 1) { // Left heavy
                if (getBalance(node->previous) < 0) { // Left-Right Case
                    const ProtoSparseListImplementation* new_prev = leftRotate(context, node->previous);
                    const auto* new_node = new(context) ProtoSparseListImplementation(context, node->key, node->value, new_prev, node->next);
                    return rightRotate(context, new_node);
                }
                return rightRotate(context, node); // Left-Left Case
            }
            if (balance < -1) { // Right heavy
                if (getBalance(node->next) > 0) { // Right-Left Case
                    const ProtoSparseListImplementation* new_next = rightRotate(context, node->next);
                    const auto* new_node = new(context) ProtoSparseListImplementation(context, node->key, node->value, node->previous, new_next);
                    return leftRotate(context, new_node);
                }
                return leftRotate(context, node); // Right-Right Case
            }
            return node; // Already balanced
        }
    }

    bool ProtoSparseListImplementation::implHas(ProtoContext* context, const unsigned long offset) const {
        return implGetAt(context, offset) != PROTO_NONE;
    }

    const ProtoObject* ProtoSparseListImplementation::implGetAt(ProtoContext* context, const unsigned long offset) const {
        const auto* node = this;
        while (node && !node->isEmpty) {
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
        if (this->isEmpty) {
            return new(context) ProtoSparseListImplementation(context, offset, newValue, nullptr, nullptr, false);
        }

        const ProtoSparseListImplementation* newNode;
        if (offset < this->key) {
            const ProtoSparseListImplementation* new_prev = this->previous ? this->previous->implSetAt(context, offset, newValue) : new(context) ProtoSparseListImplementation(context, offset, newValue, nullptr, nullptr, false);
            newNode = new(context) ProtoSparseListImplementation(context, this->key, this->value, new_prev, this->next, false);
        } else if (offset > this->key) {
            const ProtoSparseListImplementation* new_next = this->next ? this->next->implSetAt(context, offset, newValue) : new(context) ProtoSparseListImplementation(context, offset, newValue, nullptr, nullptr, false);
            newNode = new(context) ProtoSparseListImplementation(context, this->key, this->value, this->previous, new_next, false);
        } else {
            if (this->value == newValue) return this; // No change
            newNode = new(context) ProtoSparseListImplementation(context, this->key, newValue, this->previous, this->next, false);
        }
        return rebalance(context, newNode);
    }

    const ProtoSparseListImplementation* findMin(const ProtoSparseListImplementation* node) {
        while (node && node->previous && !node->previous->isEmpty) {
            node = node->previous;
        }
        return node;
    }

    const ProtoSparseListImplementation* ProtoSparseListImplementation::implRemoveAt(ProtoContext* context, const unsigned long offset) const {
        if (this->isEmpty) return this;

        const ProtoSparseListImplementation* newNode;
        if (offset < this->key) {
            if (!this->previous) return this;
            auto* new_prev = this->previous->implRemoveAt(context, offset);
            newNode = new(context) ProtoSparseListImplementation(context, this->key, this->value, new_prev, this->next, false);
        } else if (offset > this->key) {
            if (!this->next) return this;
            auto* new_next = this->next->implRemoveAt(context, offset);
            newNode = new(context) ProtoSparseListImplementation(context, this->key, this->value, this->previous, new_next, false);
        } else {
            // Node to delete is found
            if (!this->previous || this->previous->isEmpty) return this->next;
            if (!this->next || this->next->isEmpty) return this->previous;

            // Node with two children: get the in-order successor (smallest in the right subtree)
            const ProtoSparseListImplementation* successor = findMin(this->next);
            auto* new_next = this->next->implRemoveAt(context, successor->key);
            newNode = new(context) ProtoSparseListImplementation(context, successor->key, successor->value, this->previous, new_next, false);
        }
        return rebalance(context, newNode);
    }

    unsigned long ProtoSparseListImplementation::implGetSize(ProtoContext* context) const { return this->size; }

    bool ProtoSparseListImplementation::implIsEqual(ProtoContext* context, const ProtoSparseListImplementation* otherDict) const {
        if (this->size != otherDict->size) return false;
        auto it = this->implGetIterator(context);
        while (it->implHasNext(context)) {
            const auto key = it->implNextKey(context);
            if (otherDict->implGetAt(context, key) != this->implGetAt(context, key)) return false;
            it = it->implAdvance(context);
        }
        return true;
    }

    void ProtoSparseListImplementation::implProcessElements(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, unsigned long, const ProtoObject*)) const {
        if (this->isEmpty) return;
        if (this->previous) this->previous->implProcessElements(context, self, method);
        method(context, self, this->key, this->value);
        if (this->next) this->next->implProcessElements(context, self, method);
    }

    void ProtoSparseListImplementation::implProcessValues(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const ProtoObject*, const Cell*)) const {
        if (this->isEmpty) return;
        if (this->previous) this->previous->implProcessValues(context, self, method);
        method(context, self, this->value, this);
        if (this->next) this->next->implProcessValues(context, self, method);
    }

    void ProtoSparseListImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (this->isEmpty) return;
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
        const ProtoSparseListImplementation* node = this;
        const ProtoSparseListIteratorImplementation* stack = nullptr;
        while (node && !node->isEmpty) {
            stack = new(context) ProtoSparseListIteratorImplementation(context, node, stack);
            node = node->previous;
        }
        return stack;
    }

    unsigned long ProtoSparseListImplementation::getHash(ProtoContext* context) const { return this->hash; }

    void ProtoSparseListImplementation::finalize(ProtoContext* context) const {}

    const ProtoSparseList* ProtoSparseListImplementation::asSparseList(ProtoContext* context) const {
        return (const ProtoSparseList*)this->implAsObject(context);
    }

    // --- Public API Trampolines ---

    const ProtoSparseList* ProtoSparseList::removeAt(ProtoContext* context, unsigned long index) const {
        return toImpl<const ProtoSparseListImplementation>(this)->implRemoveAt(context, index)->asSparseList(context);
    }
}
