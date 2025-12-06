/*
 * ProtoMultiset.cpp
 *
 *  Created on: 2024-05-23
 *      Author: gamarino
 *
 *  This file implements the immutable multiset. The implementation is based on
 *  a ProtoSparseList where keys are object hashes. Values are ProtoList instances
 *  (buckets) that store ProtoTuple pairs of [object, count] to handle both
 *  hash collisions and element counts.
 */

#include "../headers/proto_internal.h"

namespace proto
{
    //=========================================================================
    // ProtoMultisetImplementation
    //=========================================================================

    ProtoMultisetImplementation::ProtoMultisetImplementation(
        ProtoContext* context,
        const ProtoSparseList* base,
        unsigned long totalSize
    ) : Cell(context), base(base), totalSize(totalSize)
    {
    }

    ProtoMultisetImplementation::~ProtoMultisetImplementation() = default;

    // Helper to find an entry [object, count] in a bucket list
    static const ProtoTuple* findEntryInBucket(ProtoContext* context, const ProtoList* bucket, const ProtoObject* value, int& outIndex) {
        if (!bucket) return nullptr;
        for (unsigned long i = 0; i < bucket->getSize(context); ++i) {
            const ProtoTuple* entry = toImpl<const ProtoTuple>(bucket->getAt(context, i));
            if (entry->getAt(context, 0)->compare(context, value) == 0) {
                outIndex = i;
                return entry;
            }
        }
        outIndex = -1;
        return nullptr;
    }

    const ProtoMultisetImplementation* ProtoMultisetImplementation::implAdd(ProtoContext* context, const ProtoObject* value) const
    {
        const unsigned long hash = value->getHash(context);
        const ProtoList* bucket = toImpl<const ProtoList>(this->base->getAt(context, hash));

        int entryIndex = -1;
        const ProtoTuple* existingEntry = findEntryInBucket(context, bucket, value, entryIndex);

        if (existingEntry) {
            // Entry exists, increment count
            long long currentCount = existingEntry->getAt(context, 1)->asLong(context);
            const ProtoObject* newCountObj = context->fromLong(currentCount + 1);

            const ProtoList* entryItems = context->newList()->appendLast(context, const_cast<ProtoObject*>(value))->appendLast(context, newCountObj);
            const ProtoTuple* newEntry = context->newTupleFromList(entryItems);

            const ProtoList* newBucket = bucket->setAt(context, entryIndex, newEntry->asObject(context));
            const ProtoSparseList* newBase = this->base->setAt(context, hash, newBucket->asObject(context));
            return new(context) ProtoMultisetImplementation(context, newBase, this->totalSize + 1);
        }

        // Entry does not exist, add with count 1
        const ProtoObject* newCountObj = context->fromLong(1);
        const ProtoList* entryItems = context->newList()->appendLast(context, const_cast<ProtoObject*>(value))->appendLast(context, newCountObj);
        const ProtoTuple* newEntry = context->newTupleFromList(entryItems);

        const ProtoList* newBucket = bucket ? bucket->appendLast(context, newEntry->asObject(context)) : context->newList()->appendLast(context, newEntry->asObject(context));
        const ProtoSparseList* newBase = this->base->setAt(context, hash, newBucket->asObject(context));
        return new(context) ProtoMultisetImplementation(context, newBase, this->totalSize + 1);
    }

    const ProtoObject* ProtoMultisetImplementation::implCount(ProtoContext* context, const ProtoObject* value) const
    {
        const unsigned long hash = value->getHash(context);
        const ProtoList* bucket = toImpl<const ProtoList>(this->base->getAt(context, hash));

        int entryIndex = -1;
        const ProtoTuple* entry = findEntryInBucket(context, bucket, value, entryIndex);

        if (entry) {
            return entry->getAt(context, 1);
        }

        return context->fromLong(0);
    }

    const ProtoMultisetImplementation* ProtoMultisetImplementation::implRemove(ProtoContext* context, const ProtoObject* value) const
    {
        const unsigned long hash = value->getHash(context);
        const ProtoList* bucket = toImpl<const ProtoList>(this->base->getAt(context, hash));

        int entryIndex = -1;
        const ProtoTuple* existingEntry = findEntryInBucket(context, bucket, value, entryIndex);

        if (!existingEntry) {
            return this; // Value not in multiset
        }

        long long currentCount = existingEntry->getAt(context, 1)->asLong(context);

        if (currentCount > 1) {
            // Decrement count
            const ProtoObject* newCountObj = context->fromLong(currentCount - 1);
            const ProtoList* entryItems = context->newList()->appendLast(context, const_cast<ProtoObject*>(value))->appendLast(context, newCountObj);
            const ProtoTuple* newEntry = context->newTupleFromList(entryItems);
            const ProtoList* newBucket = bucket->setAt(context, entryIndex, newEntry->asObject(context));
            const ProtoSparseList* newBase = this->base->setAt(context, hash, newBucket->asObject(context));
            return new(context) ProtoMultisetImplementation(context, newBase, this->totalSize - 1);
        }

        // Remove entry from bucket
        const ProtoList* newBucket = bucket->removeAt(context, entryIndex);
        const ProtoSparseList* newBase;
        if (newBucket->getSize(context) == 0) {
            newBase = this->base->removeAt(context, hash);
        } else {
            newBase = this->base->setAt(context, hash, newBucket->asObject(context));
        }
        return new(context) ProtoMultisetImplementation(context, newBase, this->totalSize - 1);
    }

    unsigned long ProtoMultisetImplementation::implGetSize(ProtoContext* context) const
    {
        return this->totalSize;
    }

    void ProtoMultisetImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        if (this->base) {
            method(context, self, this->base);
        }
    }

    const ProtoObject* ProtoMultisetImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p;
        p.multisetImplementation = this;
        p.op.pointer_tag = POINTER_TAG_MULTISET;
        return p.oid.oid;
    }

    const ProtoMultiset* ProtoMultisetImplementation::asProtoMultiset(ProtoContext* context) const
    {
        return (const ProtoMultiset*)this->implAsObject(context);
    }

    // --- Public API Trampolines ---

    const ProtoMultiset* ProtoMultiset::add(ProtoContext* context, const ProtoObject* value) const {
        return toImpl<const ProtoMultisetImplementation>(this)->implAdd(context, value)->asProtoMultiset(context);
    }

    const ProtoObject* ProtoMultiset::count(ProtoContext* context, const ProtoObject* value) const {
        return toImpl<const ProtoMultisetImplementation>(this)->implCount(context, value);
    }

    const ProtoMultiset* ProtoMultiset::remove(ProtoContext* context, const ProtoObject* value) const {
        return toImpl<const ProtoMultisetImplementation>(this)->implRemove(context, value)->asProtoMultiset(context);
    }

    unsigned long ProtoMultiset::getSize(ProtoContext* context) const {
        return toImpl<const ProtoMultisetImplementation>(this)->implGetSize(context);
    }

    const ProtoObject* ProtoMultiset::asObject(ProtoContext* context) const {
        return toImpl<const ProtoMultisetImplementation>(this)->implAsObject(context);
    }
}
