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


    //=========================================================================
    // ProtoTupleImplementation
    //=========================================================================

    namespace {
        const ProtoTupleImplementation* fromListRecursive(
            ProtoContext* context,
            const ProtoList* list,
            unsigned long start,
            unsigned long end
        ) {
            const unsigned long count = end - start;
            if (count == 0) {
                return new(context) ProtoTupleImplementation(context, nullptr, 0UL);
            }

            if (count <= TUPLE_SIZE) {
                const ProtoObject* data[TUPLE_SIZE] = {nullptr};
                for (unsigned long i = 0; i < count; ++i) {
                    data[i] = list->getAt(context, start + i);
                }
                return new(context) ProtoTupleImplementation(context, data, count);
            }

            const unsigned long children_count = (count + TUPLE_SIZE - 1) / TUPLE_SIZE;
            const ProtoTupleImplementation* indirect[TUPLE_SIZE] = {nullptr};
            const ProtoObject* indirect_handles[TUPLE_SIZE] = {nullptr};
            unsigned long child_height = 0;

            for (unsigned long i = 0; i < children_count; ++i) {
                const unsigned long child_start = start + i * TUPLE_SIZE;
                const unsigned long child_end = std::min(child_start + TUPLE_SIZE, end);
                const ProtoTupleImplementation* child_impl = fromListRecursive(context, list, child_start, child_end); // Recursive call returns implementation
                if (child_impl) {
                    indirect_handles[i] = child_impl->implAsObject(context); // Convert to universal ProtoObject* handle
                }
            }
            return new(context) ProtoTupleImplementation(context, indirect_handles, count);
        }
    }

    const ProtoTupleImplementation* ProtoTupleImplementation::tupleFromList(ProtoContext* context, const ProtoListImplementation* list) {
        return fromListRecursive(context, list->asProtoList(context), 0, list->size);
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
        } else
        {
            // This is an internal node
            unsigned long current_child_start_index = 0;
            for (int i = 0; i < TUPLE_SIZE; ++i) {
                if (slot[i] != PROTO_NONE && slot[i]->isTuple(context)) {
                    const ProtoTupleImplementation* child_tuple = toImpl<const ProtoTupleImplementation>(slot[i]);
                    // Check if the index falls within this child's range
                    if ((unsigned long)index < current_child_start_index + child_tuple->actual_size) {
                        // Recursively get the element from the child tuple
                        return child_tuple->implGetAt(context, index - current_child_start_index);
                    }
                    current_child_start_index += child_tuple->actual_size;
                } else if (slot[i] != PROTO_NONE) {
                    // This slot contains a non-tuple object, which means it's a mixed node or an error in construction.
                    // For a pure rope, internal nodes should only contain other tuples.
                    // For now, we'll treat it as a direct element if it's within the index range.
                    if ((unsigned long)index == current_child_start_index) {
                        return slot[i];
                    }
                    current_child_start_index++;
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
        for (int i = 0; i < TUPLE_SIZE; ++i) {
            if (this->slot[i] && this->slot[i]->isCell(context)) {
                method(context, self, this->slot[i]->asCell(context));
            }
        }
    }

    unsigned long ProtoTupleImplementation::getHash(ProtoContext* context) const {
        // Simple hash for now
        unsigned long hash = 0;
        for (int i = 0; i < TUPLE_SIZE; ++i) {
            if (this->slot[i]) {
                hash ^= this->slot[i]->getHash(context);
            }
        }
        if (this->next) {
            hash ^= this->next->getHash(context);
        }
        return hash;
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

    const ProtoObject* ProtoTuple::getSlice(ProtoContext* context, int start, int end) const {
        const ProtoList* list = toImpl<const ProtoTupleImplementation>(this)->implAsList(context);
        const ProtoList* sublist = context->newList();
        for(int i = start; i < end; ++i) {
            sublist = sublist->appendLast(context, list->getAt(context, i));
        }
        return (new (context) ProtoTupleImplementation(context, nullptr, 0UL))->implAsObject(context);
    }
}
