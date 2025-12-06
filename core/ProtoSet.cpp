/*
 * ProtoSet.cpp
 *
 *  Created on: 2024-05-23
 *      Author: gamarino
 *
 *  This file implements the immutable set. The set is implemented as a wrapper
 *  around a ProtoSparseList, where keys are object hashes and values are
 *  ProtoList instances to handle hash collisions.
 */

#include "../headers/proto_internal.h"

namespace proto
{
    //=========================================================================
    // ProtoSetImplementation
    //=========================================================================

    ProtoSetImplementation::ProtoSetImplementation(
        ProtoContext* context,
        const ProtoSparseList* base,
        unsigned long totalSize
    ) : Cell(context), base(base), totalSize(totalSize)
    {
    }

    ProtoSetImplementation::~ProtoSetImplementation() = default;

    const ProtoSetImplementation* ProtoSetImplementation::implAdd(ProtoContext* context, const ProtoObject* value) const
    {
        const unsigned long hash = value->getHash(context);
        const ProtoList* bucket = toImpl<const ProtoList>(this->base->getAt(context, hash));

        if (bucket && bucket->getSize(context) > 0) {
            if (bucket->has(context, value)) {
                return this; // Value already in set
            }
            const ProtoList* newBucket = bucket->appendLast(context, value);
            const ProtoSparseList* newBase = this->base->setAt(context, hash, newBucket->asObject(context));
            return new(context) ProtoSetImplementation(context, newBase, this->totalSize + 1);
        }

        const ProtoList* newBucket = context->newList()->appendLast(context, value);
        const ProtoSparseList* newBase = this->base->setAt(context, hash, newBucket->asObject(context));
        return new(context) ProtoSetImplementation(context, newBase, this->totalSize + 1);
    }

    const ProtoObject* ProtoSetImplementation::implHas(ProtoContext* context, const ProtoObject* value) const
    {
        const unsigned long hash = value->getHash(context);
        const ProtoList* bucket = toImpl<const ProtoList>(this->base->getAt(context, hash));

        if (bucket && bucket->getSize(context) > 0) {
            return context->fromBoolean(bucket->has(context, value));
        }
        return PROTO_FALSE;
    }

    const ProtoSetImplementation* ProtoSetImplementation::implRemove(ProtoContext* context, const ProtoObject* value) const
    {
        const unsigned long hash = value->getHash(context);
        const ProtoList* bucket = toImpl<const ProtoList>(this->base->getAt(context, hash));

        if (!bucket || bucket->getSize(context) == 0 || !bucket->has(context, value)) {
            return this; // Value not in set
        }

        ProtoList* newBucket = const_cast<ProtoList*>(context->newList());
        for (unsigned long i = 0; i < bucket->getSize(context); ++i) {
            const ProtoObject* current = bucket->getAt(context, i);
            if (current->compare(context, value) != 0) {
                newBucket = const_cast<ProtoList*>(newBucket->appendLast(context, current));
            }
        }

        const ProtoSparseList* newBase;
        if (newBucket->getSize(context) == 0) {
            newBase = this->base->removeAt(context, hash);
        } else {
            newBase = this->base->setAt(context, hash, newBucket->asObject(context));
        }
        return new(context) ProtoSetImplementation(context, newBase, this->totalSize - 1);
    }

    unsigned long ProtoSetImplementation::implGetSize(ProtoContext* context) const
    {
        return this->totalSize;
    }

    void ProtoSetImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        if (this->base) {
            method(context, self, this->base);
        }
    }

    const ProtoObject* ProtoSetImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p;
        p.setImplementation = this;
        p.op.pointer_tag = POINTER_TAG_SET;
        return p.oid.oid;
    }

    const ProtoSet* ProtoSetImplementation::asProtoSet(ProtoContext* context) const
    {
        return (const ProtoSet*)this->implAsObject(context);
    }

    // --- Public API Trampolines ---

    const ProtoSet* ProtoSet::add(ProtoContext* context, const ProtoObject* value) const {
        return toImpl<const ProtoSetImplementation>(this)->implAdd(context, value)->asProtoSet(context);
    }

    const ProtoObject* ProtoSet::has(ProtoContext* context, const ProtoObject* value) const {
        return toImpl<const ProtoSetImplementation>(this)->implHas(context, value);
    }

    const ProtoSet* ProtoSet::remove(ProtoContext* context, const ProtoObject* value) const {
        return toImpl<const ProtoSetImplementation>(this)->implRemove(context, value)->asProtoSet(context);
    }

    unsigned long ProtoSet::getSize(ProtoContext* context) const {
        return toImpl<const ProtoSetImplementation>(this)->implGetSize(context);
    }

    const ProtoObject* ProtoSet::asObject(ProtoContext* context) const {
        return toImpl<const ProtoSetImplementation>(this)->implAsObject(context);
    }
}
