/*
 * ProtoString.cpp
 *
 *  Created on: Aug 6, 2017
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <algorithm> // For std::max and std::min

namespace proto
{
    // --- ProtoStringIteratorImplementation ---

    // Modernized constructor with initialization list.
    ProtoStringIteratorImplementation::ProtoStringIteratorImplementation(
        ProtoContext* context,
        ProtoStringImplementation* base,
        unsigned long currentIndex
    ) : Cell(context), base(base), currentIndex(currentIndex)
    {
    }

    // Default destructor.
    ProtoStringIteratorImplementation::~ProtoStringIteratorImplementation() = default;

    int ProtoStringIteratorImplementation::implHasNext(ProtoContext* context)
    {
        // It is safer to check if the base is not null.
        if (!this->base)
        {
            return false;
        }
        return this->currentIndex < this->base->implGetSize(context);
    }

    ProtoObject* ProtoStringIteratorImplementation::implNext(ProtoContext* context)
    {
        if (!this->base)
        {
            return PROTO_NONE;
        }
        // Returns the current element, but does not advance the iterator.
        // Advancement is done explicitly with advance().
        return this->base->implGetAt(context, this->currentIndex);
    }

    ProtoStringIteratorImplementation* ProtoStringIteratorImplementation::implAdvance(ProtoContext* context)
    {
        // CRITICAL FIX: The iterator must advance to the next index.
        // The previous version created an iterator at the same position.
        return new(context) ProtoStringIteratorImplementation(context, this->base, this->currentIndex + 1);
    }

    ProtoObject* ProtoStringIteratorImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)this;
        // CRITICAL FIX: Use the correct tag for the string iterator.
        p.op.pointer_tag = POINTER_TAG_STRING_ITERATOR;
        return p.oid.oid;
    }

    // The finalizer does not need to do anything, so we use '= default'.
    void ProtoStringIteratorImplementation::finalize(ProtoContext* context)
    {
        // TODO: No special finalization needed for this iterator.
    }

    void ProtoStringIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, Cell* cell)
    )
    {
        // FIX: Inform the GC about the reference to the base string.
        if (this->base)
        {
            method(context, self, this->base);
        }
    }


    // --- ProtoStringImplementation ---

    // Modernized constructor.
    ProtoStringImplementation::ProtoStringImplementation(
        ProtoContext* context,
        ProtoTupleImplementation* baseTuple
    ) : Cell(context), baseTuple(baseTuple)
    {
    }

    // Default destructor.
    ProtoStringImplementation::~ProtoStringImplementation() = default;

    // --- Access and Utility Methods ---

    ProtoObject* ProtoStringImplementation::implGetAt(ProtoContext* context, int index)
    {
        // Delegates directly to the base tuple.
        return this->baseTuple->implGetAt(context, index);
    }

    unsigned long ProtoStringImplementation::implGetSize(ProtoContext* context)
    {
        // Delegates directly to the base tuple.
        return this->baseTuple->implGetSize(context);
    }

    // Helper function to normalize slice indices.
    namespace
    {
        void normalizeSliceIndices(int& from, int& to, int size)
        {
            if (from < 0) from += size;
            if (to < 0) to += size;

            from = std::max(0, from);
            to = std::max(0, to);

            if (from > size) from = size;
            if (to > size) to = size;
        }
    }

    ProtoStringImplementation* ProtoStringImplementation::implGetSlice(ProtoContext* context, int from, int to)
    {
        int thisSize = this->baseTuple->implGetSize(context);
        normalizeSliceIndices(from, to, thisSize);

        if (from >= to)
        {
            // Returns an empty string if the range is invalid.
            return new(context) ProtoStringImplementation(
                context, (ProtoTupleImplementation*)context->newTuple());
        }

        ProtoListImplementation* sourceList = (ProtoListImplementation*)context->newList();
        for (int i = from; i < to; i++)
        {
            sourceList = sourceList->implAppendLast(context, this->implGetAt(context, i));
        }

        return new(context) ProtoStringImplementation(
            context,
            ProtoTupleImplementation::tupleFromList(context, sourceList)
        );
    }

    // --- Modification Methods (Immutable) ---

    ProtoStringImplementation* ProtoStringImplementation::implSetAt(ProtoContext* context, int index,
                                                                    ProtoObject* value)
    {
        if (!value)
        {
            return this; // Return the original string if the value is null.
        }

        int thisSize = this->baseTuple->implGetSize(context);
        if (index < 0)
        {
            index += thisSize;
        }

        if (index < 0 || index >= thisSize)
        {
            return this; // Index out of range, return the original string.
        }

        ProtoList* sourceList = context->newList();
        for (int i = 0; i < thisSize; i++)
        {
            if (i == index)
            {
                sourceList = (ProtoList*)sourceList->appendLast(context, value);
            }
            else
            {
                sourceList = (ProtoList*)sourceList->appendLast(context, this->implGetAt(context, i));
            }
        }

        return new(context) ProtoStringImplementation(
            context,
            ProtoTupleImplementation::tupleFromList(context, sourceList)
        );
    }

    ProtoStringImplementation* ProtoStringImplementation::implInsertAt(ProtoContext* context, int index,
                                                                       ProtoObject* value)
    {
        if (!value)
        {
            return this;
        }

        int thisSize = this->baseTuple->implGetSize(context);
        if (index < 0)
        {
            index += thisSize;
        }
        // Allow insertion at the end.
        if (index < 0) index = 0;
        if (index > thisSize) index = thisSize;

        ProtoList* sourceList = context->newList();
        for (int i = 0; i < index; i++)
        {
            sourceList = (ProtoList*)sourceList->appendLast(context, this->implGetAt(context, i));
        }
        sourceList = (ProtoList*)sourceList->appendLast(context, value);
        for (int i = index; i < thisSize; i++)
        {
            sourceList = (ProtoList*)sourceList->appendLast(context, this->implGetAt(context, i));
        }

        return new(context) ProtoStringImplementation(
            context,
            ProtoTupleImplementation::tupleFromList(context, sourceList)
        );
    }

    ProtoStringImplementation* ProtoStringImplementation::implAppendLast(
        ProtoContext* context, ProtoString* otherString)
    {
        if (!otherString)
        {
            return this;
        }

        ProtoList* sourceList = this->asList(context);
        unsigned long otherSize = otherString->getSize(context);
        for (unsigned long i = 0; i < otherSize; i++)
        {
            sourceList = (ProtoList*)sourceList->appendLast(context, otherString->getAt(context, i));
        }

        return new(context) ProtoStringImplementation(
            context,
            ProtoTupleImplementation::tupleFromList(context, sourceList)
        );
    }

    ProtoStringImplementation* ProtoStringImplementation::implAppendFirst(
        ProtoContext* context, ProtoString* otherString)
    {
        // CRITICAL FIX: The original logic was incorrect.
        if (!otherString)
        {
            return this;
        }

        ProtoList* sourceList = otherString->asList(context);
        unsigned long thisSize = this->implGetSize(context);
        for (unsigned long i = 0; i < thisSize; i++)
        {
            sourceList = (ProtoList*)sourceList->appendLast(context, this->implGetAt(context, i));
        }

        return new(context) ProtoStringImplementation(
            context,
            ProtoTupleImplementation::tupleFromList(context, sourceList)
        );
    }

    ProtoStringImplementation* ProtoStringImplementation::implRemoveSlice(ProtoContext* context, int from, int to)
    {
        // FIX: The original logic created a slice, not removed one.
        int thisSize = this->baseTuple->implGetSize(context);
        normalizeSliceIndices(from, to, thisSize);

        if (from >= to)
        {
            return this; // Nothing to delete.
        }

        ProtoList* sourceList = context->newList();
        // Part before the slice
        for (int i = 0; i < from; i++)
        {
            sourceList = (ProtoList*)sourceList->appendLast(context, this->implGetAt(context, i));
        }
        // Part after the slice
        for (int i = to; i < thisSize; i++)
        {
            sourceList = (ProtoList*)sourceList->appendLast(context, this->implGetAt(context, i));
        }

        return new(context) ProtoStringImplementation(
            context,
            ProtoTupleImplementation::tupleFromList(context, sourceList)
        );
    }

    // --- Conversion and GC Methods ---

    ProtoListImplementation* ProtoStringImplementation::implAsList(ProtoContext* context)
    {
        // ADJUSTED: Template syntax was removed.
        auto* result = reinterpret_cast<ProtoListImplementation*>(context->newList());
        unsigned long thisSize = this->implGetSize(context);
        for (unsigned long i = 0; i < thisSize; i++)
        {
            result = result->implAppendLast(context, this->implGetAt(context, i));
        }
        return result;
    }

    void ProtoStringImplementation::finalize(ProtoContext* context)
    {
    };

    void ProtoStringImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, Cell* cell)
    )
    {
        // Inform the GC about the base tuple containing the characters.
        if (this->baseTuple)
        {
            method(context, self, reinterpret_cast<Cell*>(this->baseTuple));
        }
    }

    ProtoObject* ProtoStringImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)this;
        p.op.pointer_tag = POINTER_TAG_STRING;
        return p.oid.oid;
    }

    unsigned long ProtoStringImplementation::getHash(ProtoContext* context)
    {
        // Delegates the hash to the base tuple for consistency.
        // If the tuple is the same, the string's hash will be the same.
        return this->baseTuple ? this->baseTuple->getHash(context) : 0;
    }

    ProtoStringIteratorImplementation* ProtoStringImplementation::implGetIterator(ProtoContext* context)
    {
        // ADJUSTED: Template syntax was removed.
        return new(context) ProtoStringIteratorImplementation(context, this, 0);
    }

    unsigned long ProtoStringIteratorImplementation::getHash(ProtoContext* context)
    {
        return Cell::getHash(context);
    }

    int ProtoStringImplementation::implCmpToString(ProtoContext* context, ProtoString* otherString) {
        // TODO: Implement actual string comparison logic.
        return 0;
    }

    ProtoStringImplementation* ProtoStringImplementation::implSetAtString(
        ProtoContext* context, int index, ProtoString* otherString) {
        // TODO: Implement actual logic for setting a string at a specific index.
        return nullptr;
    }

    ProtoStringImplementation* ProtoStringImplementation::implInsertAtString(
        ProtoContext* context, int index, ProtoString* otherString) {
        // TODO: Implement actual logic for inserting a string at a specific index.
        return nullptr;
    }

    ProtoStringImplementation* ProtoStringImplementation::implSplitFirst(ProtoContext* context, int count)
    {
        // TODO: Implement actual logic for splitting the string.
        return nullptr;
    }

    ProtoStringImplementation* ProtoStringImplementation::implSplitLast(ProtoContext* context, int count)
    {
        // TODO: Implement actual logic for splitting the string.
        return nullptr;
    }

    ProtoStringImplementation* ProtoStringImplementation::implRemoveFirst(ProtoContext* context, int count)
    {
        // TODO: Implement actual logic for removing the first 'count' characters.
        return nullptr;
    }

    ProtoStringImplementation* ProtoStringImplementation::implRemoveLast(ProtoContext* context, int count)
    {
        // TODO: Implement actual logic for removing the last 'count' characters.
        return nullptr;
    }

    ProtoStringImplementation* ProtoStringImplementation::implRemoveAt(ProtoContext* context, int index)
    {
        // TODO: Implement actual logic for removing a character at a specific index.
        return nullptr;
    }
} // namespace proto
