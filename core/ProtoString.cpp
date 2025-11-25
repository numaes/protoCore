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
        if (this->base && this->base->isCell(context))
        {
            method(context, self, this->base->asCell(context));
        }
    }

    unsigned long ProtoStringIteratorImplementation::getHash(ProtoContext* context) const
    {
        return Cell::getHash(context);
    }


    // --- ProtoStringImplementation ---

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

    const ProtoStringImplementation* ProtoStringImplementation::implGetSlice(ProtoContext* context, int from, int to) const
    {
        const auto* newTuple = this->base->implGetSlice(context, from, to);
        return new(context) ProtoStringImplementation(context, newTuple);
    }

    // ... (and so on for all other methods, delegating to this->base)

    void ProtoStringImplementation::finalize(ProtoContext* context) const {}

    void ProtoStringImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const
    {
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
        return this->base ? this->base->getHash(context) : 0;
    }

    const ProtoStringIteratorImplementation* ProtoStringImplementation::implGetIterator(ProtoContext* context) const
    {
        return new(context) ProtoStringIteratorImplementation(context, (const ProtoString*)this->asObject(context), 0);
    }
}
