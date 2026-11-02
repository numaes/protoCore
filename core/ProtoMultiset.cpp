/*
 * ProtoMultiset.cpp
 *
 *  Created on: 2024-05-28
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

namespace proto {

    ProtoMultisetImplementation::ProtoMultisetImplementation(
        ProtoContext* context,
        const ProtoSparseListImplementation* list,
        proto_ulong size,
        proto_ulong distinct
    ) : Cell(context), list(list), size(size), distinct(distinct)
    {
    }

    void ProtoMultisetImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (list) {
            // list is a raw IMPL pointer; pass directly to the GC tracer.
            method(context, self, list);
        }
    }

    const ProtoObject* ProtoMultisetImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.multisetImplementation = this;
        p.op.pointer_tag = POINTER_TAG_MULTISET;
        return p.oid;
    }

    const ProtoMultiset* ProtoMultisetImplementation::asProtoMultiset(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.multisetImplementation = this;
        p.op.pointer_tag = POINTER_TAG_MULTISET;
        return p.multiset;
    }

    ProtoMultisetIteratorImplementation::ProtoMultisetIteratorImplementation(
        ProtoContext* context,
        const ProtoSparseListIteratorImplementation* it,
        proto_ulong index
    ) : Cell(context), iterator(it), index(index)
    {
    }

    namespace {
        // A bucket is a non-empty ProtoList of (element, count) pairs laid out
        // flat: the element at an even position, its count (a SmallInteger
        // >= 1) right after it.  Pairing is positional, so an element that is
        // itself a ProtoList is never mistaken for a pair.

        // The bucket stored under `hash`, or nullptr.
        const ProtoList* bucketAt(ProtoContext* context, const ProtoSparseListImplementation* list, proto_ulong hash) {
            const ProtoObject* slot = list->implGetAt(context, hash);
            return slot ? slot->asList(context) : nullptr;
        }

        // Position of `value`'s pair (by pointer) in `bucket`, or -1.
        int pairInBucket(ProtoContext* context, const ProtoList* bucket, const ProtoObject* value) {
            const int n = static_cast<int>(bucket->getSize(context));
            for (int i = 0; i < n; i += 2)
                if (bucket->getAt(context, i) == value) return i;
            return -1;
        }

        long long countAt(ProtoContext* context, const ProtoList* bucket, int pair) {
            return bucket->getAt(context, pair + 1)->asLong(context);
        }

        const ProtoMultiset* wrap(ProtoContext* context, const ProtoSparseListImplementation* list,
                                  proto_ulong size, proto_ulong distinct) {
            return (new (context) ProtoMultisetImplementation(context, list, size, distinct))->asProtoMultiset(context);
        }

        const ProtoMultiset* addUnder(ProtoContext* context, const ProtoMultiset* self, proto_ulong hash, const ProtoObject* value) {
            const auto* impl = toImpl<const ProtoMultisetImplementation>(self);
            const ProtoList* bucket = bucketAt(context, impl->list, hash);
            const int at = bucket ? pairInBucket(context, bucket, value) : -1;
            // GC critical section: the new bucket and the chain of new
            // SparseList nodes are reachable only through C++ locals until
            // the wrapping ProtoMultisetImplementation stitches them in.
            ProtoContext::CriticalSection cs(context);
            const ProtoList* newBucket;
            proto_ulong distinct = impl->distinct;
            if (at >= 0) {
                newBucket = bucket->setAt(context, at + 1, context->fromInteger(countAt(context, bucket, at) + 1));
            } else {
                const ProtoObject* one = context->fromInteger(1);
                if (bucket) {
                    newBucket = bucket->appendLast(context, value)->appendLast(context, one);
                } else {
                    const ProtoObject* pair[2] = {value, one};
                    newBucket = context->newList(2, pair);
                }
                ++distinct;
            }
            const auto* newList = impl->list->implSetAt(context, hash, newBucket->asObject(context));
            return wrap(context, newList, impl->size + 1, distinct);
        }

        // The stored count of `value` (a SmallInteger), or nullptr if absent.
        const ProtoObject* countObjectUnder(ProtoContext* context, const ProtoMultiset* self, proto_ulong hash, const ProtoObject* value) {
            const ProtoList* bucket = bucketAt(context, toImpl<const ProtoMultisetImplementation>(self)->list, hash);
            const int at = bucket ? pairInBucket(context, bucket, value) : -1;
            return at >= 0 ? bucket->getAt(context, at + 1) : nullptr;
        }

        long long countUnder(ProtoContext* context, const ProtoMultiset* self, proto_ulong hash, const ProtoObject* value) {
            const ProtoObject* c = countObjectUnder(context, self, hash, value);
            return c ? c->asLong(context) : 0;
        }

        const ProtoMultiset* removeUnder(ProtoContext* context, const ProtoMultiset* self, proto_ulong hash, const ProtoObject* value) {
            const auto* impl = toImpl<const ProtoMultisetImplementation>(self);
            const ProtoList* bucket = bucketAt(context, impl->list, hash);
            const int at = bucket ? pairInBucket(context, bucket, value) : -1;
            if (at < 0) return self;
            const long long count = countAt(context, bucket, at);
            // GC critical section: same rationale as addUnder.
            ProtoContext::CriticalSection cs(context);
            if (count > 1) {
                const ProtoList* newBucket = bucket->setAt(context, at + 1, context->fromInteger(count - 1));
                return wrap(context, impl->list->implSetAt(context, hash, newBucket->asObject(context)),
                            impl->size - 1, impl->distinct);
            }
            // The last occurrence: drop the pair, and the bucket with it when
            // it was the only one.
            const ProtoSparseListImplementation* newList = bucket->getSize(context) == 2
                ? impl->list->implRemoveAt(context, hash)
                : impl->list->implSetAt(context, hash, bucket->removeSlice(context, at, at + 2)->asObject(context));
            return wrap(context, newList, impl->size - 1, impl->distinct - 1);
        }
    }

    int ProtoMultisetIteratorImplementation::implHasNext(ProtoContext* context) const {
        return iterator ? iterator->implHasNext() : false;
    }

    const ProtoObject* ProtoMultisetIteratorImplementation::implNext(ProtoContext* context) {
        if (!iterator || !iterator->implHasNext()) return PROTO_NONE;
        return iterator->implNextValue()->asList(context)->getAt(context, static_cast<int>(2 * index));
    }

    const ProtoObject* ProtoMultisetIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.multisetIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_MULTISET_ITERATOR;
        return p.oid;
    }

    void ProtoMultisetIteratorImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (iterator) {
            method(context, self, iterator);
        }
    }

    // ProtoMultiset / ProtoMultisetIterator external API trampolines
    const ProtoMultiset* ProtoMultiset::add(ProtoContext* context, const ProtoObject* value) const {
        return addUnder(context, this, value->getHash(context), value);
    }

    const ProtoObject* ProtoMultiset::count(ProtoContext* context, const ProtoObject* value) const {
        const ProtoObject* c = countObjectUnder(context, this, value->getHash(context), value);
        return c ? c : context->fromInteger(0);
    }

    const ProtoObject* ProtoMultiset::has(ProtoContext* context, const ProtoObject* value) const {
        return countObjectUnder(context, this, value->getHash(context), value) ? PROTO_TRUE : PROTO_FALSE;
    }

    const ProtoMultiset* ProtoMultiset::remove(ProtoContext* context, const ProtoObject* value) const {
        return removeUnder(context, this, value->getHash(context), value);
    }

    const ProtoMultiset* ProtoMultiset::addWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const {
        return addUnder(context, this, hash, value);
    }

    proto_ulong ProtoMultiset::countWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const {
        return static_cast<proto_ulong>(countUnder(context, this, hash, value));
    }

    bool ProtoMultiset::hasWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const {
        return countObjectUnder(context, this, hash, value) != nullptr;
    }

    const ProtoMultiset* ProtoMultiset::removeWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const {
        return removeUnder(context, this, hash, value);
    }

    proto_ulong ProtoMultiset::getSize(ProtoContext* context) const { return toImpl<const ProtoMultisetImplementation>(this)->size; }
    proto_ulong ProtoMultiset::getDistinctSize(ProtoContext* context) const { return toImpl<const ProtoMultisetImplementation>(this)->distinct; }
    const ProtoObject* ProtoMultiset::asObject(ProtoContext* context) const { return toImpl<const ProtoMultisetImplementation>(this)->implAsObject(context); }
    const ProtoMultisetIterator* ProtoMultiset::getIterator(ProtoContext* context) const {
        const auto* list_iterator = toImpl<const ProtoMultisetImplementation>(this)->list->implGetIterator(context);
        if (!list_iterator) return nullptr;
        return (new (context) ProtoMultisetIteratorImplementation(context, list_iterator))->asMultisetIterator(context);
    }

    int ProtoMultisetIterator::hasNext(ProtoContext* context) const { if (!this) return 0; return toImpl<const ProtoMultisetIteratorImplementation>(this)->implHasNext(context); }
    const ProtoObject* ProtoMultisetIterator::next(ProtoContext* context) const { if (!this) return nullptr; return const_cast<ProtoMultisetIteratorImplementation*>(toImpl<const ProtoMultisetIteratorImplementation>(this))->implNext(context); }
    const ProtoMultisetIterator* ProtoMultisetIterator::advance(ProtoContext* context) const {
        if (!this) return nullptr;
        const auto* impl = toImpl<const ProtoMultisetIteratorImplementation>(this);
        if (!impl->iterator || !impl->iterator->implHasNext())
            return (new (context) ProtoMultisetIteratorImplementation(context, nullptr))->asMultisetIterator(context);
        // The next element of the same bucket, if any; otherwise the first
        // element of the next bucket.
        const ProtoList* bucket = impl->iterator->implNextValue()->asList(context);
        if (2 * (impl->index + 1) < bucket->getSize(context))
            return (new (context) ProtoMultisetIteratorImplementation(context, impl->iterator, impl->index + 1))->asMultisetIterator(context);
        const ProtoSparseListIteratorImplementation* advanced = impl->iterator->implAdvance(context);
        return (new (context) ProtoMultisetIteratorImplementation(context, advanced))->asMultisetIterator(context);
    }
    const ProtoObject* ProtoMultisetIterator::asObject(ProtoContext* context) const { if (!this) return nullptr; return toImpl<const ProtoMultisetIteratorImplementation>(this)->implAsObject(context); }
    proto_ulong ProtoMultisetIterator::nextCount(ProtoContext* context) const {
        if (!this) return 0;
        const auto* impl = toImpl<const ProtoMultisetIteratorImplementation>(this);
        if (!impl->iterator || !impl->iterator->implHasNext()) return 0;
        const ProtoList* bucket = impl->iterator->implNextValue()->asList(context);
        return static_cast<proto_ulong>(bucket->getAt(context, static_cast<int>(2 * impl->index + 1))->asLong(context));
    }
    proto_ulong ProtoMultisetIterator::nextHash(ProtoContext* context) const {
        if (!this) return 0;
        const auto* impl = toImpl<const ProtoMultisetIteratorImplementation>(this);
        return impl->iterator ? impl->iterator->implNextKey() : 0;
    }
}
