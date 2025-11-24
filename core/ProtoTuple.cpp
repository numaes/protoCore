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
        const ProtoTupleImplementation* key,
        TupleDictionary* next,
        TupleDictionary* previous
    ): Cell(context)
    {
        this->key = key;
        this->next = next; // Right child
        this->previous = previous;
        this->height = (key ? 1 : 0) + std::max((previous ? previous->height : 0), (next ? next->height : 0)),
            this->count = (previous ? previous->count : 0) + (key ? 1 : 0) + (next ? next->count : 0);
    };

    void TupleDictionary::finalize(ProtoContext* context) const override
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
    ) const override
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
            unsigned long thisElementHash = this->key->implGetAt(context, i)->getHash(context);
            unsigned long tupleElementHash = list->getAt(context, i)->getHash(context);
            if (thisElementHash > tupleElementHash)
                return 1;
            else if (thisElementHash < tupleElementHash)
                return -1;
        }
        if (thisSize < listSize)
            return -1;
        else if (thisSize > listSize)
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
            if (cmp > 0) // this > list, so we search in the smaller part (previous)
                node = node->previous;
            else
                node = node->next;
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
            if (cmp > 0) // this > tuple, search in smaller part (previous)
                node = node->previous;
            else
                node = node->next;
        }
        return false;
    };

    const ProtoTupleImplementation* TupleDictionary::getAt(ProtoContext* context, const ProtoTupleImplementation* tuple)
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
            if (cmp > 0) // this > tuple, search in smaller part (previous)
                node = node->previous;
            else
                node = node->next;
        }
        return nullptr;
    };

    TupleDictionary* TupleDictionary::set(ProtoContext* context, const ProtoTupleImplementation* tuple)
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
        { // this > tuple, so insert in the left subtree (previous)
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
        { // this < tuple, so insert in the right subtree (next)
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

    int TupleDictionary::compareTuple(ProtoContext* context, const ProtoTuple* tuple) const
    {
        int thisSize = this->key->implGetSize(context);
        int tupleSize = tuple->getSize(context);

        int cmpSize = (thisSize < tupleSize) ? thisSize : tupleSize;
        int i;
        for (i = 0; i < cmpSize; ++i)
        {
            unsigned long thisElementHash = this->key->implGetAt(context, i)->getHash(context);
            unsigned long tupleElementHash = tuple->getAt(context, i)->getHash(context);
            if (thisElementHash > tupleElementHash)
                return 1;
            else if (thisElementHash < tupleElementHash)
                return -1;
        }
        if (thisSize < tupleSize)
            return -1;
        else if (thisSize > tupleSize)
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
        const ProtoTupleImplementation* base,
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
        return new(context) ProtoTupleIteratorImplementation(context, this->base, this->currentIndex + 1);
    };

    ProtoObject* ProtoTupleIteratorImplementation::implAsObject(ProtoContext* context)
    {   
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)this;
        p.op.pointer_tag = POINTER_TAG_LIST_ITERATOR;

        return p.oid.oid;
    };

    void ProtoTupleIteratorImplementation::finalize(ProtoContext* context) const override
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
    ) const override
    {   
        if (this->base) {
            method(context, self, const_cast<ProtoTupleImplementation*>(this->base));
        }
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
        for (int i = 0; i < TUPLE_SIZE; i++) {
            if (data && i < (int)elementCount)
                this->pointers.data[i] = data[i];
            else
                this->pointers.data[i] = nullptr;
        }
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
        for (int i = 0; i < TUPLE_SIZE; i++) {
            if (indirect && i < TUPLE_SIZE) // Indirect nodes are always full or padded
                this->pointers.indirect[i] = indirect[i];
            else
                this->pointers.indirect[i] = nullptr;
        }
    };

    ProtoTupleImplementation::~ProtoTupleImplementation()
    {
    };

    ProtoTupleImplementation* ProtoTupleImplementation::tupleFromList(ProtoContext* context, ProtoList* list)
    {   
        unsigned long size = list->getSize(context);
        if (size == 0) {
            return new(context) ProtoTupleImplementation(context, 0, 0, (ProtoObject**)nullptr);
        }

        // Level 0: data nodes
        std::vector<ProtoTupleImplementation*> currentLevel;
        for (unsigned long i = 0; i < size; ) {
            ProtoObject* data[TUPLE_SIZE] = {nullptr};
            unsigned long chunkSize = 0;
            for (int j = 0; j < TUPLE_SIZE && i < size; ++j, ++i, ++chunkSize) {
                data[j] = const_cast<ProtoObject*>(list->getAt(context, i));
            }
            currentLevel.push_back(new(context) ProtoTupleImplementation(context, chunkSize, 0, data));
        }

        // Build tree upwards
        int height = 0;
        while (currentLevel.size() > 1) {
            height++;
            std::vector<ProtoTupleImplementation*> nextLevel;
            for (size_t i = 0; i < currentLevel.size(); ) {
                ProtoTupleImplementation* indirect[TUPLE_SIZE] = {nullptr};
                unsigned long chunkElementCount = 0;
                for (int j = 0; j < TUPLE_SIZE && i < currentLevel.size(); ++j, ++i) {
                    indirect[j] = currentLevel[i];
                    chunkElementCount += indirect[j]->elementCount;
                }
                nextLevel.push_back(new(context) ProtoTupleImplementation(context, chunkElementCount, height, indirect));
            }
            currentLevel = std::move(nextLevel);
        }

        ProtoTupleImplementation* finalTuple = currentLevel[0];
        // The final tuple must have the total element count.
        if (finalTuple->elementCount != size) {
             if (height > 0) {
                 finalTuple = new(context) ProtoTupleImplementation(context, size, height, finalTuple->pointers.indirect);
             } else {
                 finalTuple = new(context) ProtoTupleImplementation(context, size, height, finalTuple->pointers.data);
             }
        }

        // Intern the tuple
        TupleDictionary *currentRoot, *newRoot;
        currentRoot = context->space->tupleRoot.load();
        do
        {
            const ProtoTupleImplementation* found = currentRoot->getAt(context, finalTuple);
            if (found)
            {
                finalTuple = const_cast<ProtoTupleImplementation*>(found);
                break;
            }
            else
                newRoot = currentRoot->set(context, finalTuple);
        }
        while (context->space->tupleRoot.compare_exchange_strong(
            currentRoot,
            newRoot
        ));

        return finalTuple;
    }


    ProtoObject* ProtoTupleImplementation::implGetAt(ProtoContext* context, int index)
    {
        if (index < 0)
            index += this->elementCount;

        if (index < 0 || (unsigned long)index >= this->elementCount)
            return PROTO_NONE;

        ProtoTupleImplementation* node = this;
        for (int h = this->height; h > 0; --h)
        {
            unsigned long span = 1;
            for(int p = 0; p < h; ++p) span *= TUPLE_SIZE;

            int chunk_index = index / span;
            index %= span;
            node = node->pointers.indirect[index];
            if (!node) return PROTO_NONE; // Should not happen in a well-formed tuple
        }

        return node->pointers.data[index % TUPLE_SIZE];
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
            if (i < thisSize) // Redundant check, but safe
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
        for (int i = 0; i < otherSize; i++) // Corrected: iterate over otherTuple
            sourceList = sourceList->appendLast(context, otherTuple->getAt(context, i));

        for (int i = 0; i < thisSize; i++)
            if (i < thisSize) // Redundant check
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
            if (i < thisSize) // Redundant check
                sourceList = sourceList->appendLast(context, this->getAt(context, i));

        int otherSize = otherTuple->getSize(context);
        for (int i = 0; i < otherSize; i++)
            sourceList = sourceList->appendLast(context, otherTuple->getAt(context, i));

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

    void ProtoTupleImplementation::finalize(ProtoContext* context) const override
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
    ) const override
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

    unsigned long ProtoTupleImplementation::getHash(ProtoContext* context) const override
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
