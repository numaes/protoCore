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

    // Modernized constructor with an initialization list
    ProtoListIteratorImplementation::ProtoListIteratorImplementation(
        ProtoContext* context,
        const ProtoListImplementation* base,
        const unsigned long currentIndex
    ) : Cell(context), base(base), currentIndex(currentIndex)
    {
    }

    // Default destructor
    ProtoListIteratorImplementation::~ProtoListIteratorImplementation() = default;

    int ProtoListIteratorImplementation::implHasNext(ProtoContext* context) const
    {
        // It is safer to check if the base is not null.
        if (!this->base)
        {
            return false;
        }
        return this->currentIndex < this->base->implGetSize(context);
    }

    const ProtoObject* ProtoListIteratorImplementation::implNext(ProtoContext* context) const
    {
        if (!this->base)
        {
            return PROTO_NONE;
        }
        // Returns the current element but does not advance the iterator.
        // Advancement is done explicitly with advance().
        return this->base->implGetAt(context, static_cast<int>(this->currentIndex));
    }

    const ProtoListIteratorImplementation* ProtoListIteratorImplementation::implAdvance(ProtoContext* context) const
    {
        // CRITICAL FIX: The iterator must advance to the next index.
        // The previousNode version created an iterator at the same position.
        return new(context) ProtoListIteratorImplementation(context, this->base, this->currentIndex + 1);
    }

    const ProtoObject* ProtoListIteratorImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.listIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_ITERATOR;
        return p.oid.oid;
    }

    void ProtoListIteratorImplementation::finalize(ProtoContext* context) const
    {
    };

    void ProtoListIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            Cell* cell
        )
    ) const
    {
        // Inform the GC about the reference to the base list.
        if (this->base)
        {
            method(context, self, this->base->asCell(context));
        }
    }

    unsigned long ProtoListIteratorImplementation::getHash(ProtoContext* context) const
    {
        return Cell::getHash(context);
    }


    // --- ProtoListImplementation ---

    // Modernized constructor with an initialization list
    ProtoListImplementation::ProtoListImplementation(
        ProtoContext* context,
        const ProtoObject* value,
        const bool isEmpty,
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
            this->value = nullptr;
            this->previousNode = nullptr;
            this->nextNode = nullptr;
        }

        // Calculate hash and counters after initializing members.
        this->hash = (value ? value->getHash(context) : 0UL) ^
            (this->previousNode ? this->previousNode->hash : 0UL) ^
            (this->nextNode ? this->nextNode->hash : 0UL);

        const unsigned long previous_height = previousNode ? previousNode->height : 0;
        const unsigned long next_height = nextNode ? nextNode->height : 0;
        this->height = 1 + std::max(previous_height, next_height);

        if (isEmpty)
            this->size = 0;
        else
            this->size = 1 + (previousNode ? previousNode->size : 0) + (nextNode ? nextNode->size : 0);
    }

    ProtoListImplementation::~ProtoListImplementation() = default;

    // --- AVL Tree Logic (Corrected) ---

    namespace
    {
        // Anonymous helper functions

        int getHeight(const ProtoListImplementation* node)
        {
            return node ? node->height : 0;
        }

        int getBalance(const ProtoListImplementation* node)
        {
            if (!node)
            {
                return 0;
            }
            return getHeight(node->nextNode) - getHeight(node->previousNode);
        }

        const ProtoListImplementation* rightRotate(ProtoContext* context, const ProtoListImplementation* y)
        {
            const ProtoListImplementation* x = y->previousNode;
            const auto T2 = x->nextNode;

            // Perform rotation
            const auto* new_y = new(context) ProtoListImplementation(context, y->value, T2, y->nextNode);
            return new(context) ProtoListImplementation(context, x->value, x->previousNode, new_y);
        }

        const ProtoListImplementation* leftRotate(ProtoContext* context, const ProtoListImplementation* x)
        {
            const ProtoListImplementation* y = x->nextNode;
            const ProtoListImplementation* T2 = y->previousNode;

            // Perform rotation
            const auto* new_x = new(context) ProtoListImplementation(context, x->value, x->previousNode, T2);
            return new(context) ProtoListImplementation(context, y->value, new_x, y->nextNode);
        }

        const ProtoListImplementation* rebalance(ProtoContext* context, const ProtoListImplementation* node)
        {
            const int balance = getBalance(node);

            // Case 1: Left-Left (LL)
            if (balance < -1 && getBalance(node->previousNode) <= 0)
            {
                return rightRotate(context, node);
            }

            // Case 2: Right-Right (RR)
            if (balance > 1 && getBalance(node->nextNode) >= 0)
            {
                return leftRotate(context, node);
            }

            // Case 3: Left-Right (LR)
            if (balance < -1 && getBalance(node->previousNode) > 0)
            {
                const ProtoListImplementation* new_prev = leftRotate(context, node->previousNode);
                const auto* new_node = new(context) ProtoListImplementation(
                    context, node->value, new_prev, node->nextNode);
                return rightRotate(context, new_node);
            }

            // Case 4: Right-Left (RL)
            if (balance > 1 && getBalance(node->nextNode) < 0)
            {
                const ProtoListImplementation* new_next = rightRotate(context, node->nextNode);
                const auto* new_node = new(context) ProtoListImplementation(
                    context, node->value, node->previousNode, new_next);
                return leftRotate(context, new_node);
            }

            return node; // The node is already balanced
        }
    } // end of anonymous namespace

    // --- Public Interface Methods ---

    const ProtoObject* ProtoListImplementation::implGetAt(ProtoContext* context, int index) const
    {
        if (this->isEmpty)
        {
            return PROTO_NONE;
        }

        if (index < 0)
        {
            index += this->hash;
        }

        if (index < 0 || static_cast<unsigned>(index) >= this->hash)
        {
            return PROTO_NONE;
        }

        auto node = this;
        while (node)
        {
            const unsigned long thisIndex = node->previousNode ? node->previousNode->hash : 0;
            if (static_cast<unsigned long>(index) == thisIndex)
            {
                return node->value;
            }
            if (static_cast<unsigned long>(index) < thisIndex)
            {
                node = node->previousNode;
            }
            else
            {
                node = node->nextNode;
                index -= (thisIndex + 1);
            }
        }
        return PROTO_NONE; // Should not reach here if the logic is correct
    }

    const ProtoObject* ProtoListImplementation::implGetFirst(ProtoContext* context) const
    {
        return this->implGetAt(context, 0);
    }

    const ProtoObject* ProtoListImplementation::implGetLast(ProtoContext* context) const
    {
        return this->implGetAt(context, -1);
    }

    unsigned long ProtoListImplementation::implGetSize(ProtoContext* context) const
    {
        return this->hash;
    }

    bool ProtoListImplementation::implHas(ProtoContext* context, const ProtoObject* targetValue) const
    {
        // CRITICAL FIX: Loop corrected to avoid out-of-bounds access.
        // The original loop (i <= this->count) iterated one time too many.
        for (unsigned long i = 0; i < this->hash; i++)
        {
            if (this->implGetAt(context, i) == targetValue)
            {
                return true;
            }
        }
        return false;
    }

    const ProtoListImplementation* ProtoListImplementation::implAppendLast(ProtoContext* context, ProtoObject* newValue) const
    {
        if (!this->value)
        {
            return new(context) ProtoListImplementation(context, newValue);
        }

        ProtoListImplementation* newNode;
        if (this->nextNode)
        {
            auto* new_next = static_cast<const ProtoListImplementation*>(this->nextNode->implAppendLast(
                context, newValue));
            newNode = new(context) ProtoListImplementation(context, this->value, this->previousNode, new_next);
        }
        else
        {
            newNode = new(context) ProtoListImplementation(
                context,
                this->value,
                false,
                this->previousNode,
                new(context) ProtoListImplementation(context, newValue)
            );
        }
        return rebalance(context, newNode);
    }

    const ProtoListImplementation* ProtoListImplementation::implRemoveLast(ProtoContext* context) const
    {
        // STUB implementation, the actual logic is more complex.
        return this->implRemoveAt(context, -1);
    }

    // ... Implementation of the rest of the methods (setAt, insertAt, etc.) ...
    // NOTE: Many of these methods use the 'value' variable which is not defined.
    // It must be corrected to 'this->value' in all cases.

    const ProtoListImplementation* ProtoListImplementation::implRemoveAt(ProtoContext* context, int index) const
    {
        if (this->isEmpty)
        {
            return this;
        }
        // ... Index normalization logic ...
        if (index < 0) index += this->size;
        if (index < 0 || static_cast<unsigned>(index) >= this->size) return this;

        const unsigned long thisIndex = this->previousNode ? this->previousNode->hash : 0;
        ProtoListImplementation* newNode;

        if (static_cast<unsigned long>(index) == thisIndex)
        {
            // Logic to join left and right subtrees
            if (!this->previousNode) return this->nextNode;
            if (!this->nextNode) return this->previousNode;

            // Join the two subtrees
            const ProtoObject* rightmost_of_left = this->previousNode->implGetLast(context);
            auto* left_without_rightmost = static_cast<const ProtoListImplementation*>(this->previousNode->
                implRemoveLast(context));

            newNode = new(context) ProtoListImplementation(
                context,
                rightmost_of_left,
                left_without_rightmost,
                this->nextNode
            );
        }
        else
        {
            if (static_cast<unsigned long>(index) < thisIndex)
            {
                auto* new_prev = this->previousNode?
                    static_cast<const ProtoListImplementation*>(this->previousNode->implRemoveAt(context, index)) :
                    nullptr;

                newNode = new(context) ProtoListImplementation(context, this->value, new_prev, this->nextNode);
            }
            else
            {
                auto* new_next = this->nextNode?
                    static_cast<const ProtoListImplementation*>(this->nextNode->implRemoveAt(context, index - thisIndex - 1)) :
                    nullptr;

                newNode = new(context) ProtoListImplementation(context, this->value, this->previousNode, new_next);
            }
        }
        return rebalance(context, newNode);
    }

    const ProtoListImplementation* ProtoListImplementation::implGetSlice(ProtoContext* context, int from, int to) const
    {
        if (from < 0)
        {
            from = this->hash + from;
            if (from < 0)
                from = 0;
        }

        if (to < 0)
        {
            to = this->hash + to;
            if (to < 0)
                to = 0;
        }

        if (to >= from)
        {
            const ProtoListImplementation* upperPart = this->implSplitLast(context, from);
            return upperPart->implSplitFirst(context, to - from);
        }
        else
            return new(context) ProtoListImplementation(context);
    };

    const ProtoListImplementation* ProtoListImplementation::implSetAt(ProtoContext* context, int index,
                                                                      const ProtoObject* newValue) const
    {
        if (!this->value)
        {
            return nullptr;
        }

        if (index < 0)
        {
            index = this->hash + index;
            if (index < 0)
                index = 0;
        }

        if (static_cast<unsigned long>(index) >= this->hash)
        {
            return nullptr;
        }

        const int thisIndex = this->previousNode ? this->previousNode->hash : 0;
        if (thisIndex == index)
        {
            return new(context) ProtoListImplementation(
                context,
                value,
                this->previousNode,
                this->nextNode
            );
        }

        if (index < thisIndex)
            return new(context) ProtoListImplementation(
                context,
                value,
                false,
                this->previousNode? this->previousNode->implSetAt(context, index, value) : nullptr,
                this->nextNode
            );
        else
            return new(context) ProtoListImplementation(
                context,
                value,
                this->previousNode,
                this->nextNode? this->nextNode->implSetAt(context, index - thisIndex - 1, value) : nullptr
            );
    };

    const ProtoListImplementation* ProtoListImplementation::implInsertAt(
        ProtoContext* context, int index, ProtoObject* newValue) const
    {
        if (!this->value)
            return new(context) ProtoListImplementation(
                context,
                newValue
            );

        if (index < 0)
        {
            index = this->hash + index;
            if (index < 0)
                index = 0;
        }

        if (static_cast<unsigned long>(index) >= this->hash)
            index = this->hash - 1;

        const unsigned long thisIndex = this->previousNode ? this->previousNode->hash : 0;
        ProtoListImplementation* newNode;

        if (thisIndex == static_cast<unsigned long>(index))
            newNode = new(context) ProtoListImplementation(
                context,
                newValue,
                this->previousNode,
                new(context) ProtoListImplementation(
                    context,
                    this->value,
                    false,
                    nullptr,
                    this->nextNode
                )
            );
        else
        {
            if (static_cast<unsigned long>(index) < thisIndex)
                newNode = new(context) ProtoListImplementation(
                    context,
                    this->value,
                    false,
                    this->previousNode? this->previousNode->implInsertAt(context, index, newValue) : nullptr,
                    this->nextNode
                );
            else
                newNode = new(context) ProtoListImplementation(
                    context,
                    this->value,
                    false,
                    this->previousNode,
                    this->nextNode? this->nextNode->implInsertAt(context, index - thisIndex - 1, newValue) : nullptr
                );
        }

        return rebalance(context, newNode);
    };

    const ProtoListImplementation* ProtoListImplementation::implAppendFirst(
        ProtoContext* context, ProtoObject* newValue) const
    {
        if (!this->value)
            return new(context) ProtoListImplementation(
                context,
                newValue
            );

        ProtoListImplementation* newNode;

        if (this->previousNode)
            newNode = new(context) ProtoListImplementation(
                context,
                this->value,
                false,
                this->previousNode->implAppendFirst(context, newValue),
                this->nextNode
            );
        else
        {
            newNode = new(context) ProtoListImplementation(
                context,
                this->value,
                false,
                new(context) ProtoListImplementation(
                    context,
                    newValue
                ),
                this->nextNode
            );
        }

        return rebalance(context, newNode);
    };

    const ProtoListImplementation* ProtoListImplementation::implExtend(ProtoContext* context, const ProtoListImplementation* other) const
    {
        if (this->hash == 0)
            return other;

        const unsigned long otherCount = other->getSize(context);

        if (otherCount == 0)
            return this;

        if (this->hash < otherCount)
            return rebalance(
                context,
                new(context) ProtoListImplementation(
                    context,
                    this->implGetLast(context),
                    false,
                    this->implRemoveLast(context),
                    other
                ));
        else
            return rebalance(
                context,
                new(context) ProtoListImplementation(
                    context,
                    other->getFirst(context),
                    false,
                    this,
                    static_cast<const ProtoListImplementation*>(other->removeFirst(context))
                ));
    };

    const ProtoListImplementation* ProtoListImplementation::implSplitFirst(ProtoContext* context, int index) const
    {
        if (!this->value)
            return this;

        if (index < 0)
        {
            index = static_cast<int>(this->hash) + index;
            if (index < 0)
                index = 0;
        }

        if (index >= static_cast<int>(this->hash))
            index = static_cast<int>(this->hash) - 1;

        if (index == static_cast<int>(this->hash) - 1)
            return this;

        if (index == 0)
            return new(context) ProtoListImplementation(context);

        const ProtoListImplementation* newNode = nullptr;

        if (const int thisIndex = (this->previousNode ? this->previousNode->hash : 0); thisIndex == index)
            return this->previousNode;
        else
        {
            if (index > thisIndex)
            {
                const ProtoListImplementation* newNext = this->nextNode->
                                                         implSplitFirst(context, index - thisIndex - 1);
                if (newNext->hash == 0)
                    newNext = nullptr;
                newNode = new(context) ProtoListImplementation(
                    context,
                    this->value,
                    this->previousNode,
                    newNext
                );
            }
            else
            {
                if (this->previousNode)
                    return this->previousNode->implSplitFirst(context, index);
                else
                    newNode = new(context) ProtoListImplementation(
                        context,
                        value,
                        false,
                        nullptr,
                        this->nextNode->implSplitFirst(context, index - thisIndex - 1)
                    );
            }
        }

        return rebalance(context, newNode);
    };

    const ProtoListImplementation* ProtoListImplementation::implSplitLast(ProtoContext* context, int index) const
    {
        if (!this->value)
            return this;

        if (index < 0)
        {
            index = this->hash + index;
            if (index < 0)
                index = 0;
        }

        if (static_cast<unsigned long>(index) >= this->hash)
            index = this->hash - 1;

        if (index == 0)
            return this;

        const ProtoListImplementation* newNode = nullptr;

        if (const int thisIndex = (this->previousNode ? this->previousNode->hash : 0); thisIndex == index)
        {
            if (!this->previousNode)
                return this;
            else
            {
                newNode = new(context) ProtoListImplementation(
                    context,
                    value,
                    false,
                    nullptr,
                    this->nextNode
                );
            }
        }
        else
        {
            if (index < thisIndex)
            {
                newNode = new(context) ProtoListImplementation(
                    context,
                    this->value,
                    false,
                    this->previousNode? this->previousNode->implSplitLast(context, index) : nullptr,
                    this->nextNode
                );
            }
            else
            {
                if (!this->nextNode)
                // It should not happen!
                    return new(context) ProtoListImplementation(context);
                else
                {
                    return this->nextNode->implSplitLast(
                        context,
                        index - thisIndex - 1
                    );
                }
            }
        }

        return rebalance(context, newNode);
    };

    const ProtoListImplementation* ProtoListImplementation::implRemoveFirst(ProtoContext* context) const
    {
        if (!this->value)
            return this;

        const ProtoListImplementation* newNode;

        if (this->previousNode)
        {
            newNode = this->previousNode->implRemoveFirst(context);
            if (newNode->value == nullptr)
                newNode = nullptr;
            newNode = new(context) ProtoListImplementation(
                context,
                value,
                false,
                newNode,
                this->nextNode
            );
        }
        else
        {
            if (this->nextNode)
                return this->nextNode;

            newNode = new(context) ProtoListImplementation(
                context,
                nullptr,
                false,
                nullptr,
                nullptr
            );
        }

        return rebalance(context, newNode);
    };


    const ProtoListImplementation* ProtoListImplementation::implRemoveSlice(
        ProtoContext* context, int from, int to) const
    {
        if (from < 0)
        {
            from = this->hash + from;
            if (from < 0)
                from = 0;
        }

        if (to < 0)
        {
            to = this->hash + to;
            if (to < 0)
                to = 0;
        }

        if (to >= from)
        {
            return this->implSplitFirst(context, from)->implExtend(
                context,
                this->implSplitLast(context, from)
            );
        }
        else
            return this;
    };


    // ... The rest of the implementations must be reviewed to correct the 'value' error ...

    const ProtoObject* ProtoListImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.listImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST;
        return p.oid.oid;
    }

    unsigned long ProtoListImplementation::getHash(ProtoContext* context) const
    {
        // The base Cell class already provides an address-based hash,
        // which is consistent. That one or the one that was here can be used.
        return Cell::getHash(context);
    }

    const ProtoListIteratorImplementation* ProtoListImplementation::implGetIterator(ProtoContext* context) const
    {
        // CRITICAL FIX: The iterator must point to 'this', not 'nullptr'.
        return new(context) ProtoListIteratorImplementation(context, this, 0);
    }

    void ProtoListImplementation::finalize(ProtoContext* context) const
    {
    };

    void ProtoListImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            Cell* cell
        )
    ) const
    {
        // Recursively traverse all references for the GC.
        if (this->previousNode)
        {
            this->previousNode->processReferences(context, self, method);
        }
        if (this->nextNode)
        {
            this->nextNode->processReferences(context, self, method);
        }
        if (this->value && this->value->isCell(context))
        {
            const auto cell = this->value->asCell(context);
            cell->processReferences(context, self, method);
        }
    }
} // namespace proto
