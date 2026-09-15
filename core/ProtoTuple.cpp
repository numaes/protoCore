/*
 * ProtoTuple.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <algorithm>
#include <cstring>

namespace proto {

    //=========================================================================
    // TupleInterner
    //=========================================================================

    TupleInterner::~TupleInterner() {
        for (Shard& shard : shards) {
            delete[] shard.buckets;
            Chunk* chunk = shard.first.load(std::memory_order_relaxed);
            while (chunk) {
                Chunk* next = chunk->next.load(std::memory_order_relaxed);
                delete chunk;
                chunk = next;
            }
        }
    }

    uint64_t TupleInterner::hashSlots(const ProtoObject** slots, unsigned long size) {
        // Tuple nodes are equal when their size and slot pointers are equal, so
        // hash exactly that: mix each pointer, then avalanche (pointers share
        // their alignment bits, and the shard index takes the high bits).
        uint64_t h = 0x9E3779B97F4A7C15ULL ^ static_cast<uint64_t>(size);
        for (int i = 0; i < TUPLE_SIZE; ++i) {
            h ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(slots[i]));
            h *= 0xFF51AFD7ED558CCDULL;
            h ^= h >> 32;
        }
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ULL;
        h ^= h >> 33;
        return h;
    }

    TupleInterner::Entry* TupleInterner::find(const Shard& shard, uint64_t hash,
                                             const ProtoObject** slots, unsigned long size) {
        if (!shard.buckets) return nullptr;
        for (Entry* e = shard.buckets[hash & (shard.bucketCount - 1)]; e; e = e->chain) {
            if (e->hash == hash && e->tuple->actual_size == size &&
                std::memcmp(e->tuple->slot, slots, TUPLE_SIZE * sizeof(ProtoObject*)) == 0) {
                return e;
            }
        }
        return nullptr;
    }

    void TupleInterner::insertLocked(Shard& shard, uint64_t hash, const ProtoTupleImplementation* tuple) {
        const size_t count = shard.published.load(std::memory_order_relaxed);
        // Keep the load factor at most 1. Entries never move: growing the index
        // rebuilds only the bucket array and the entries' chain links, which the
        // GC never reads.
        if (count + 1 > shard.bucketCount) {
            const size_t newBucketCount = shard.bucketCount ? shard.bucketCount * 2 : 16;
            Entry** fresh = new Entry*[newBucketCount]();
            size_t remaining = count;
            for (Chunk* c = shard.first.load(std::memory_order_relaxed); c && remaining;
                 c = c->next.load(std::memory_order_relaxed)) {
                const size_t n = std::min(remaining, CHUNK_SIZE);
                for (size_t i = 0; i < n; ++i) {
                    Entry& e = c->entries[i];
                    Entry*& head = fresh[e.hash & (newBucketCount - 1)];
                    e.chain = head;
                    head = &e;
                }
                remaining -= n;
            }
            delete[] shard.buckets;
            shard.buckets = fresh;
            shard.bucketCount = newBucketCount;
        }
        const size_t index = count % CHUNK_SIZE;
        if (index == 0) {
            Chunk* chunk = new Chunk();
            if (shard.last) shard.last->next.store(chunk, std::memory_order_release);
            else shard.first.store(chunk, std::memory_order_release);
            shard.last = chunk;
        }
        Entry& entry = shard.last->entries[index];
        entry.hash = hash;
        entry.tuple = tuple;
        Entry*& head = shard.buckets[hash & (shard.bucketCount - 1)];
        entry.chain = head;
        head = &entry;
        // Publish last: the GC walks only published entries.
        shard.published.store(count + 1, std::memory_order_release);
    }

    const ProtoTupleImplementation* TupleInterner::intern(ProtoContext* context,
                                                         const ProtoObject** slots,
                                                         unsigned long size) {
        const uint64_t hash = hashSlots(slots, size);
        Shard& shard = shards[hash >> 58];
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            if (const Entry* e = find(shard, hash, slots, size)) return e->tuple;
        }
        // First sight. Allocate outside the shard lock (an allocation may reach
        // a GC safepoint), then insert unless another thread interned an equal
        // tuple meanwhile; a losing candidate is ordinary garbage. The new node
        // is a young cell of `context`, protected by it until the table
        // snapshot of a later GC cycle covers the entry.
        const ProtoTupleImplementation* candidate =
            new(context) ProtoTupleImplementation(context, slots, size);
        std::lock_guard<std::mutex> lock(shard.mutex);
        if (const Entry* e = find(shard, hash, slots, size)) return e->tuple;
        insertLocked(shard, hash, candidate);
        return candidate;
    }

    void TupleInterner::captureForGC() {
        for (Shard& shard : shards) {
            shard.gcCaptured = shard.published.load(std::memory_order_acquire);
        }
    }

    void TupleInterner::forEachCaptured(void* user, void (*visit)(void* user, const Cell* tuple)) const {
        for (const Shard& shard : shards) {
            size_t remaining = shard.gcCaptured;
            for (const Chunk* c = shard.first.load(std::memory_order_acquire); c && remaining;
                 c = c->next.load(std::memory_order_acquire)) {
                const size_t n = std::min(remaining, CHUNK_SIZE);
                for (size_t i = 0; i < n; ++i) visit(user, c->entries[i].tuple);
                remaining -= n;
            }
        }
    }

    size_t TupleInterner::size() const {
        size_t total = 0;
        for (const Shard& shard : shards) {
            total += shard.published.load(std::memory_order_acquire);
        }
        return total;
    }


    //=========================================================================
    // ProtoTupleIteratorImplementation
    //=========================================================================

    ProtoTupleIteratorImplementation::ProtoTupleIteratorImplementation(
        ProtoContext* context,
        const ProtoTupleImplementation* t,
        int i
    ) : Cell(context), base(t), currentIndex(i)
    {
    }

    int ProtoTupleIteratorImplementation::implHasNext(ProtoContext* context) const {
        return this->currentIndex < (int)this->base->implGetSize(context);
    }

    const ProtoObject* ProtoTupleIteratorImplementation::implNext(ProtoContext* context) {
        return this->base->implGetAt(context, this->currentIndex++);
    }

    const ProtoTupleIteratorImplementation* ProtoTupleIteratorImplementation::implAdvance(ProtoContext* context) const {
        if (this->currentIndex < (int)this->base->implGetSize(context)) {
            return new (context) ProtoTupleIteratorImplementation(context, this->base, this->currentIndex + 1);
        }
        return this;
    }

    const ProtoObject* ProtoTupleIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.tupleIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_TUPLE_ITERATOR;
        return p.oid;
    }

    void ProtoTupleIteratorImplementation::finalize(ProtoContext* context) const {}

    void ProtoTupleIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
            )
    ) const {
        if (this->base) {
            method(context, self, this->base);
        }
    }

    unsigned long ProtoTupleIteratorImplementation::getHash(ProtoContext* context) const {
        return reinterpret_cast<uintptr_t>(this);
    }
    
    const ProtoTupleIterator* ProtoTupleIteratorImplementation::asProtoTupleIterator(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.tupleIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_TUPLE_ITERATOR;
        return p.tupleIterator;
    }


    //=========================================================================
    // ProtoTupleImplementation
    //=========================================================================

    namespace {
        // Every tuple node is canonicalized by the space's TupleInterner; null
        // `slots` is the empty tuple.
        const ProtoTupleImplementation* internTuple(ProtoContext* context, const ProtoObject** slots, unsigned long size) {
            static const ProtoObject* noSlots[TUPLE_SIZE] = {nullptr};
            return context->space->tupleInterner->intern(context, slots ? slots : noSlots, size);
        }

        const ProtoTupleImplementation* fromListRecursive(
            ProtoContext* context,
            const ProtoList* list,
            unsigned long start,
            unsigned long end
        ) {
            const unsigned long count = end - start;
            if (count == 0) {
                return internTuple(context, nullptr, 0UL);
            }

            if (count <= TUPLE_SIZE) {
                const ProtoObject* data[TUPLE_SIZE] = {nullptr};
                for (unsigned long i = 0; i < count; ++i) {
                    data[i] = list->getAt(context, start + i);
                }
                return internTuple(context, data, count);
            }

            const unsigned long chunk_size = (count + TUPLE_SIZE - 1) / TUPLE_SIZE;
            const ProtoObject* indirect_handles[TUPLE_SIZE] = {nullptr};

            for (unsigned long i = 0; i < TUPLE_SIZE; ++i) {
                const unsigned long child_start = start + i * chunk_size;
                if (child_start >= end) break;
                const unsigned long child_end = std::min(child_start + chunk_size, end);
                
                const ProtoTupleImplementation* child_impl = fromListRecursive(context, list, child_start, child_end);
                if (child_impl) {
                    indirect_handles[i] = child_impl->implAsObject(context);
                }
            }
            return internTuple(context, indirect_handles, count);
        }

        const ProtoTupleImplementation* fromVectorRecursive(
            ProtoContext* context,
            const std::vector<const ProtoObject*>& vec,
            unsigned long start,
            unsigned long end
        ) {
            const unsigned long count = end - start;
            if (count == 0) {
                return internTuple(context, nullptr, 0UL);
            }

            if (count <= TUPLE_SIZE) {
                const ProtoObject* data[TUPLE_SIZE] = {nullptr};
                for (unsigned long i = 0; i < count; ++i) {
                    data[i] = vec[start + i];
                }
                return internTuple(context, data, count);
            }

            const unsigned long chunk_size = (count + TUPLE_SIZE - 1) / TUPLE_SIZE;
            const ProtoObject* indirect_handles[TUPLE_SIZE] = {nullptr};

            for (unsigned long i = 0; i < TUPLE_SIZE; ++i) {
                const unsigned long child_start = start + i * chunk_size;
                if (child_start >= end) break;
                const unsigned long child_end = std::min(child_start + chunk_size, end);
                
                const ProtoTupleImplementation* child_impl = fromVectorRecursive(context, vec, child_start, child_end);
                if (child_impl) {
                    indirect_handles[i] = child_impl->implAsObject(context);
                }
            }
            return internTuple(context, indirect_handles, count);
        }
    }

    const ProtoTupleImplementation* ProtoTupleImplementation::tupleFromList(ProtoContext* context, const ProtoListImplementation* list) {
        // GC critical section: fromListRecursive allocates the tuple nodes
        // bottom-up; each finished child is held in a C++ local while its
        // parent is interned.
        ProtoContext::CriticalSection cs(context);
        return fromListRecursive(context, list->asProtoList(context), 0, list->size);
    }

    const ProtoTupleImplementation* ProtoTupleImplementation::tupleFromVector(ProtoContext* context, const std::vector<const ProtoObject*>& source) {
        ProtoContext::CriticalSection cs(context);
        return fromVectorRecursive(context, source, 0, source.size());
    }

    const ProtoTupleImplementation* ProtoTupleImplementation::tupleConcat(ProtoContext* context, const ProtoObject* left, const ProtoObject* right, unsigned long totalSize) {
        const ProtoObject* slots[TUPLE_SIZE] = { left, right, nullptr, nullptr };
        return new(context) ProtoTupleImplementation(context, slots, totalSize);
    }

    ProtoTupleImplementation::ProtoTupleImplementation(
        ProtoContext* context,
        const ProtoObject** slot_values,
        unsigned long size
    ) : Cell(context), actual_size(size){
        if (slot_values) {
            // We MUST NOT copy more than TUPLE_SIZE pointers.
            // If size > TUPLE_SIZE, this is an internal node where slots contain child tuples.
            // These child tuples are already prepared in slot_values.
            std::memcpy(this->slot, slot_values, TUPLE_SIZE * sizeof(ProtoObject*));
        } else {
            std::memset(this->slot, 0, TUPLE_SIZE * sizeof(ProtoObject*));
        }
    }

    const ProtoObject* ProtoTupleImplementation::implGetAt(ProtoContext* context, int index) const
    {
        if (index < 0 || (unsigned long)index >= actual_size) {
            return nullptr; // Index out of bounds
        }

        if (actual_size <= TUPLE_SIZE) { // This is a leaf node
            return slot[index];
        } else { // This is an internal node, its slots should only contain child tuples
            unsigned long current_child_start_index = 0;
            for (int i = 0; i < TUPLE_SIZE; ++i) {
                if (slot[i] != PROTO_NONE) { // All non-null slots in an internal node must be tuples
                    // Assert that slot[i] is indeed a tuple, otherwise it's a construction error
                    if (!slot[i]->isTuple(context)) {
                        std::cerr << "Error: Non-tuple object found in internal tuple node slot." << std::endl;
                        std::abort();
                    }
                    const ProtoTupleImplementation* child_tuple = toImpl<const ProtoTupleImplementation>(slot[i]);
                    if ((unsigned long)index < current_child_start_index + child_tuple->actual_size) {
                        return child_tuple->implGetAt(context, index - current_child_start_index);
                    }
                    current_child_start_index += child_tuple->actual_size;
                }
            }
            // Should not be reached if actual_size is correct and all slots are properly filled/handled
            return PROTO_NONE;
        }
    }

    unsigned long ProtoTupleImplementation::implGetSize(ProtoContext* context) const {
        // This method now simply returns the pre-calculated actual_size.
        return actual_size;
    }

    const ProtoList* ProtoTupleImplementation::implAsList(ProtoContext* context) const {
        ProtoList* list = const_cast<ProtoList*>(context->newList());
        for (unsigned long i = 0; i < this->implGetSize(context); ++i) {
            list = const_cast<ProtoList*>(list->appendLast(context, this->implGetAt(context, i)));
        }
        return list;
    }

    void ProtoTupleImplementation::finalize(ProtoContext* context) const {}

    void ProtoTupleImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        // Process references for all elements in the slot array
        for (int i = 0; i < TUPLE_SIZE; ++i) {
            if (const Cell* c = ProtoObject::asCellPointer(slot[i])) {
                method(context, self, c);
            }
        }
        // If this is an internal node, the 'slot' elements are themselves tuples,
        // and their references will be processed when their 'processReferences' is called.
        // If this is a leaf node, the 'slot' elements are direct values.
        // The current logic correctly processes direct cells.
        // No further recursive calls needed here, as the GC will trace through the child tuples.
    }

    unsigned long ProtoTupleImplementation::getHash(ProtoContext* context) const {
        // STRUCT-194: content-based hash.  The previous identity-based
        // hash (pointer address) returned different values for equal-
        // content tuples whose elements came from different sources
        // (e.g. interned literal strings vs `bytes.decode()` results).
        // Dicts keyed by such tuples lost the value on lookup despite
        // `in` reporting True.  Mix element hashes with a tuple-specific
        // seed/multiplier so equal-content tuples hash identically.
        const ProtoTuple* selfT = this->asProtoTuple(context);
        if (!selfT) return reinterpret_cast<uintptr_t>(this);
        unsigned long size = selfT->getSize(context);
        // FNV-1a-ish mix with a tuple-distinguishing seed so empty
        // tuple is non-zero and small tuples don't collide with their
        // string elements.
        unsigned long h = 0x345678UL ^ (size * 0xa6b3f7UL + 1UL);
        for (unsigned long i = 0; i < size; ++i) {
            const ProtoObject* e = selfT->getAt(context, static_cast<int>(i));
            unsigned long eh = e ? e->getHash(context) : 0UL;
            h = (h * 1000003UL) ^ eh;
        }
        return h;
    }

    const ProtoObject* ProtoTupleImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.tupleImplementation = this;
        p.op.pointer_tag = POINTER_TAG_TUPLE;
        return p.oid;
    }

    const ProtoTuple* ProtoTupleImplementation::asProtoTuple(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.tupleImplementation = this;
        p.op.pointer_tag = POINTER_TAG_TUPLE;
        return p.tuple;
    }


    //=========================================================================
    // ProtoTuple API
    //=========================================================================

    unsigned long ProtoTuple::getSize(ProtoContext* context) const {
        return toImpl<const ProtoTupleImplementation>(this)->implGetSize(context);
    }

    const ProtoObject* ProtoTuple::getAt(ProtoContext* context, int index) const {
        return toImpl<const ProtoTupleImplementation>(this)->implGetAt(context, index);
    }

    const ProtoObject* ProtoTuple::asObject(ProtoContext* context) const {
        return toImpl<const ProtoTupleImplementation>(this)->implAsObject(context);
    }

    const ProtoList* ProtoTuple::asList(ProtoContext* context) const {
        return toImpl<const ProtoTupleImplementation>(this)->implAsList(context);
    }

    const ProtoObject* ProtoTuple::getSlice(ProtoContext* context, int start, int end) const {
        // GC critical section: holds `sublist` (a freshly built ProtoList
        // returned by getSlice) in a C++ local across newTupleFromList,
        // which itself allocates.  Inner calls also enter their own
        // critical sections; the depth counter handles nesting.
        ProtoContext::CriticalSection cs(context);
        const ProtoList* list = toImpl<const ProtoTupleImplementation>(this)->implAsList(context);
        // Ensure start and end are within bounds and make sense
        start = std::max(0, start);
        end = std::min((int)list->getSize(context), end);

        if (start >= end) {
            return context->newTuple()->asObject(context); // Return an empty tuple if slice is empty
        }

        // Create a new tuple from the sub-list
        const ProtoList* sublist = list->getSlice(context, start, end);
        return context->newTupleFromList(sublist)->asObject(context);
    }

    const ProtoObject* ProtoTuple::getFirst(ProtoContext* context) const {
        unsigned long size = getSize(context);
        if (size == 0) return PROTO_NONE;
        return getAt(context, 0);
    }

    const ProtoObject* ProtoTuple::getLast(ProtoContext* context) const {
        unsigned long size = getSize(context);
        if (size == 0) return PROTO_NONE;
        return getAt(context, size - 1);
    }

    bool ProtoTuple::has(ProtoContext* context, const ProtoObject* value) const {
        unsigned long size = getSize(context);
        for (unsigned long i = 0; i < size; ++i) {
            const ProtoObject* elem = getAt(context, i);
            if (elem == value) {
                return true;
            }
            if (elem->isInteger(context) && value->isInteger(context)) {
                if (Integer::compare(context, elem, value) == 0) return true;
            } else if (elem->isString(context) && value->isString(context)) {
                if (elem->asString(context)->cmp_to_string(context, value->asString(context)) == 0) return true;
            }
        }
        return false;
    }

    // ProtoTupleIterator external API trampolines
    int ProtoTupleIterator::hasNext(ProtoContext* context) const { return toImpl<const ProtoTupleIteratorImplementation>(this)->implHasNext(context); }
    const ProtoObject* ProtoTupleIterator::next(ProtoContext* context) { return toImpl<ProtoTupleIteratorImplementation>(this)->implNext(context); }
    const ProtoTupleIterator* ProtoTupleIterator::advance(ProtoContext* context) { return toImpl<ProtoTupleIteratorImplementation>(this)->implAdvance(context)->asProtoTupleIterator(context); }
    const ProtoObject* ProtoTupleIterator::asObject(ProtoContext* context) const { return toImpl<const ProtoTupleIteratorImplementation>(this)->implAsObject(context); }
}
