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
        const ProtoStringImplementation* base,
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

    const ProtoObject* ProtoStringIteratorImplementation::implNext(ProtoContext* context)
    {
        if (!this->base)
        {
            return PROTO_NONE;
        }
        // Returns the current element, but does not advance the iterator.
        // Advancement is done explicitly with advance().
        return this->base->implGetAt(context, this->currentIndex);
    }

    const ProtoStringIteratorImplementation* ProtoStringIteratorImplementation::implAdvance(ProtoContext* context)
    {
        // CRITICAL FIX: The iterator must advance to the next index.
        // The previousNode version created an iterator at the same position.
        return new(context) ProtoStringIteratorImplementation(context, this->base, this->currentIndex + 1);
    }

    const ProtoObject* ProtoStringIteratorImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p;
        p.stringIteratorImplementation = this;
        // CRITICAL FIX: Use the correct tag for the string iterator.
        p.op.pointer_tag = POINTER_TAG_STRING_ITERATOR;
        return p.oid.oid;
    }

    // The finalizer does not need to do anything, so we use '= default'.
    void ProtoStringIteratorImplementation::finalize(ProtoContext* context) const
    {
        // TODO: No special finalization needed for this iterator.
    }

    void ProtoStringIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, Cell* cell)
    ) const override
    {
        // FIX: Inform the GC about the reference to the base string.
        if (this->base)
        {
            method(context, self, const_cast<ProtoStringImplementation*>(this->base));
        }
    }


    // --- ProtoStringImplementation ---

    // Modernized constructor.
    ProtoStringImplementation::ProtoStringImplementation(
        ProtoContext* context,
        const ProtoTupleImplementation* baseTuple
    ) : Cell(context), baseTuple(baseTuple)
    {
    }

    // Default destructor.
    ProtoStringImplementation::~ProtoStringImplementation() = default;

    // --- Access and Utility Methods ---

    const ProtoObject* ProtoStringImplementation::implGetAt(ProtoContext* context, int index) const
    {
        // Delegates directly to the base tuple.
        return this->baseTuple->implGetAt(context, index);
    }

    unsigned long ProtoStringImplementation::implGetSize(ProtoContext* context) const
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

    const ProtoStringImplementation* ProtoStringImplementation::implGetSlice(ProtoContext* context, int from, int to) const
    {
        int thisSize = this->baseTuple->implGetSize(context);
        normalizeSliceIndices(from, to, thisSize);

        if (from >= to)
        {
            // Returns an empty string if the range is invalid.
            return new(context) ProtoStringImplementation(
                context, (const ProtoTupleImplementation*)context->newTuple());
        }

        const ProtoList* sourceList = context->newList();
        for (int i = from; i < to; i++)
        {
            sourceList = sourceList->appendLast(context, this->implGetAt(context, i));
        }

        return new(context) ProtoStringImplementation(
            context,
            ProtoTupleImplementation::tupleFromList(context, sourceList)
        );
    }

    // --- Modification Methods (Immutable) ---

    const ProtoStringImplementation* ProtoStringImplementation::implSetAt(ProtoContext* context, int index,
                                                                    const ProtoObject* value) const
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

        const ProtoList* sourceList = context->newList();
        for (int i = 0; i < thisSize; i++)
        {
            if (i == index)
            {
                sourceList = sourceList->appendLast(context, value);
            }
            else
            {
                sourceList = sourceList->appendLast(context, this->implGetAt(context, i));
            }
        }

        return new(context) ProtoStringImplementation(
            context,
            ProtoTupleImplementation::tupleFromList(context, sourceList)
        );
    }

    const ProtoStringImplementation* ProtoStringImplementation::implInsertAt(ProtoContext* context, int index,
                                                                       const ProtoObject* value) const
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

        const ProtoList* sourceList = context->newList();
        for (int i = 0; i < index; i++)
        {
            sourceList = sourceList->appendLast(context, this->implGetAt(context, i));
        }
        sourceList = sourceList->appendLast(context, value);
        for (int i = index; i < thisSize; i++)
        {
            sourceList = sourceList->appendLast(context, this->implGetAt(context, i));
        }

        return new(context) ProtoStringImplementation(
            context,
            ProtoTupleImplementation::tupleFromList(context, sourceList)
        );
    }

    const ProtoStringImplementation* ProtoStringImplementation::implAppendLast(
        ProtoContext* context, const ProtoString* otherString) const
    {
        if (!otherString)
        {
            return this;
        }

        const ProtoList* sourceList = this->asList(context);
        unsigned long otherSize = otherString->getSize(context);
        for (unsigned long i = 0; i < otherSize; i++)
        {
            sourceList = sourceList->appendLast(context, otherString->getAt(context, i));
        }

        return new(context) ProtoStringImplementation(
            context,
            ProtoTupleImplementation::tupleFromList(context, sourceList)
        );
    }

    const ProtoStringImplementation* ProtoStringImplementation::implAppendFirst(
        ProtoContext* context, const ProtoString* otherString) const
    {
        // CRITICAL FIX: The original logic was incorrect.
        if (!otherString)
        {
            return this;
        }

        const ProtoList* sourceList = otherString->asList(context);
        unsigned long thisSize = this->implGetSize(context);
        for (unsigned long i = 0; i < thisSize; i++)
        {
            sourceList = sourceList->appendLast(context, this->implGetAt(context, i));
        }

        return new(context) ProtoStringImplementation(
            context,
            ProtoTupleImplementation::tupleFromList(context, sourceList)
        );
    }

    const ProtoStringImplementation* ProtoStringImplementation::implRemoveSlice(ProtoContext* context, int from, int to) const
    {
        // FIX: The original logic created a slice, not removed one.
        int thisSize = this->baseTuple->implGetSize(context);
        normalizeSliceIndices(from, to, thisSize);

        if (from >= to)
        {
            return this; // Nothing to delete.
        }

        const ProtoList* sourceList = context->newList();
        // Part before the slice
        for (int i = 0; i < from; i++)
        {
            sourceList = sourceList->appendLast(context, this->implGetAt(context, i));
        }
        // Part after the slice
        for (int i = to; i < thisSize; i++)
        {
            sourceList = sourceList->appendLast(context, this->implGetAt(context, i));
        }

        return new(context) ProtoStringImplementation(
            context,
            ProtoTupleImplementation::tupleFromList(context, sourceList)
        );
    }

    // --- Conversion and GC Methods ---

    const ProtoList* ProtoStringImplementation::implAsList(ProtoContext* context) const
    {
        // ADJUSTED: Template syntax was removed.
        const ProtoList* result = context->newList();
        unsigned long thisSize = this->implGetSize(context);
        for (unsigned long i = 0; i < thisSize; i++)
        {
            result = result->appendLast(context, this->implGetAt(context, i));
        }
        return result;
    }

    void ProtoStringImplementation::finalize(ProtoContext* context) const override
    {
    };

    void ProtoStringImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, Cell* cell)
    ) const override
    {
        // Inform the GC about the base tuple containing the characters.
        if (this->baseTuple)
        {
            method(context, self, const_cast<ProtoTupleImplementation*>(this->baseTuple));
        }
    }

    const ProtoObject* ProtoStringImplementation::implAsObject(ProtoContext* context) const override
    {
        ProtoObjectPointer p;
        p.stringImplementation = this;
        p.op.pointer_tag = POINTER_TAG_STRING;
        return p.oid.oid;
    }

    unsigned long ProtoStringImplementation::getHash(ProtoContext* context) const override
    {
        // Delegates the hash to the base tuple for consistency.
        // If the tuple is the same, the string's hash will be the same.
        return this->baseTuple ? this->baseTuple->getHash(context) : 0;
    }

    const ProtoStringIteratorImplementation* ProtoStringImplementation::implGetIterator(ProtoContext* context) const
    {
        // ADJUSTED: Template syntax was removed.
        return new(context) ProtoStringIteratorImplementation(context, this, 0);
    }

    unsigned long ProtoStringIteratorImplementation::getHash(ProtoContext* context)
    {
        return Cell::getHash(context);
    }

    int ProtoStringImplementation::implCmpToString(ProtoContext* context, const ProtoString* otherString) const {
        unsigned long thisSize = this->implGetSize(context);
        unsigned long otherSize = otherString->getSize(context);
        unsigned long minSize = std::min(thisSize, otherSize);

        for (unsigned long i = 0; i < minSize; ++i) {
            auto thisChar = this->implGetAt(context, i);
            auto otherChar = otherString->getAt(context, i);
            if (thisChar < otherChar) return -1;
            if (thisChar > otherChar) return 1;
        }

        if (thisSize < otherSize) return -1;
        if (thisSize > otherSize) return 1;
        return 0;
    }

    const ProtoStringImplementation* ProtoStringImplementation::implSetAtString(
        ProtoContext* context, int index, const ProtoString* otherString) const {
        const ProtoList* part1 = this->implGetSlice(context, 0, index)->asList(context);
        const ProtoList* part2 = otherString->asList(context);
        const ProtoList* part3 = this->implGetSlice(context, index + 1, this->implGetSize(context))->asList(context);
        const ProtoList* combined = part1->extend(context, part2)->extend(context, part3);
        return new(context) ProtoStringImplementation(context, ProtoTupleImplementation::tupleFromList(context, combined));
    }

    const ProtoStringImplementation* ProtoStringImplementation::implInsertAtString(
        ProtoContext* context, int index, const ProtoString* otherString) const {
        const ProtoList* part1 = this->implGetSlice(context, 0, index)->asList(context);
        const ProtoList* part2 = otherString->asList(context);
        const ProtoList* part3 = this->implGetSlice(context, index, this->implGetSize(context))->asList(context);
        const ProtoList* combined = part1->extend(context, part2)->extend(context, part3);
        return new(context) ProtoStringImplementation(context, ProtoTupleImplementation::tupleFromList(context, combined));
    }

    const ProtoStringImplementation* ProtoStringImplementation::implSplitFirst(ProtoContext* context, int count) const
    {
        return this->implGetSlice(context, 0, count);
    }

    const ProtoStringImplementation* ProtoStringImplementation::implSplitLast(ProtoContext* context, int count) const
    {
        return this->implGetSlice(context, -count, this->implGetSize(context));
    }

    const ProtoStringImplementation* ProtoStringImplementation::implRemoveFirst(ProtoContext* context, int count) const
    {
        return this->implGetSlice(context, count, this->implGetSize(context));
    }

    const ProtoStringImplementation* ProtoStringImplementation::implRemoveLast(ProtoContext* context, int count) const
    {
        return this->implGetSlice(context, 0, -count);
    }

    const ProtoStringImplementation* ProtoStringImplementation::implRemoveAt(ProtoContext* context, int index) const
    {
        return this->implRemoveSlice(context, index, index + 1);
    }
} // namespace proto
