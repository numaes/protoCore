/*
 * ProtoTuple.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <cstring>

namespace proto {

    namespace {
        int getBalance(const TupleDictionary* node) {
            if (!node) return 0;
            return (node->next ? node->height : 0) - (node->previous ? node->height : 0);
        }
    }

    TupleDictionary::TupleDictionary(
        ProtoContext* context,
        const ProtoTupleImplementation* key,
        TupleDictionary* previous,
        TupleDictionary* next
    ): Cell(context), key(key), previous(previous), next(next) {
        height = 1 + std::max(
            (this->previous ? this->previous->height : 0),
            (this->next ? this->next->height : 0)
        );
    }

    void TupleDictionary::finalize(ProtoContext* context) const {
    }

    void TupleDictionary::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const {
        if (this->key) {
            method(context, self, this->key);
        }
        if (this->previous) {
            method(context, self, this->previous);
        }
        if (this->next) {
            method(context, self, this->next);
        }
    }

    const ProtoObject* TupleDictionary::implAsObject(ProtoContext* context) const {
        return PROTO_NONE;
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
        const ProtoTupleImplementation* internTuple(ProtoContext* context, const ProtoTupleImplementation* newTuple);

        const ProtoTupleImplementation* fromListRecursive(
            ProtoContext* context,
            const ProtoList* list,
            unsigned long start,
            unsigned long end
        ) {
            const unsigned long count = end - start;
            if (count == 0) {
                return internTuple(context, new(context) ProtoTupleImplementation(context, nullptr, 0UL));
            }

            if (count <= TUPLE_SIZE) {
                const ProtoObject* data[TUPLE_SIZE] = {nullptr};
                for (unsigned long i = 0; i < count; ++i) {
                    data[i] = list->getAt(context, start + i);
                }
                return internTuple(context, new(context) ProtoTupleImplementation(context, data, count));
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
            return internTuple(context, new(context) ProtoTupleImplementation(context, indirect_handles, count));
        }
    }

    namespace {
        // Compare two tuples for identity equality of their contents
        int compareTuples(ProtoContext* context, const ProtoTupleImplementation* t1, const ProtoTupleImplementation* t2) {
            if (t1 == t2) return 0;
            unsigned long s1 = t1->implGetSize(context);
            unsigned long s2 = t2->implGetSize(context);
            if (s1 < s2) return -1;
            if (s1 > s2) return 1;

            unsigned long h1 = t1->getHash(context);
            unsigned long h2 = t2->getHash(context);
            if (h1 < h2) return -1;
            if (h1 > h2) return 1;

            for (unsigned long i = 0; i < s1; ++i) {
                const ProtoObject* o1 = t1->implGetAt(context, i);
                const ProtoObject* o2 = t2->implGetAt(context, i);
                if (o1 < o2) return -1;
                if (o1 > o2) return 1;
            }
            return 0;
        }

        const ProtoTupleImplementation* internTuple(ProtoContext* context, const ProtoTupleImplementation* newTuple) {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            ProtoSpace* space = context->space;
            TupleDictionary* current = space->tupleRoot.load();
            TupleDictionary* parent = nullptr;
            
            while (current) {
                int cmp = compareTuples(context, newTuple, current->key);
                if (cmp == 0) {
                    return current->key; // Found existing tuple
                }
                parent = current;
                if (cmp < 0) {
                    current = current->previous;
                } else {
                    current = current->next;
                }
            }

            // Not found, insert new node
            TupleDictionary* newNode = new(context) TupleDictionary(context, newTuple, nullptr, nullptr);
            if (!parent) {
                space->tupleRoot.store(newNode);
            } else {
                int cmp = compareTuples(context, newTuple, parent->key);
                if (cmp < 0) {
                    parent->previous = newNode;
                } else {
                    parent->next = newNode;
                }
            }
            return newTuple;
        }
    }

    const ProtoTupleImplementation* ProtoTupleImplementation::tupleFromList(ProtoContext* context, const ProtoListImplementation* list) {
        const ProtoTupleImplementation* rawTuple = fromListRecursive(context, list->asProtoList(context), 0, list->size);
        const ProtoTupleImplementation* internedTuple = internTuple(context, rawTuple);
        // If interned came back different, rawTuple is redundant (garbage) but not easily deletable here due to GC.
        // It will be collected eventually as it's not referenced by anyone.
        return internedTuple;
    }

    const ProtoTupleImplementation* ProtoTupleImplementation::tupleConcat(ProtoContext* context, const ProtoObject* left, const ProtoObject* right, unsigned long totalSize) {
        const ProtoObject* slots[2] = { left, right };
        return new(context) ProtoTupleImplementation(context, slots, totalSize);
    }

    ProtoTupleImplementation::ProtoTupleImplementation(
        ProtoContext* context,
        const ProtoObject** slot_values,
        unsigned long size
    ) : Cell(context), actual_size(size){
        if (slot_values) {
            std::memcpy(this->slot, slot_values, TUPLE_SIZE * sizeof(ProtoObject*));
        } else {
            std::memset(this->slot, 0, TUPLE_SIZE * sizeof(ProtoObject*));
        }
    }

    const ProtoObject* ProtoTupleImplementation::implGetAt(ProtoContext* context, int index) const
    {
        if (index < 0 || (unsigned long)index >= actual_size) {
            return PROTO_NONE; // Index out of bounds
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
            if (slot[i] && slot[i]->isCell(context)) {
                method(context, self, slot[i]->asCell(context));
            }
        }
        // If this is an internal node, the 'slot' elements are themselves tuples,
        // and their references will be processed when their 'processReferences' is called.
        // If this is a leaf node, the 'slot' elements are direct values.
        // The current logic correctly processes direct cells.
        // No further recursive calls needed here, as the GC will trace through the child tuples.
    }

    unsigned long ProtoTupleImplementation::getHash(ProtoContext* context) const {
        unsigned long current_hash = 0;
        // Combine hashes of all elements in the slot
        for (int i = 0; i < TUPLE_SIZE; ++i) {
            if (slot[i]) {
                current_hash ^= slot[i]->getHash(context);
            }
        }
        // For interning, the hash must also depend on the structure and size.
        // We can combine the current_hash with the actual_size.
        // A simple way is to shift and XOR, or use a prime multiplier.
        // Let's use a simple combination for now.
        current_hash = (current_hash << 1) ^ actual_size;

        // If this is an internal node, the hash of its children (which are also tuples)
        // is already incorporated into the slot[i]->getHash(context) call.
        // So, the current logic is fine for a rope structure.

        return current_hash;
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

    const ProtoObject* ProtoTuple::getSlice(ProtoContext* context, int start, int end) const {
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
}
