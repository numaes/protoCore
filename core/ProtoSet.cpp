/*
 * ProtoSet.cpp
 *
 *  Created on: 2024-05-28
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

namespace proto {

    ProtoSetImplementation::ProtoSetImplementation(
        ProtoContext* context,
        const ProtoSparseListImplementation* list,
        proto_ulong size
    ) : Cell(context), list(list), size(size)
    {
    }

    void ProtoSetImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (list) {
            // list is a raw IMPL pointer; pass directly to the GC tracer.
            method(context, self, list);
        }
    }

    const ProtoObject* ProtoSetImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.setImplementation = this;
        p.op.pointer_tag = POINTER_TAG_SET;
        return p.oid;
    }

    const ProtoSet* ProtoSetImplementation::asProtoSet(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.setImplementation = this;
        p.op.pointer_tag = POINTER_TAG_SET;
        return p.set;
    }

    ProtoSetIteratorImplementation::ProtoSetIteratorImplementation(
        ProtoContext* context,
        const ProtoSparseListIteratorImplementation* it,
        proto_ulong index
    ) : Cell(context), iterator(it), index(index)
    {
    }

    namespace {
        // The bucket stored under `hash`, or nullptr.  A bucket is always a
        // non-empty ProtoList (see ProtoSetImplementation).
        const ProtoList* bucketAt(ProtoContext* context, const ProtoSparseListImplementation* list, proto_ulong hash) {
            const ProtoObject* slot = list->implGetAt(context, hash);
            return slot ? slot->asList(context) : nullptr;
        }

        // Position of `value` (by pointer) in `bucket`, or -1.
        int indexInBucket(ProtoContext* context, const ProtoList* bucket, const ProtoObject* value) {
            const int n = static_cast<int>(bucket->getSize(context));
            for (int i = 0; i < n; ++i)
                if (bucket->getAt(context, i) == value) return i;
            return -1;
        }

        const ProtoSet* wrap(ProtoContext* context, const ProtoSparseListImplementation* list, proto_ulong size) {
            return (new (context) ProtoSetImplementation(context, list, size))->asProtoSet(context);
        }

        const ProtoSet* addUnder(ProtoContext* context, const ProtoSet* self, proto_ulong hash, const ProtoObject* value) {
            const auto* impl = toImpl<const ProtoSetImplementation>(self);
            const ProtoList* bucket = bucketAt(context, impl->list, hash);
            if (bucket && indexInBucket(context, bucket, value) >= 0) return self;
            // GC critical section: the new bucket and the chain of new
            // SparseList nodes are reachable only through C++ locals until
            // the wrapping ProtoSetImplementation stitches them in.
            ProtoContext::CriticalSection cs(context);
            const ProtoList* newBucket = bucket
                ? bucket->appendLast(context, value)
                : context->newList(1, &value);
            const auto* newList = impl->list->implSetAt(context, hash, newBucket->asObject(context));
            return wrap(context, newList, impl->size + 1);
        }

        bool hasUnder(ProtoContext* context, const ProtoSet* self, proto_ulong hash, const ProtoObject* value) {
            const ProtoList* bucket = bucketAt(context, toImpl<const ProtoSetImplementation>(self)->list, hash);
            return bucket && indexInBucket(context, bucket, value) >= 0;
        }

        const ProtoSet* removeUnder(ProtoContext* context, const ProtoSet* self, proto_ulong hash, const ProtoObject* value) {
            const auto* impl = toImpl<const ProtoSetImplementation>(self);
            const ProtoList* bucket = bucketAt(context, impl->list, hash);
            const int at = bucket ? indexInBucket(context, bucket, value) : -1;
            if (at < 0) return self;
            // GC critical section: same rationale as addUnder.
            ProtoContext::CriticalSection cs(context);
            const ProtoSparseListImplementation* newList = bucket->getSize(context) == 1
                ? impl->list->implRemoveAt(context, hash)
                : impl->list->implSetAt(context, hash, bucket->removeAt(context, at)->asObject(context));
            return wrap(context, newList, impl->size - 1);
        }
    }

    int ProtoSetIteratorImplementation::implHasNext(ProtoContext* context) const {
        return iterator ? iterator->implHasNext() : false;
    }

    const ProtoObject* ProtoSetIteratorImplementation::implNext(ProtoContext* context) {
        if (!iterator || !iterator->implHasNext()) return PROTO_NONE;
        return iterator->implNextValue()->asList(context)->getAt(context, static_cast<int>(index));
    }

    const ProtoObject* ProtoSetIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.setIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_SET_ITERATOR;
        return p.oid;
    }

    void ProtoSetIteratorImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (iterator) {
            method(context, self, iterator);
        }
    }

    // ProtoSet / ProtoSetIterator external API trampolines
    const ProtoSet* ProtoSet::add(ProtoContext* context, const ProtoObject* value) const {
        return addUnder(context, this, value->getHash(context), value);
    }

    const ProtoObject* ProtoSet::has(ProtoContext* context, const ProtoObject* value) const {
        return hasUnder(context, this, value->getHash(context), value) ? PROTO_TRUE : PROTO_FALSE;
    }

    const ProtoSet* ProtoSet::remove(ProtoContext* context, const ProtoObject* value) const {
        return removeUnder(context, this, value->getHash(context), value);
    }

    const ProtoSet* ProtoSet::addWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const {
        return addUnder(context, this, hash, value);
    }

    bool ProtoSet::hasWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const {
        return hasUnder(context, this, hash, value);
    }

    const ProtoSet* ProtoSet::removeWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const {
        return removeUnder(context, this, hash, value);
    }

    bool ProtoSet::hasHash(ProtoContext* context, proto_ulong hash) const {
        return toImpl<const ProtoSetImplementation>(this)->list->implHas(context, hash);
    }

    const ProtoSet* ProtoSet::removeHash(ProtoContext* context, proto_ulong hash) const {
        const auto* impl = toImpl<const ProtoSetImplementation>(this);
        const ProtoList* bucket = bucketAt(context, impl->list, hash);
        if (!bucket) return this;
        const proto_ulong removed = bucket->getSize(context);
        // GC critical section: same rationale as addUnder.
        ProtoContext::CriticalSection cs(context);
        return wrap(context, impl->list->implRemoveAt(context, hash), impl->size - removed);
    }

    proto_ulong ProtoSet::getSize(ProtoContext* context) const { return toImpl<const ProtoSetImplementation>(this)->size; }
    const ProtoObject* ProtoSet::asObject(ProtoContext* context) const { return toImpl<const ProtoSetImplementation>(this)->implAsObject(context); }
    const ProtoSetIterator* ProtoSet::getIterator(ProtoContext* context) const {
        const auto* list_iterator = toImpl<const ProtoSetImplementation>(this)->list->implGetIterator(context);
        if (!list_iterator) return nullptr;
        return (new (context) ProtoSetIteratorImplementation(context, list_iterator))->asSetIterator(context);
    }

    int ProtoSetIterator::hasNext(ProtoContext* context) const { if (!this) return 0; return toImpl<const ProtoSetIteratorImplementation>(this)->implHasNext(context); }
    const ProtoObject* ProtoSetIterator::next(ProtoContext* context) const { if (!this) return nullptr; return const_cast<ProtoSetIteratorImplementation*>(toImpl<const ProtoSetIteratorImplementation>(this))->implNext(context); }
    const ProtoSetIterator* ProtoSetIterator::advance(ProtoContext* context) const {
        if (!this) return nullptr;
        const auto* impl = toImpl<const ProtoSetIteratorImplementation>(this);
        if (!impl->iterator || !impl->iterator->implHasNext())
            return (new (context) ProtoSetIteratorImplementation(context, nullptr))->asSetIterator(context);
        // The next element of the same bucket, if any; otherwise the first
        // element of the next bucket.
        const ProtoList* bucket = impl->iterator->implNextValue()->asList(context);
        if (impl->index + 1 < bucket->getSize(context))
            return (new (context) ProtoSetIteratorImplementation(context, impl->iterator, impl->index + 1))->asSetIterator(context);
        const ProtoSparseListIteratorImplementation* advanced = impl->iterator->implAdvance(context);
        return (new (context) ProtoSetIteratorImplementation(context, advanced))->asSetIterator(context);
    }
    const ProtoObject* ProtoSetIterator::asObject(ProtoContext* context) const { if (!this) return nullptr; return toImpl<const ProtoSetIteratorImplementation>(this)->implAsObject(context); }
    proto_ulong ProtoSetIterator::nextHash(ProtoContext* context) const {
        if (!this) return 0;
        const auto* impl = toImpl<const ProtoSetIteratorImplementation>(this);
        return impl->iterator ? impl->iterator->implNextKey() : 0;
    }
}
