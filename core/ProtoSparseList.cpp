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

    // Modernized constructor with an initialization list.
    ProtoSparseListIteratorImplementation::ProtoSparseListIteratorImplementation(
        ProtoContext* context,
        int state,
        const ProtoSparseListImplementation* current,
        const ProtoSparseListIteratorImplementation* queue
    ) : Cell(context), state(state), current(current), queue(queue), Cell(context)
    {
    }
 
    // Default destructor.
    ProtoSparseListIteratorImplementation::~ProtoSparseListIteratorImplementation() = default;

    int ProtoSparseListIteratorImplementation::implHasNext(ProtoContext* context) const
    {
        // The original logic is complex but is maintained.
        // An iterator has a "next" if it is in a valid state or if the iterator queue has elements.
        if (this->state == ITERATOR_NEXT_PREVIOUS && this->current && this->current->previous)
            return true;
        if (this->state == ITERATOR_NEXT_THIS && this->current && this->current->value != PROTO_NONE)
            return true;
        if (this->state == ITERATOR_NEXT_NEXT && this->current && this->current->next)
            return true;
        if (this->queue)
            return this->queue->implHasNext(context);

        return false;
    }

    unsigned long ProtoSparseListIteratorImplementation::implNextKey(ProtoContext* context) const
    {
        if (this->state == ITERATOR_NEXT_THIS && this->current)
        {
            return this->current->index;
        }
        return 0;
    }

    const ProtoObject ProtoSparseListIteratorImplementation::implNextValue(ProtoContext* context) const
    {
        if (this->state == ITERATOR_NEXT_THIS && this->current)
        {
            return this->current->value;
        }
        return PROTO_NONE;
    }

    const ProtoSparseListIteratorImplementation* ProtoSparseListIteratorImplementation::implAdvance(ProtoContext* context) const
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
                return this->current->next->implGetIterator(context);
            }
            if (this->queue)
            {
                // If there is no right subtree, the next is the parent in the queue.
                return this->queue->implAdvance(context);
            }
            return nullptr; // End of iteration.
        }

        if (this->state == ITERATOR_NEXT_NEXT && this->queue)
        {
            return this->queue->implAdvance(context);
        }

        return nullptr;
    }

    const ProtoObject* ProtoSparseListIteratorImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = reinterpret_cast<const ProtoObject>(this);
        p.op.pointer_tag = POINTER_TAG_SPARSE_LIST_ITERATOR;
        return p.oid.oid;
    }

    void ProtoSparseListIteratorImplementation::finalize(ProtoContext* context) const override
    {
    };

    void ProtoSparseListIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, Cell* cell)
    ) const override
    {
        // Inform the GC about internal references.
        if (this->current)
        {
            method(context, self, const_cast<ProtoSparseListImplementation*>(this->current));
        }
        if (this->queue)
        {
            method(context, self, const_cast<ProtoSparseListIteratorImplementation*>(this->queue));
        }
    }

    unsigned long ProtoSparseListIteratorImplementation::getHash(ProtoContext* context) const override
    {
        return Cell::getHash(context);
    }


    // --- ProtoSparseListImplementation ---

    // Modernized constructor.
    ProtoSparseListImplementation::ProtoSparseListImplementation(
        ProtoContext* context,
        unsigned long index,
        const ProtoObject* value,
        const ProtoSparseListImplementation* previous,
        const ProtoSparseListImplementation* next
    ) : Cell(context), index(index), value(value), previous(previous), next(next)
    {
        // Calculate hash, count, and height after initializing members.
        this->hash = index ^
            (value ? value->getHash(context) : 0) ^
            (previous ? previous->hash : 0) ^
            (next ? next->hash : 0);

        this->count = (value != PROTO_NONE ? 1 : 0) +
            (previous ? previous->count : 0) +
            (next ? next->count : 0);

        const unsigned long previous_height = previous ? previous->height : 0;
        const unsigned long next_height = next ? next->height : 0;
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

        const ProtoSparseListImplementation* rightRotate(ProtoContext* context, const ProtoSparseListImplementation* y)
        {
            const ProtoSparseListImplementation* x = y->previous;
            const ProtoSparseListImplementation* T2 = x->next;

            // Perform rotation
            auto* new_y = new(context) ProtoSparseListImplementation(
                context, y->index, y->value, T2, y->next);
            return new(context) ProtoSparseListImplementation(context, x->index, x->value, x->previous, new_y);
        }

        const ProtoSparseListImplementation* leftRotate(ProtoContext* context, const ProtoSparseListImplementation* x)
        {
            const ProtoSparseListImplementation* y = x->next;
            const ProtoSparseListImplementation* T2 = y->previous;

            // Perform rotation
            auto* new_x = new(context) ProtoSparseListImplementation(
                context, x->index, x->value, x->previous, T2);
            return new(context) ProtoSparseListImplementation(context, y->index, y->value, new_x, y->next);
        }

        // CRITICAL FIX: The rebalancing logic was broken.
        // This is a standard and correct implementation for an AVL tree.
        const ProtoSparseListImplementation* rebalance(ProtoContext* context, const ProtoSparseListImplementation* node)
        {
            const int balance = getBalance(node);

            // Case 1: Left-Left (LL)
            if (balance < -1 && getBalance(node->previous) <= 0)
            {
                return rightRotate(context, const_cast<ProtoSparseListImplementation*>(node));
            }
            // Case 2: Right-Right (RR)
            if (balance > 1 && getBalance(node->next) >= 0)
            {
                return leftRotate(context, const_cast<ProtoSparseListImplementation*>(node));
            }
            // Case 3: Left-Right (LR)
            if (balance < -1 && getBalance(node->previous) > 0)
            {
                const ProtoSparseListImplementation* new_prev = leftRotate(context, node->previous);
                const auto* new_node = new(context) ProtoSparseListImplementation(
                    context, node->index, node->value, new_prev, node->next);
                return rightRotate(context, new_node);
            }
            // Case 4: Right-Left (RL)
            if (balance > 1 && getBalance(node->next) < 0)
            {
                const ProtoSparseListImplementation* new_next = rightRotate(context, node->next);
                const auto* new_node = new(context) ProtoSparseListImplementation(
                    context, node->index, node->value, node->previous, new_next);
                return leftRotate(context, new_node);
            }

            return node; // The node is already balanced.
        }
    } // end of anonymous namespace

    // --- Public Interface Methods ---

    bool ProtoSparseListImplementation::implHas(ProtoContext* context, const unsigned long offset) const
    {
        // The search is more efficient with a non-constant pointer.
        const auto* node = this;
        while (node)
        {
            if (node->index == offset)
            {
                return node->value != PROTO_NONE;
            }
            // CRITICAL FIX: The comparison was incorrect.
            if (offset < node->index)
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

    const ProtoObject* ProtoSparseListImplementation::implGetAt(ProtoContext* context, const unsigned long offset) const
    {
        const auto* node = this;
        while (node)
        {
            if (node->index == offset)
            {
                return node->value;
            }
            // CRITICAL FIX: The comparison was incorrect.
            if (offset < node->index)
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

    const ProtoSparseListImplementation* ProtoSparseListImplementation::implSetAt(
        ProtoContext* context, const unsigned long offset,
        const ProtoObject* newValue)
    {
        const ProtoSparseListImplementation* newNode;

        // Base case: empty tree or leaf node.
        if (this->value == PROTO_NONE && this->count == 0)
        {
            return new(context) ProtoSparseListImplementation(context, offset, newValue);
        }

        if (offset < this->index)
        {
            const ProtoSparseListImplementation* new_prev = this->previous
                                                          ? this->previous->implSetAt(context, offset, newValue)
                                                          : new(context) ProtoSparseListImplementation(
                                                              context, offset, newValue);
            newNode = new(context) ProtoSparseListImplementation(context, this->index, this->value, new_prev,
                                                                 this->next);
        }
        else if (offset > this->index)
        {
            const ProtoSparseListImplementation* new_next = this->next
                                                          ? this->next->implSetAt(context, offset, newValue)
                                                          : new(context) ProtoSparseListImplementation(
                                                              context, offset, newValue);
            newNode = new(context) ProtoSparseListImplementation(context, this->index, this->value, this->previous,
                                                                 new_next);
        }
        else
        {
            // index == this->index
            // If the value is the same, do nothing.
            if (this->value == newValue) return this;
            // Replace the value in the current node.
            newNode = new(context) ProtoSparseListImplementation(context, this->index, newValue, this->previous,
                                                                 this->next);
        }

        return rebalance(context, newNode);
    }

    const ProtoSparseListImplementation* ProtoSparseListImplementation::implRemoveAt(
        ProtoContext* context, const unsigned long offset)
    {
        if (this->value == PROTO_NONE && this->count == 0)
        {
            return this; // The element was not found.
        }

        const ProtoSparseListImplementation* newNode;

        if (offset < this->index)
        {
            if (!this->previous) return this; // Not found.
            auto* new_prev = this->previous->implRemoveAt(context, offset);
            newNode = new(context) ProtoSparseListImplementation(context, this->index, this->value, new_prev,
                                                                 this->next);
        }
        else if (offset > this->index)
        {
            if (!this->next) return this; // Not found.
            auto* new_next = this->next->implRemoveAt(context, offset);
            newNode = new(context) ProtoSparseListImplementation(context, this->index, this->value, this->previous,
                                                                 new_next);
        }
        else
        {
            // index == this->index
            // Node found. Deletion logic.
            if (!this->previous) return this->next; // Case with 0 or 1 children (right).
            if (!this->next) return this->previous; // Case with 1 child (left).

            // Case with 2 children: find the in-order successor (the smallest in the right subtree).
            const ProtoSparseListImplementation* successor = this->next;
            while (successor->previous)
            {
                successor = successor->previous;
            }
            // Remove the successor from the right subtree.
            auto* new_next = this->next->implRemoveAt(context, successor->index);
            // Replace this node with the successor.
            newNode = new(context) ProtoSparseListImplementation(context, successor->index, successor->value,
                                                                 this->previous, new_next);
        }

        return rebalance(context, newNode);
    }

    unsigned long ProtoSparseListImplementation::implGetSize(ProtoContext* context) const
    {
        return this->count;
    }

    bool ProtoSparseListImplementation::implIsEqual(ProtoContext* context, const ProtoSparseListImplementation* otherDict)
    {


        if (this->count != otherDict->count) return false;
        auto it = this->implGetIterator(context);
        while (it->implHasNext(context))
        {
            const auto key = it->implNextKey(context);
            if (!otherDict->implGetAt(context, key)) return false;
            if (otherDict->getAt(context, key) != this->getAt(context, key)) return false;
            it->implAdvance(context);
        }
        return true;
    }

    void ProtoSparseListImplementation::implProcessElements(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, unsigned long index, const ProtoObject* value)
    ) const
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
        void (*method)(ProtoContext* context, void* self, const ProtoObject* value)
    ) const
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
    ) const override
    {
        if (this->previous)
        {
            method(context, self, const_cast<ProtoSparseListImplementation*>(this->previous));
        }
        if (this->next)
        {
            method(context, self, const_cast<ProtoSparseListImplementation*>(this->next));
        }
        if (this->value && this->value->isCell(context))
        {
            method(context, self, this->value->asCell(context));
        }
    }

    const ProtoObject* ProtoSparseListImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = reinterpret_cast<const ProtoObject>(this);
        p.op.pointer_tag = POINTER_TAG_SPARSE_LIST;
        return p.oid.oid;
    }

    const ProtoSparseListIteratorImplementation* ProtoSparseListImplementation::implGetIterator(ProtoContext* context)
    {
        // The iterator logic is complex, but the original structure is maintained.
        // Find the first node (the leftmost) and build the iterator queue.
        const auto* node = this;
        const ProtoSparseListIteratorImplementation* queue = nullptr;
        while (node->previous)
        {
            queue = new(context) ProtoSparseListIteratorImplementation(context, ITERATOR_NEXT_NEXT, node, queue);
            node = node->previous;
        }
        return new(context) ProtoSparseListIteratorImplementation(context, ITERATOR_NEXT_THIS, node, queue);
    }

    unsigned long ProtoSparseListImplementation::getHash(ProtoContext* context) const override
    {
        return this->hash;
    }

    void ProtoSparseListImplementation::finalize(ProtoContext* context) const override
    {
    };

    // The getHash method is inherited from the base class Cell, which provides a hash
    // based on the address, which is enough and consistent.
} // namespace proto
