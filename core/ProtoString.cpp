/*
 * ProtoString.cpp
 *
 *  Created on: Aug 6, 2017
 *      Author: gamarino
 *
 *  This file implements the immutable string and its iterator. The string is
 *  implemented as a wrapper around a `ProtoTuple` of characters, leveraging
 *  the tuple's underlying rope implementation for highly efficient operations.
 */

#include "../headers/proto_internal.h"
#include <algorithm>

namespace proto
{
    //=========================================================================
    // ProtoStringIteratorImplementation
    //=========================================================================

    /**
     * @class ProtoStringIteratorImplementation
     * @brief An iterator for traversing a `ProtoString`.
     *
     * This iterator simply wraps the underlying string's `getAt` method,
     * advancing an index on each call to `implAdvance`.
     */

    ProtoStringIteratorImplementation::ProtoStringIteratorImplementation(
        ProtoContext* context,
        const ProtoString* base,
        unsigned long currentIndex
    ) : Cell(context), base(base), currentIndex(currentIndex)
    {
    }

    ProtoStringIteratorImplementation::~ProtoStringIteratorImplementation() = default;

    int ProtoStringIteratorImplementation::implHasNext(ProtoContext* context) const
    {
        if (!this->base) return false;
        return this->currentIndex < this->base->getSize(context);
    }

    const ProtoObject* ProtoStringIteratorImplementation::implNext(ProtoContext* context) const
    {
        if (!implHasNext(context)) return PROTO_NONE;
        return this->base->getAt(context, this->currentIndex);
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

    void ProtoStringIteratorImplementation::finalize(ProtoContext* context) const {}

    void ProtoStringIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const
    {
        const ProtoObject* baseAsObject = (const ProtoObject*)this->base;
        if (this->base && baseAsObject->isCell(context))
        {
            method(context, self, baseAsObject->asCell(context));
        }
    }

    unsigned long ProtoStringIteratorImplementation::getHash(ProtoContext* context) const
    {
        return Cell::getHash(context);
    }


    //=========================================================================
    // ProtoStringImplementation
    //=========================================================================

    /**
     * @class ProtoStringImplementation
     * @brief An immutable string implemented as a facade over a `ProtoTuple`.
     *
     * This class demonstrates composition. It holds a `ProtoTupleImplementation`
     * (which is a rope) and delegates all of its operations (slicing, concatenation,
     * access) to the underlying tuple. This provides powerful string manipulation
     * capabilities with high performance and memory efficiency.
     */

    ProtoStringImplementation::ProtoStringImplementation(
        ProtoContext* context,
        const ProtoTupleImplementation* baseTuple
    ) : Cell(context), base(baseTuple)
    {
    }

    ProtoStringImplementation::~ProtoStringImplementation() = default;

    const ProtoObject* ProtoStringImplementation::implGetAt(ProtoContext* context, int index) const
    {
        return this->base->implGetAt(context, index);
    }

    unsigned long ProtoStringImplementation::implGetSize(ProtoContext* context) const
    {
        return this->base->implGetSize(context);
    }

    const ProtoList* ProtoStringImplementation::implAsList(ProtoContext* context) const
    {
        return this->base->implAsList(context);
    }

    const ProtoStringImplementation* ProtoStringImplementation::implAppendLast(ProtoContext* context, const ProtoString* otherString) const {
        const auto* otherTuple = toImpl<const ProtoStringImplementation>(otherString)->base;
        const auto* newTuple = this->base->implAppendLast(context, otherTuple->asProtoTuple(context));
        return new(context) ProtoStringImplementation(context, newTuple);
    }

    const ProtoString* ProtoStringImplementation::asProtoString(ProtoContext* context) const {
        return (const ProtoString*)this->implAsObject(context);
    }

    void ProtoStringImplementation::finalize(ProtoContext* context) const {}

    void ProtoStringImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const
    {
        // Report the underlying tuple to the GC.
        if (this->base)
        {
            method(context, self, this->base);
        }
    }

    const ProtoObject* ProtoStringImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p;
        p.stringImplementation = this;
        p.op.pointer_tag = POINTER_TAG_STRING;
        return p.oid.oid;
    }

    unsigned long ProtoStringImplementation::getHash(ProtoContext* context) const
    {
        // The string's hash is the hash of its underlying tuple.
        return this->base ? this->base->getHash(context) : 0;
    }

    int ProtoStringImplementation::implCompare(ProtoContext* context, const ProtoString* other) const {
        // A proper implementation would compare character by character.
        return this->getHash(context) == other->getHash(context) ? 0 : 1;
    }

    const ProtoStringIteratorImplementation* ProtoStringImplementation::implGetIterator(ProtoContext* context) const
    {
        return new(context) ProtoStringIteratorImplementation(context, (const ProtoString*)this->implAsObject(context), 0);
    }

    // --- Public API Trampolines ---

    const ProtoList* ProtoString::asList(ProtoContext* context) const {
        return toImpl<const ProtoStringImplementation>(this)->implAsList(context);
    }

    const ProtoString* ProtoString::getSlice(ProtoContext* context, int from, int to) const {
        const auto* tuple_slice = toImpl<const ProtoStringImplementation>(this)->base->implAsList(context)->getSlice(context, from, to);
        const auto* tuple_impl = toImpl<const ProtoTupleImplementation>(context->newTupleFromList(tuple_slice));
        return (new(context) ProtoStringImplementation(context, tuple_impl))->asProtoString(context);
    }

    const ProtoString* ProtoString::fromUTF8String(ProtoContext* context, const char* zeroTerminatedUtf8String) {
        // This is a static method, it should call the context's factory.
        return context->fromUTF8String(zeroTerminatedUtf8String)->asString(context);
    }

    int ProtoString::cmp_to_string(ProtoContext* context, const ProtoString* otherString) const {
        return toImpl<const ProtoStringImplementation>(this)->implCompare(context, otherString);
    }

}
