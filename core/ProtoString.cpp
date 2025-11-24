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

    ProtoStringIteratorImplementation::ProtoStringIteratorImplementation(
        ProtoContext* context,
        const ProtoStringImplementation* base,
        unsigned long currentIndex
    ) : Cell(context), base(base), currentIndex(currentIndex)
    {
    }

    ProtoStringIteratorImplementation::~ProtoStringIteratorImplementation() = default;

    int ProtoStringIteratorImplementation::implHasNext(ProtoContext* context) const
    {
        if (!this->base) return false;
        return this->currentIndex < this->base->implGetSize(context);
    }

    const ProtoObject* ProtoStringIteratorImplementation::implNext(ProtoContext* context) const
    {
        if (!implHasNext(context)) return PROTO_NONE;
        return this->base->implGetAt(context, this->currentIndex);
    }

    const ProtoStringIteratorImplementation* ProtoStringIteratorImplementation::implAdvance(ProtoContext* context) const
    {
        return new(context) ProtoStringIteratorImplementation(context, this->base, this->currentIndex + 1);
    }

    const ProtoObject* ProtoStringIteratorImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p;
        p.stringIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_STRING_ITERATOR;
        return p.oid.oid;
    }

    void ProtoStringIteratorImplementation::finalize(ProtoContext* context) const override {}

    void ProtoStringIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const override
    {
        if (this->base)
        {
            method(context, self, this->base);
        }
    }

    unsigned long ProtoStringIteratorImplementation::getHash(ProtoContext* context) const {
        return Cell::getHash(context);
    }


    // --- ProtoStringImplementation ---

    ProtoStringImplementation::ProtoStringImplementation(
        ProtoContext* context,
        const ProtoTupleImplementation* baseTuple
    ) : Cell(context), baseTuple(baseTuple)
    {
    }

    ProtoStringImplementation::~ProtoStringImplementation() = default;

    const ProtoObject* ProtoStringImplementation::implGetAt(ProtoContext* context, int index) const
    {
        return this->baseTuple->implGetAt(context, index);
    }

    unsigned long ProtoStringImplementation::implGetSize(ProtoContext* context) const
    {
        return this->baseTuple->implGetSize(context);
    }

    const ProtoStringImplementation* ProtoStringImplementation::implGetSlice(ProtoContext* context, int from, int to) const
    {
        const auto* newTuple = this->baseTuple->implGetSlice(context, from, to);
        return new(context) ProtoStringImplementation(context, newTuple);
    }

    const ProtoStringImplementation* ProtoStringImplementation::implSetAt(ProtoContext* context, int index, const ProtoObject* value) const
    {
        const auto* newTuple = this->baseTuple->implSetAt(context, index, value);
        return new(context) ProtoStringImplementation(context, newTuple);
    }

    const ProtoStringImplementation* ProtoStringImplementation::implInsertAt(ProtoContext* context, int index, const ProtoObject* value) const
    {
        const auto* newTuple = this->baseTuple->implInsertAt(context, index, value);
        return new(context) ProtoStringImplementation(context, newTuple);
    }

    const ProtoStringImplementation* ProtoStringImplementation::implAppendLast(
        ProtoContext* context, const ProtoString* otherString) const
    {
        if (!otherString) return this;
        const auto* otherTuple = toImpl<const ProtoStringImplementation>(otherString)->baseTuple;
        const auto* newTuple = this->baseTuple->implAppendLast(context, otherTuple);
        return new(context) ProtoStringImplementation(context, newTuple);
    }

    const ProtoStringImplementation* ProtoStringImplementation::implAppendFirst(
        ProtoContext* context, const ProtoString* otherString) const
    {
        if (!otherString) return this;
        const auto* otherTuple = toImpl<const ProtoStringImplementation>(otherString)->baseTuple;
        const auto* newTuple = this->baseTuple->implAppendFirst(context, otherTuple);
        return new(context) ProtoStringImplementation(context, newTuple);
    }

    const ProtoStringImplementation* ProtoStringImplementation::implRemoveSlice(ProtoContext* context, int from, int to) const
    {
        const auto* newTuple = this->baseTuple->implRemoveSlice(context, from, to);
        return new(context) ProtoStringImplementation(context, newTuple);
    }

    const ProtoList* ProtoStringImplementation::implAsList(ProtoContext* context) const
    {
        return this->baseTuple->implAsList(context);
    }

    void ProtoStringImplementation::finalize(ProtoContext* context) const override {}

    void ProtoStringImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const override
    {
        if (this->baseTuple)
        {
            method(context, self, this->baseTuple);
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
        return this->baseTuple ? this->baseTuple->getHash(context) : 0;
    }

    const ProtoStringIteratorImplementation* ProtoStringImplementation::implGetIterator(ProtoContext* context) const
    {
        return new(context) ProtoStringIteratorImplementation(context, this, 0);
    }

    int ProtoStringImplementation::implCmpToString(ProtoContext* context, const ProtoString* otherString) const {
        // This can be delegated to the tuple comparison if available, or implemented manually.
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
        // This is a complex operation, for now, we can use the list conversion as a fallback.
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
