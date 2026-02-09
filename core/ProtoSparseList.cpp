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
        return (state == ITERATOR_NEXT_THIS && current) ? current->value : PROTO_NONE;
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
        if (current) method(context, self, current);
        if (queue) method(context, self, queue);
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
          hash(empty ? 0 : k ^ (v ? v->getHash(context) : 0) ^ (p ? p->hash : 0) ^ (n ? n->hash : 0)),
          size(empty ? 0 : (v != PROTO_NONE) + get_node_size(p) + get_node_size(n)),
          height(empty ? 0 : 1 + std::max(get_node_height(p), get_node_height(n))),
          isEmpty(empty) {}

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
        if (newValue == PROTO_NONE) {
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
                    return new(context) ProtoSparseListImplementation(context, 0, PROTO_NONE, nullptr, nullptr, true);
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
            return new(context) ProtoSparseListImplementation(context, 0, PROTO_NONE, nullptr, nullptr, true);
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
        if (value && value->isCell(context)) method(context, self, value->asCell(context));
        if (previous) method(context, self, previous);
        if (next) method(context, self, next);
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
