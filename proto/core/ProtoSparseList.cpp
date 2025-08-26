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

    // Modernized constructor with initialization list.
    ProtoSparseListIteratorImplementation::ProtoSparseListIteratorImplementation(
        ProtoContext* context,
        int state,
        ProtoSparseListImplementation* current,
        ProtoSparseListIteratorImplementation* queue
    ) : Cell(context), state(state), current(current), queue(queue)
    {
    }

    // Default destructor.
    ProtoSparseListIteratorImplementation::~ProtoSparseListIteratorImplementation() = default;

    int ProtoSparseListIteratorImplementation::implHasNext(ProtoContext* context)
    {
        // The original logic is complex but is maintained.
        // An iterator has a "next" if it is in a valid state or if the iterator queue has elements.
        if (this->state == ITERATOR_NEXT_PREVIOUS && this->current && this->current->previous)
            return true;
        if (this->state == ITERATOR_NEXT_THIS && this->current)
            return true;
        if (this->state == ITERATOR_NEXT_NEXT && this->current && this->current->next)
            return true;
        if (this->queue)
            return this->queue->implHasNext(context);

        return false;
    }

    unsigned long ProtoSparseListIteratorImplementation::implNextKey(ProtoContext* context)
    {
        if (this->state == ITERATOR_NEXT_THIS && this->current)
        {
            return this->current->index;
        }
        return 0;
    }

    ProtoObject* ProtoSparseListIteratorImplementation::implNextValue(ProtoContext* context)
    {
        if (this->state == ITERATOR_NEXT_THIS && this->current)
        {
            return this->current->value;
        }
        return PROTO_NONE;
    }

    ProtoSparseListIteratorImplementation* ProtoSparseListIteratorImplementation::implAdvance(ProtoContext* context)
    {
        // The advance logic is complex, creating a new chain of iterators
        // to maintain the state of the in-order traversal.
        if (this->state == ITERATOR_NEXT_PREVIOUS)
        {
            return new(context) ProtoSparseListIteratorImplementation(
                context,
                ITERATOR_NEXT_THIS,
                this->current,
                this->queue
            );
        }

        if (this->state == ITERATOR_NEXT_THIS)
        {
            if (this->current && this->current->next)
            {
                // If there is a right subtree, the next is the first element of that subtree.
                return dynamic_cast<ProtoSparseListIteratorImplementation*>(this->current->next->
                    implGetIterator(context));
            }
            if (this->queue)
            {
                // If there is no right subtree, the next is the parent in the queue.
                return static_cast<ProtoSparseListIteratorImplementation*>(this->queue->implAdvance(context));
            }
            return nullptr; // End of iteration.
        }

        if (this->state == ITERATOR_NEXT_NEXT && this->queue)
        {
            return static_cast<ProtoSparseListIteratorImplementation*>(this->queue->implAdvance(context));
        }

        return nullptr;
    }

    ProtoObject* ProtoSparseListIteratorImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = reinterpret_cast<ProtoObject*>(this);
        p.op.pointer_tag = POINTER_TAG_SPARSE_LIST_ITERATOR;
        return p.oid.oid;
    }

    void ProtoSparseListIteratorImplementation::finalize(ProtoContext* context)
    {
    };

    void ProtoSparseListIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, Cell* cell)
    )
    {
        // Inform the GC about internal references.
        if (this->current)
        {
            method(context, self, this->current);
        }
        if (this->queue)
        {
            method(context, self, this->queue);
        }
    }

    unsigned long ProtoSparseListIteratorImplementation::getHash(ProtoContext* context)
    {
        return Cell::getHash(context);
    }


    // --- ProtoSparseListImplementation ---

    // Modernized constructor.
    ProtoSparseListImplementation::ProtoSparseListImplementation(
        ProtoContext* context,
        const unsigned long index,
        ProtoObject* value,
        ProtoSparseListImplementation* previous,
        ProtoSparseListImplementation* next
    ) : Cell(context), previous(previous), next(next), index(index), value(value)
    {
        // Calculate hash, count, and height after initializing members.
        this->hash = index ^
            (value ? value->getHash(context) : 0) ^
            (previous ? previous->hash : 0) ^
            (next ? next->hash : 0);

        this->count = (value != PROTO_NONE ? 1 : 0) +
            (previous ? previous->count : 0) +
            (next ? next->count : 0);

        unsigned long previous_height = previous ? previous->height : 0;
        unsigned long next_height = next ? next->height : 0;
        this->height = 1 + std::max(previous_height, next_height);
    }

    ProtoSparseListImplementation::~ProtoSparseListImplementation() = default;

    // --- AVL Tree Logic (Corrected) ---

    namespace
    {
        // Anonymous helper functions for tree logic.

        int getHeight(const ProtoSparseListImplementation* node)
        {
            return node ? node->height : 0;
        }

        int getBalance(const ProtoSparseListImplementation* node)
        {
            if (!node) return 0;
            return getHeight(node->next) - getHeight(node->previous);
        }

        ProtoSparseListImplementation* rightRotate(ProtoContext* context, ProtoSparseListImplementation* y)
        {
            ProtoSparseListImplementation* x = y->previous;
            ProtoSparseListImplementation* T2 = x->next;

            // Perform rotation
            auto* new_y = new(context) ProtoSparseListImplementation(
                context, y->index, y->value, T2, y->next);
            return new(context) ProtoSparseListImplementation(context, x->index, x->value, x->previous, new_y);
        }

        ProtoSparseListImplementation* leftRotate(ProtoContext* context, const ProtoSparseListImplementation* x)
        {
            ProtoSparseListImplementation* y = x->next;
            ProtoSparseListImplementation* T2 = y->previous;

            // Perform rotation
            auto* new_x = new(context) ProtoSparseListImplementation(
                context, x->index, x->value, x->previous, T2);
            return new(context) ProtoSparseListImplementation(context, y->index, y->value, new_x, y->next);
        }

        // CRITICAL FIX: The rebalancing logic was broken.
        // This is a standard and correct implementation for an AVL tree.
        ProtoSparseListImplementation* rebalance(ProtoContext* context, ProtoSparseListImplementation* node)
        {
            if (!node) return nullptr;

            const int balance = getBalance(node);

            // Case 1: Left-Left (LL)
            if (balance < -1 && getBalance(node->previous) <= 0)
            {
                return rightRotate(context, node);
            }
            // Case 2: Right-Right (RR)
            if (balance > 1 && getBalance(node->next) >= 0)
            {
                return leftRotate(context, node);
            }
            // Case 3: Left-Right (LR)
            if (balance < -1 && getBalance(node->previous) > 0)
            {
                ProtoSparseListImplementation* new_prev = leftRotate(context, node->previous);
                auto* new_node = new(context) ProtoSparseListImplementation(
                    context, node->index, node->value, new_prev, node->next);
                return rightRotate(context, new_node);
            }
            // Case 4: Right-Left (RL)
            if (balance > 1 && getBalance(node->next) < 0)
            {
                ProtoSparseListImplementation* new_next = rightRotate(context, node->next);
                auto* new_node = new(context) ProtoSparseListImplementation(
                    context, node->index, node->value, node->previous, new_next);
                return leftRotate(context, new_node);
            }

            return node; // The node is already balanced.
        }
    } // end of anonymous namespace

    // --- Public Interface Methods ---

    bool ProtoSparseListImplementation::implHas(ProtoContext* context, const unsigned long index)
    {
        // The search is more efficient with a non-constant pointer.
        const ProtoSparseListImplementation* node = this;
        while (node)
        {
            if (node->index == index)
            {
                return node->value != PROTO_NONE;
            }
            // CRITICAL FIX: The comparison was incorrect.
            if (index < node->index)
            {
                node = node->previous;
            }
            else
            {
                node = node->next;
            }
        }
        return false;
    }

    ProtoObject* ProtoSparseListImplementation::implGetAt(ProtoContext* context, const unsigned long index)
    {
        const ProtoSparseListImplementation* node = this;
        while (node)
        {
            if (node->index == index)
            {
                return node->value;
            }
            // CRITICAL FIX: The comparison was incorrect.
            if (index < node->index)
            {
                node = node->previous;
            }
            else
            {
                node = node->next;
            }
        }
        return PROTO_NONE;
    }

    ProtoSparseListImplementation* ProtoSparseListImplementation::implSetAt(
        ProtoContext* context, const unsigned long index,
        ProtoObject* value)
    {
        ProtoSparseListImplementation* newNode;

        // Base case: empty tree or leaf node.
        if (this->value == PROTO_NONE && this->count == 0)
        {
            return new(context) ProtoSparseListImplementation(context, index, value);
        }

        if (index < this->index)
        {
            ProtoSparseListImplementation* new_prev = this->previous
                                                          ? dynamic_cast<ProtoSparseListImplementation*>(this->previous
                                                              ->
                                                              implSetAt(context, index, value))
                                                          : new(context) ProtoSparseListImplementation(
                                                              context, index, value);
            newNode = new(context) ProtoSparseListImplementation(context, this->index, this->value, new_prev,
                                                                 this->next);
        }
        else if (index > this->index)
        {
            ProtoSparseListImplementation* new_next = this->next
                                                          ? dynamic_cast<ProtoSparseListImplementation*>(this->next->
                                                              implSetAt(context, index, value))
                                                          : new(context) ProtoSparseListImplementation(
                                                              context, index, value);
            newNode = new(context) ProtoSparseListImplementation(context, this->index, this->value, this->previous,
                                                                 new_next);
        }
        else
        {
            // index == this->index
            // If the value is the same, do nothing.
            if (this->value == value) return this;
            // Replace the value in the current node.
            newNode = new(context) ProtoSparseListImplementation(context, this->index, value, this->previous,
                                                                 this->next);
        }

        return rebalance(context, newNode);
    }

    ProtoSparseListImplementation* ProtoSparseListImplementation::implRemoveAt(
        ProtoContext* context, const unsigned long index)
    {
        if (this->value == PROTO_NONE && this->count == 0)
        {
            return this; // The element was not found.
        }

        ProtoSparseListImplementation* newNode;

        if (index < this->index)
        {
            if (!this->previous) return this; // Not found.
            auto* new_prev = dynamic_cast<ProtoSparseListImplementation*>(this->previous->
                implRemoveAt(context, index));
            newNode = new(context) ProtoSparseListImplementation(context, this->index, this->value, new_prev,
                                                                 this->next);
        }
        else if (index > this->index)
        {
            if (!this->next) return this; // Not found.
            auto* new_next = dynamic_cast<ProtoSparseListImplementation*>(this->next->implRemoveAt(
                context, index));
            newNode = new(context) ProtoSparseListImplementation(context, this->index, this->value, this->previous,
                                                                 new_next);
        }
        else
        {
            // index == this->index
            // Node found. Deletion logic.
            if (!this->previous) return this->next; // Case with 0 or 1 child (right).
            if (!this->next) return this->previous; // Case with 1 child (left).

            // Case with 2 children: find the in-order successor (the smallest in the right subtree).
            const ProtoSparseListImplementation* successor = this->next;
            while (successor->previous)
            {
                successor = successor->previous;
            }
            // Remove the successor from the right subtree.
            auto* new_next = dynamic_cast<ProtoSparseListImplementation*>(this->next->implRemoveAt(
                context, successor->index));
            // Replace this node with the successor.
            newNode = new(context) ProtoSparseListImplementation(context, successor->index, successor->value,
                                                                 this->previous, new_next);
        }

        return rebalance(context, newNode);
    }

    unsigned long ProtoSparseListImplementation::implGetSize(ProtoContext* context)
    {
        return this->count;
    }

    bool ProtoSparseListImplementation::implIsEqual(ProtoContext* context, ProtoSparseListImplementation* otherDict)
    {


        if (this->count != otherDict->count) return false;
        auto it = this->implGetIterator(context);
        while (it->implHasNext(context))
        {
            auto key = it->implNextKey(context);
            auto value = it->implNextValue(context);
            if (!otherDict->getAt(context, key)) return false;
            if (otherDict->getAt(context, key) != this->getAt(context, key)) return false;
            it->implAdvance(context);
        }
        return true;
    }

    void ProtoSparseListImplementation::implProcessElements(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, unsigned long index, ProtoObject* value)
    )
    {
        // In-order traversal to process elements.
        if (this->previous)
        {
            this->previous->implProcessElements(context, self, method);
        }
        if (this->value != PROTO_NONE)
        {
            method(context, self, this->index, this->value);
        }
        if (this->next)
        {
            this->next->implProcessElements(context, self, method);
        }
    }

    void ProtoSparseListImplementation::implProcessValues(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, ProtoObject* value)
    )
    {
        // In-order traversal to process only the values.
        if (this->previous)
        {
            this->previous->implProcessValues(context, self, method);
        }
        if (this->value != PROTO_NONE)
        {
            method(context, self, this->value);
        }
        if (this->next)
        {
            this->next->implProcessValues(context, self, method);
        }
    }

    void ProtoSparseListImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, Cell* cell)
    )
    {
        if (this->previous)
        {
            method(context, self, this->previous);
        }
        if (this->next)
        {
            method(context, self, this->next);
        }
        if (this->value && this->value->isCell(context))
        {
            method(context, self, this->value->asCell(context));
        }
    }

    ProtoObject* ProtoSparseListImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = reinterpret_cast<ProtoObject*>(this);
        p.op.pointer_tag = POINTER_TAG_SPARSE_LIST;
        return p.oid.oid;
    }

    ProtoSparseListIteratorImplementation* ProtoSparseListImplementation::implGetIterator(ProtoContext* context)
    {
        // The iterator logic is complex, but the original structure is maintained.
        // Find the first node (the leftmost) and build the iterator queue.
        auto node = this;
        ProtoSparseListIteratorImplementation* queue = nullptr;
        while (node->previous)
        {
            queue = new(context) ProtoSparseListIteratorImplementation(context, ITERATOR_NEXT_NEXT, node, queue);
            node = node->previous;
        }
        return new(context) ProtoSparseListIteratorImplementation(context, ITERATOR_NEXT_THIS, node, queue);
    }

    unsigned long ProtoSparseListImplementation::getHash(ProtoContext* context)
    {
        return this->hash;
    }

    void ProtoSparseListImplementation::finalize(ProtoContext* context)
    {
    };

    // The getHash method is inherited from the base class Cell, which provides a hash
    // based on the address, which is sufficient and consistent.
} // namespace proto
