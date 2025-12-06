/*
 * ProtoMultiset.cpp
 *
 *  Created on: 2024-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

namespace proto
{
    //=========================================================================
    // ProtoMultisetIteratorImplementation
    //=========================================================================

    ProtoMultisetIteratorImplementation::ProtoMultisetIteratorImplementation(
        ProtoContext* context,
        const ProtoSparseListIterator* sparseListIterator,
        const ProtoListIterator* bucketIterator,
        const ProtoObject* currentObject,
        long count
    ) : Cell(context), sparseListIterator(sparseListIterator), bucketIterator(bucketIterator), currentObject(currentObject), count(count)
    {
    }

    int ProtoMultisetIteratorImplementation::implHasNext(ProtoContext* context) const
    {
        return this->count > 0 || (this->bucketIterator && this->bucketIterator->hasNext(context)) || (this->sparseListIterator && this->sparseListIterator->hasNext(context));
    }

    const ProtoObject* ProtoMultisetIteratorImplementation::implNext(ProtoContext* context) const
    {
        return this->currentObject;
    }

    const ProtoMultisetIteratorImplementation* ProtoMultisetIteratorImplementation::implAdvance(ProtoContext* context) const
    {
        if (this->count > 1) {
            return new(context) ProtoMultisetIteratorImplementation(context, this->sparseListIterator, this->bucketIterator, this->currentObject, this->count - 1);
        }

        const ProtoListIterator* nextBucketIterator = this->bucketIterator;
        if (nextBucketIterator && nextBucketIterator->hasNext(context)) {
             nextBucketIterator = toImpl<const ProtoListIteratorImplementation>(nextBucketIterator->advance(context))->asProtoListIterator(context);
        }

        if (nextBucketIterator && nextBucketIterator->hasNext(context)) {
            const auto* entry = toImpl<const ProtoTuple>(nextBucketIterator->next(context));
            return new(context) ProtoMultisetIteratorImplementation(context, this->sparseListIterator, nextBucketIterator, entry->getAt(context, 0), entry->getAt(context, 1)->asLong(context));
        }

        const ProtoSparseListIterator* nextSparseIterator = this->sparseListIterator;
        while (nextSparseIterator && nextSparseIterator->hasNext(context)) {
            nextSparseIterator = toImpl<const ProtoSparseListIteratorImplementation>(nextSparseIterator->advance(context))->asSparseListIterator(context);
            if (nextSparseIterator) {
                const ProtoList* bucket = toImpl<const ProtoList>(nextSparseIterator->nextValue(context));
                if (bucket && bucket->getSize(context) > 0) {
                    const ProtoListIterator* newBucketIterator = bucket->getIterator(context);
                    const auto* entry = toImpl<const ProtoTuple>(newBucketIterator->next(context));
                    return new(context) ProtoMultisetIteratorImplementation(context, nextSparseIterator, newBucketIterator, entry->getAt(context, 0), entry->getAt(context, 1)->asLong(context));
                }
            }
        }

        return new(context) ProtoMultisetIteratorImplementation(context, nullptr, nullptr, nullptr, 0);
    }

    const ProtoObject* ProtoMultisetIteratorImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p;
        p.multisetIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_MULTISET_ITERATOR;
        return p.oid.oid;
    }

    void ProtoMultisetIteratorImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        if (this->sparseListIterator) method(context, self, toImpl<const Cell>(this->sparseListIterator));
        if (this->bucketIterator) method(context, self, toImpl<const Cell>(this->bucketIterator));
        if (this->currentObject && this->currentObject->isCell(context)) method(context, self, this->currentObject->asCell(context));
    }

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

    const ProtoMultisetIteratorImplementation* ProtoMultisetImplementation::implGetIterator(ProtoContext* context) const
    {
        const ProtoSparseListIterator* sparseIterator = this->base->getIterator(context);
        if (sparseIterator && sparseIterator->hasNext(context)) {
            const ProtoList* bucket = toImpl<const ProtoList>(sparseIterator->nextValue(context));
            if (bucket && bucket->getSize(context) > 0) {
                const ProtoListIterator* bucketIterator = bucket->getIterator(context);
                const auto* entry = toImpl<const ProtoTuple>(bucketIterator->next(context));
                return new(context) ProtoMultisetIteratorImplementation(context, sparseIterator, bucketIterator, entry->getAt(context, 0), entry->getAt(context, 1)->asLong(context));
            }
        }
        return new(context) ProtoMultisetIteratorImplementation(context, sparseIterator, nullptr, nullptr, 0);
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
        return toImpl<const ProtoMultisetImplementation>(this)->implGetSize();
    }

    const ProtoObject* ProtoMultiset::asObject(ProtoContext* context) const {
        return toImpl<const ProtoMultisetImplementation>(this)->implAsObject(context);
    }

    const ProtoMultisetIterator* ProtoMultiset::getIterator(ProtoContext* context) const {
        const auto* impl = toImpl<const ProtoMultisetImplementation>(this)->implGetIterator(context);
        return impl ? (const ProtoMultisetIterator*)impl->implAsObject(context) : nullptr;
    }
}
