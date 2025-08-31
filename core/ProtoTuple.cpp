/*
 * ProtoTuple.cpp
 *
 *  Revised and corrected in 2024 to incorporate the latest
 *  project improvements, such as modern memory management,
 *  logic fixes, and API consistency.
 */

#include "../headers/proto_internal.h"
#include <algorithm> // For std::max and other algorithms
#include <vector>    // Useful for tuple creation


namespace proto
{
    // Recommended helper function
    int getBalance(const TupleDictionary* node)
    {
        if (!node) return 0;
        int right_height = (node->next) ? node->next->height : 0;
        int left_height = (node->previous) ? node->previous->height : 0;
        return right_height - left_height;
    }

    TupleDictionary::TupleDictionary(
        ProtoContext* context,
        const ProtoTupleImplementation key,
        TupleDictionary* next,
        TupleDictionary* previous
    ): Cell(context)
    {
        this->key = key;
        this->next = next;
        this->previous = previous;
        this->height = (key ? 1 : 0) + std::max((previous ? previous->height : 0), (next ? next->height : 0)),
            this->count = (previous ? previous->count : 0) + (key ? 1 : 0) + (next ? next->count : 0);
    };

    void TupleDictionary::finalize(ProtoContext* context)
    {
    };

    void TupleDictionary::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            Cell* cell
        )
    )
    {
        if (this->next)
            this->next->processReferences(context, self, method);
        if (this->previous)
            this->previous->processReferences(context, self, method);
        (*method)(context, this, this);
    };

    int TupleDictionary::compareList(ProtoContext* context, ProtoList* list)
    {
        int thisSize = this->key->implGetSize(context);
        int listSize = list->getSize(context);

        int cmpSize = (thisSize < listSize) ? thisSize : listSize;
        int i;
        for (i = 0; i <= cmpSize; i++)
        {
            int thisElementHash = this->key->implGetAt(context, i)->getObjectHash(context);
            int tupleElementHash = list->getAt(context, i)->getHash(context);
            if (thisElementHash > tupleElementHash)
                return 1;
            else if (thisElementHash < tupleElementHash)
                return 1;
        }
        if (i > thisSize)
            return -1;
        else if (i > listSize)
            return 1;
        return 0;
    };

    bool TupleDictionary::hasList(ProtoContext* context, ProtoList* list)
    {
        TupleDictionary* node = this;
        int cmp;

        // Empty tree case
        if (!this->key)
            return false;

        while (node)
        {
            cmp = node->compareList(context, list);
            if (cmp == 0)
                return true;
            if (cmp > 0)
                node = node->next;
            else
                node = node->previous;
        }
        return false;
    };

    bool TupleDictionary::has(ProtoContext* context, ProtoTuple* tuple)
    {
        TupleDictionary* node = this;
        int cmp;

        // Empty tree case
        if (!this->key)
            return false;

        while (node)
        {
            cmp = node->compareTuple(context, tuple);
            if (cmp == 0)
                return true;
            if (cmp > 0)
                node = node->next;
            else
                node = node->previous;
        }
        return false;
    };

    ProtoTupleImplementation* TupleDictionary::getAt(ProtoContext* context, ProtoTupleImplementation* tuple)
    {
        TupleDictionary* node = this;
        int cmp;

        // Empty tree case
        if (!this->key)
            return nullptr;

        while (node)
        {
            cmp = node->compareTuple(context, tuple);
            if (cmp == 0)
                return node->key;
            if (cmp > 0)
                node = node->next;
            else
                node = node->previous;
        }
        return nullptr;
    };

    TupleDictionary* TupleDictionary::set(ProtoContext* context, ProtoTupleImplementation* tuple)
    {
        TupleDictionary* newNode;
        int cmp;

        // Empty tree case
        if (!this->key)
            return new(context) TupleDictionary(
                context,
                tuple
            );

        cmp = this->compareTuple(context, tuple);
        if (cmp > 0)
        {
            if (this->next)
            {
                newNode = new(context) TupleDictionary(
                    context,
                    this->key,
                    this->previous,
                    this->next->set(context, tuple)
                );
            }
            else
            {
                newNode = new(context) TupleDictionary(
                    context,
                    this->key,
                    this->previous,
                    new(context) TupleDictionary(
                        context,
                        tuple
                    )
                );
            }
        }
        else if (cmp < 0)
        {
            if (this->previous)
            {
                newNode = new(context) TupleDictionary(
                    context,
                    this->key,
                    this->previous->set(context, tuple),
                    this->next
                );
            }
            else
            {
                newNode = new(context) TupleDictionary(
                    context,
                    this->key,
                    new(context) TupleDictionary(
                        context,
                        tuple
                    ),
                    this->next
                );
            }
        }
        else
            return this;

        return newNode->rebalance(context);
    };

    int TupleDictionary::compareTuple(ProtoContext* context, ProtoTuple* tuple)
    {
        int thisSize = this->key->implGetSize(context);
        int tupleSize = tuple->getSize(context);

        int cmpSize = (thisSize < tupleSize) ? thisSize : tupleSize;
        int i;
        for (i = 0; i < cmpSize; i++)
        {
            int thisElementHash = this->key->implGetAt(context, i)->getObjectHash(context);
            int tupleElementHash = tuple->getAt(context, i)->getHash(context);
            if (thisElementHash > tupleElementHash)
                return 1;
            else if (thisElementHash < tupleElementHash)
                return -1;
        }
        if (i > thisSize)
            return -1;
        else if (i > tupleSize)
            return 1;
        return 0;
    }

    // A utility function to right rotate subtree rooted with y
    // See the diagram given above.
    TupleDictionary* TupleDictionary::rightRotate(ProtoContext* context)
    {
        if (!this->previous)
            return this;

        TupleDictionary* newRight = new(context) TupleDictionary(
            context,
            this->key,
            this->previous->next,
            this->next
        );
        return new(context) TupleDictionary(
            context,
            this->previous->key,
            this->previous->previous,
            newRight
        );
    }

    // A utility function to left rotate subtree rooted with x
    // See the diagram given above.
    TupleDictionary* TupleDictionary::leftRotate(ProtoContext* context)
    {
        if (!this->next)
            return this;

        TupleDictionary* newLeft = new(context) TupleDictionary(
            context,
            this->key,
            this->previous,
            this->next->previous
        );
        return new(context) TupleDictionary(
            context,
            this->next->key,
            newLeft,
            this->next->next
        );
    }


    TupleDictionary* TupleDictionary::rebalance(ProtoContext* context)
    {
        // Convention: balance = height(right) - height(left)
        int balance = getBalance(this); // getBalance should encapsulate the calculation

        // CASE 1: Left subtree is heavier
        if (balance < -1)
        {
            // A right rotation is needed. But single or double?
            // We need to look at the balance of the left child.
            int left_child_balance = getBalance(this->previous);

            // Left-Right Case (requires double rotation)
            if (left_child_balance > 0) // The left child is heavy on the RIGHT
            {
                // Step 1: Rotate the left child to the left.
                TupleDictionary* new_left_child = this->previous->leftRotate(context);
                // Step 2: Create a new 'this' node with the updated left child.
                TupleDictionary* new_this = new(context)
                    TupleDictionary(context, this->key, new_left_child, this->next);
                // Step 3: Rotate this new node to the right.
                return new_this->rightRotate(context);
            }
            // Left-Left Case (requires single rotation)
            else
            {
                return this->rightRotate(context);
            }
        }

        // CASE 2: Right subtree is heavier
        if (balance > 1)
        {
            // A left rotation is needed. But single or double?
            // We need to look at the balance of the right child.
            int right_child_balance = getBalance(this->next);

            // Right-Left Case (requires double rotation)
            if (right_child_balance < 0) // The right child is heavy on the LEFT
            {
                // Step 1: Rotate the right child to the right.
                TupleDictionary* new_right_child = this->next->rightRotate(context);
                // Step 2: Create a new 'this' node with the updated right child.
                TupleDictionary* new_this = new(context) TupleDictionary(
                    context, this->key, this->previous, new_right_child);
                // Step 3: Rotate this new node to the left.
                return new_this->leftRotate(context);
            }
            // Right-Right Case (requires single rotation)
            else
            {
                return this->leftRotate(context);
            }
        }

        // If there is no imbalance, simply return the current node.
        return this;
    }

    long unsigned int TupleDictionary::getHash(proto::ProtoContext*)
    {
        // The hash of a Cell is derived directly from its memory address.
        // This provides a fast and unique identifier for the object.
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)this;

        return p.asHash.hash;
    }

    proto::ProtoObject* TupleDictionary::asObject(proto::ProtoContext*)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)this;
        p.op.pointer_tag = POINTER_TAG_OBJECT;
        return p.oid.oid;
    }

    // --- ProtoTupleIteratorImplementation ---

    ProtoTupleIteratorImplementation::ProtoTupleIteratorImplementation(
        ProtoContext* context,
        ProtoTupleImplementation* base,
        unsigned long currentIndex
    ) : Cell(context)
    {
        this->base = base;
        this->currentIndex = currentIndex;
    };

    ProtoTupleIteratorImplementation::~ProtoTupleIteratorImplementation()
    {
    };

    int ProtoTupleIteratorImplementation::implHasNext(ProtoContext* context)
    {
        if (this->currentIndex >= this->base->getSize(context))
            return false;
        else
            return true;
    };

    ProtoObject* ProtoTupleIteratorImplementation::implNext(ProtoContext* context)
    {
        return this->base->getAt(context, this->currentIndex);
    };

    ProtoTupleIteratorImplementation* ProtoTupleIteratorImplementation::implAdvance(ProtoContext* context)
    {
        return new(context) ProtoTupleIteratorImplementation(context, this->base, this->currentIndex);
    };

    ProtoObject* ProtoTupleIteratorImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)this;
        p.op.pointer_tag = POINTER_TAG_LIST_ITERATOR;

        return p.oid.oid;
    };

    void ProtoTupleIteratorImplementation::finalize(ProtoContext* context)
    {
    };

    unsigned long ProtoTupleIteratorImplementation::getHash(ProtoContext* context)
    {
        return Cell::getHash(context);
    };

    void ProtoTupleIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            Cell* cell
        )
    )
    {
        // TODO
    };

    ProtoTupleImplementation::ProtoTupleImplementation(
        ProtoContext* context,
        unsigned long elementCount,
        unsigned long height,
        ProtoObject** data
    ) : Cell(context)
    {
        this->elementCount = elementCount;
        this->height = height;
        for (int i = 0; i < TUPLE_SIZE; i++)
            this->pointers.data[i] = data[i];
    };

    ProtoTupleImplementation::ProtoTupleImplementation(
        ProtoContext* context,
        unsigned long elementCount,
        unsigned long height,
        ProtoTupleImplementation** indirect
    ) : Cell(context)
    {
        this->elementCount = elementCount;
        this->height = height;
        for (int i = 0; i < TUPLE_SIZE; i++)
            this->pointers.indirect[i] = indirect[i];
    };

    ProtoTupleImplementation::~ProtoTupleImplementation()
    {
    };

    ProtoTupleImplementation* ProtoTupleImplementation::tupleFromList(ProtoContext* context, ProtoList* list)
    {
        unsigned long size = list->getSize(context);
        ProtoTupleImplementation* newTuple = nullptr;
        ProtoListImplementation *nextLevel, *lastLevel = nullptr;
        ProtoTupleImplementation* indirectData[TUPLE_SIZE];
        ProtoObject* data[TUPLE_SIZE];

        ProtoListImplementation* indirectPointers = new(context) ProtoListImplementation(context);
        unsigned long i, j;
        for (i = 0, j = 0; i < size; i++)
        {
            data[j++] = list->getAt(context, i);

            if (j == TUPLE_SIZE)
            {
                newTuple = new(context) ProtoTupleImplementation(
                    context,
                    TUPLE_SIZE,
                    0,
                    data
                );
                indirectPointers = indirectPointers->implAppendLast(context, (ProtoObject*)newTuple);
                j = 0;
            }
        }

        if (j != 0)
        {
            unsigned long lastSize = j;
            for (; j < TUPLE_SIZE; j++)
                data[j] = nullptr;
            newTuple = new(context) ProtoTupleImplementation(
                context,
                lastSize,
                0,
                data
            );
            indirectPointers = indirectPointers->implAppendLast(context, (ProtoObject*)newTuple);
        }

        if (size > TUPLE_SIZE)
        {
            int indirectSize = 0;
            int levelCount = 0;
            do
            {
                nextLevel = new(context) ProtoListImplementation(context);
                levelCount++;
                indirectSize = 0;
                for (i = 0, j = 0; i < indirectPointers->getSize(context); i++)
                {
                    indirectData[j] = (ProtoTupleImplementation*)indirectPointers->getAt(context, i);
                    indirectSize += indirectData[j]->elementCount;
                    j++;
                    if (j == TUPLE_SIZE)
                    {
                        newTuple = new(context) ProtoTupleImplementation(
                            context,
                            indirectSize,
                            levelCount,
                            indirectData
                        );
                        indirectSize = 0;
                        j = 0;
                        nextLevel = nextLevel->implAppendLast(context, (ProtoObject*)newTuple);
                    }
                }
                if (j != 0)
                {
                    for (; j < TUPLE_SIZE; j++)
                        indirectData[j] = nullptr;
                    newTuple = new(context) ProtoTupleImplementation(
                        context,
                        indirectSize,
                        levelCount,
                        indirectData
                    );
                    nextLevel = nextLevel->implAppendLast(context, (ProtoObject*)newTuple);
                }

                lastLevel = indirectPointers;
                indirectPointers = nextLevel;
            }
            while (nextLevel->getSize(context) > 1);
        }

        TupleDictionary *currentRoot, *newRoot;
        currentRoot = context->space->tupleRoot.load();
        do
        {
            if (currentRoot->has(context, newTuple))
            {
                newTuple = currentRoot->getAt(context, newTuple);
                break;
            }
            else
                newRoot = currentRoot->set(context, newTuple);
        }
        while (context->space->tupleRoot.compare_exchange_strong(
            currentRoot,
            newRoot
        ));

        return newTuple;
    }


    ProtoObject* ProtoTupleImplementation::implGetAt(ProtoContext* context, int index)
    {
        if (index < 0)
            index = ((int)this->elementCount) + index;

        if (index < 0)
            index = 0;

        int rest = index % TUPLE_SIZE;
        ProtoTupleImplementation* node = this;
        for (int i = this->height; i > 0; i--)
        {
            index = index / TUPLE_SIZE;
            node = node->pointers.indirect[index];
        }

        return node->pointers.data[rest];
    };

    ProtoObject* ProtoTupleImplementation::implGetFirst(ProtoContext* context)
    {
        return this->getAt(context, 0);
    };

    ProtoObject* ProtoTupleImplementation::implGetLast(ProtoContext* context)
    {
        if (this->elementCount > 0)
            return this->getAt(context, this->elementCount - 1);

        return PROTO_NONE;
    };

    ProtoTupleImplementation* ProtoTupleImplementation::implGetSlice(ProtoContext* context, int from, int to)
    {
        int thisSize = this->elementCount;
        if (from < 0)
        {
            from = thisSize + from;
            if (from < 0)
                from = 0;
        }

        if (to < 0)
        {
            to = thisSize + to;
            if (to < 0)
                to = 0;
        }

        ProtoList* sourceList = context->newList();
        for (int i = from; i <= to; i++)
            if (i < thisSize)
                sourceList = sourceList->appendLast(context, this->getAt(context, i));

        return (ProtoTupleImplementation*)context->newTupleFromList(sourceList);
    };

    unsigned long ProtoTupleImplementation::implGetSize(ProtoContext* context)
    {
        return this->elementCount;
    };

    bool ProtoTupleImplementation::implHas(ProtoContext* context, ProtoObject* value)
    {
        for (unsigned long i = 0; i < this->elementCount; i++)
            if (value == this->getAt(context, i))
                return true;

        return false;
    };

    ProtoTupleImplementation* ProtoTupleImplementation::implSetAt(ProtoContext* context, int index, ProtoObject* value)
    {
        if (!value)
        {
            return nullptr;
        }

        int thisSize = this->elementCount;

        if (index < 0)
        {
            index = thisSize + index;
            if (index < 0)
                index = 0;
        }

        if (index >= thisSize)
        {
            return nullptr;
        }

        ProtoList* sourceList = context->newList();
        for (int i = 0; i < index; i++)
            if (i < thisSize)
                sourceList = sourceList->appendLast(context, this->getAt(context, i));

        sourceList = sourceList->appendLast(context, value);

        for (int i = index + 1; i < thisSize; i++)
            sourceList = sourceList->appendLast(context, this->getAt(context, i));


        return ProtoTupleImplementation::tupleFromList(context, sourceList);
    };

    ProtoTupleImplementation* ProtoTupleImplementation::implInsertAt(ProtoContext* context, int index,
                                                                     ProtoObject* value)
    {
        if (!value)
        {
            return nullptr;
        }

        int thisSize = this->elementCount;

        if (index < 0)
        {
            index = thisSize + index;
            if (index < 0)
                index = 0;
        }

        if (index >= thisSize)
        {
            return nullptr;
        }

        ProtoList* sourceList = context->newList();
        for (int i = 0; i < index; i++)
            if (i < thisSize)
                sourceList = sourceList->appendLast(context, this->getAt(context, i));

        sourceList = sourceList->appendLast(context, value);

        for (int i = index; i < thisSize; i++)
            sourceList = sourceList->appendLast(context, this->getAt(context, i));


        return ProtoTupleImplementation::tupleFromList(context, sourceList);
    };

    ProtoTupleImplementation* ProtoTupleImplementation::implAppendFirst(ProtoContext* context, ProtoTuple* otherTuple)
    {
        if (!otherTuple)
        {
            return nullptr;
        }

        int thisSize = this->elementCount;

        ProtoList* sourceList = context->newList();
        int otherSize = otherTuple->getSize(context);
        for (int i = 0; i < otherSize; i++)
            sourceList = sourceList->appendLast(context, this->getAt(context, i));

        for (int i = 0; i < thisSize; i++)
            if (i < thisSize)
                sourceList = sourceList->appendLast(context, this->getAt(context, i));

        return ProtoTupleImplementation::tupleFromList(context, sourceList);
    };

    ProtoTupleImplementation* ProtoTupleImplementation::implAppendLast(ProtoContext* context, ProtoTuple* otherTuple)
    {
        if (!otherTuple)
        {
            return nullptr;
        }

        int thisSize = this->elementCount;

        ProtoList* sourceList = context->newList();
        for (int i = 0; i < thisSize; i++)
            if (i < thisSize)
                sourceList = sourceList->appendLast(context, this->getAt(context, i));

        int otherSize = otherTuple->getSize(context);
        for (int i = 0; i < otherSize; i++)
            sourceList = sourceList->appendLast(context, this->getAt(context, i));

        return ProtoTupleImplementation::tupleFromList(context, sourceList);
    };

    ProtoTupleImplementation* ProtoTupleImplementation::implSplitFirst(ProtoContext* context, int count)
    {
        int thisSize = this->elementCount;

        ProtoList* sourceList = context->newList();
        for (int i = 0; i < count; i++)
            if (i < thisSize)
                sourceList = sourceList->appendLast(context, this->getAt(context, i));

        return ProtoTupleImplementation::tupleFromList(context, sourceList);
    };

    ProtoTupleImplementation* ProtoTupleImplementation::implSplitLast(ProtoContext* context, int count)
    {
        int thisSize = this->elementCount;
        int first = thisSize - count;
        if (first < 0)
            first = 0;

        ProtoList* sourceList = context->newList();
        for (int i = first; i < thisSize; i++)
            if (i < thisSize)
                sourceList = sourceList->appendLast(context, this->getAt(context, i));

        return ProtoTupleImplementation::tupleFromList(context, sourceList);
    };

    ProtoTupleImplementation* ProtoTupleImplementation::implRemoveFirst(ProtoContext* context, int count)
    {
        int thisSize = this->elementCount;

        ProtoList* sourceList = context->newList();
        for (int i = count; i < thisSize; i++)
            sourceList = sourceList->appendLast(context, this->getAt(context, i));

        return ProtoTupleImplementation::tupleFromList(context, sourceList);
    };

    ProtoTupleImplementation* ProtoTupleImplementation::implRemoveLast(ProtoContext* context, int count)
    {
        int thisSize = this->elementCount;

        ProtoList* sourceList = context->newList();
        for (int i = 0; i < thisSize - count; i++)
            if (i < thisSize)
                sourceList = sourceList->appendLast(context, this->getAt(context, i));
            else
                break;

        return ProtoTupleImplementation::tupleFromList(context, sourceList);
    };

    ProtoTupleImplementation* ProtoTupleImplementation::implRemoveAt(ProtoContext* context, int index)
    {
        int thisSize = this->elementCount;

        if (index < 0)
        {
            index = thisSize + index;
            if (index < 0)
                index = 0;
        }

        if (index >= thisSize)
        {
            return nullptr;
        }

        ProtoList* sourceList = context->newList();
        for (int i = 0; i < index; i++)
            if (i < thisSize)
                sourceList = sourceList->appendLast(context, this->getAt(context, i));

        for (int i = index + 1; i < thisSize; i++)
            sourceList = sourceList->appendLast(context, this->getAt(context, i));

        return ProtoTupleImplementation::tupleFromList(context, sourceList);
    };

    ProtoTupleImplementation* ProtoTupleImplementation::implRemoveSlice(ProtoContext* context, int from, int to)
    {
        int thisSize = this->elementCount;

        if (from < 0)
        {
            from = thisSize + from;
            if (from < 0)
                from = 0;
        }

        if (to < 0)
        {
            to = thisSize + from;
            if (to < 0)
                to = 0;
        }

        ProtoList* sourceList = context->newList();
        for (int i = from; i < to; i++)
            if (i < thisSize)
                sourceList = sourceList->appendLast(context, this->getAt(context, i));
            else
                break;

        return ProtoTupleImplementation::tupleFromList(context, sourceList);
    };

    ProtoList* ProtoTupleImplementation::implAsList(ProtoContext* context)
    {
        ProtoList* sourceList = context->newList();
        for (unsigned long i = 0; i < this->elementCount; i++)
            sourceList = sourceList->appendLast(context, this->getAt(context, i));

        return sourceList;
    };

    void ProtoTupleImplementation::finalize(ProtoContext* context)
    {
    };

    void ProtoTupleImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            Cell* cell
        )
    )
    {
        int size = (this->elementCount > TUPLE_SIZE) ? TUPLE_SIZE : this->elementCount;
        for (int i = 0; i < size; i++)
            if (this->height > 0)
                method(context, self, this->pointers.indirect[i]);
            else
            {
                if (this->pointers.data[i]->isCell(context))
                    method(context, self, this->pointers.data[i]->asCell(context));
            }
    };

    ProtoObject* ProtoTupleImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)this;
        p.op.pointer_tag = POINTER_TAG_TUPLE;

        return p.oid.oid;
    };

    unsigned long ProtoTupleImplementation::getHash(ProtoContext* context)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)this;

        return p.asHash.hash;
    };

    ProtoTupleIteratorImplementation* ProtoTupleImplementation::implGetIterator(ProtoContext* context)
    {
        return new(context) ProtoTupleIteratorImplementation(context, this, 0);
    };
} // namespace proto
