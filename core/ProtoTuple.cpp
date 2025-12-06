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
                return new(context) ProtoTupleImplementation(context, nullptr, nullptr);
            }

            if (count <= TUPLE_SIZE) {
                const ProtoObject* data[TUPLE_SIZE] = {nullptr};
                for (unsigned long i = 0; i < count; ++i) {
                    data[i] = list->getAt(context, start + i);
                }
                return new(context) ProtoTupleImplementation(context, data, nullptr);
            }

            const unsigned long children_count = (count + TUPLE_SIZE - 1) / TUPLE_SIZE;
            const ProtoTupleImplementation* indirect[TUPLE_SIZE] = {nullptr};
            unsigned long child_height = 0;

            for (unsigned long i = 0; i < children_count; ++i) {
                const unsigned long child_start = start + i * TUPLE_SIZE;
                const unsigned long child_end = std::min(child_start + TUPLE_SIZE, end);
                indirect[i] = fromListRecursive(context, list, child_start, child_end);
                if(indirect[i]) child_height = std::max(child_height, (long unsigned int)indirect[i]->implGetSize(context));
            }

            const ProtoObject** indirect_as_objects = (const ProtoObject**)indirect;
            return new(context) ProtoTupleImplementation(context, indirect_as_objects, nullptr);
        }
    }

    const ProtoTupleImplementation* ProtoTupleImplementation::tupleFromList(ProtoContext* context, const ProtoListImplementation* list) {
        return fromListRecursive(context, list->asProtoList(context), 0, list->size);
    }

    ProtoTupleImplementation::ProtoTupleImplementation(
        ProtoContext* context,
        const ProtoObject** slot_values,
        const ProtoTupleImplementation* next_tuple
    ) : Cell(context), next(next_tuple) {
        if (slot_values) {
            std::memcpy(this->slot, slot_values, TUPLE_SIZE * sizeof(ProtoObject*));
        } else {
            std::memset(this->slot, 0, TUPLE_SIZE * sizeof(ProtoObject*));
        }
    }

    const ProtoObject* ProtoTupleImplementation::implGetAt(ProtoContext* context, int index) const {
        const ProtoTupleImplementation* current = this;
        while(current != nullptr && current->next != nullptr) {
            int child_index = index / TUPLE_SIZE;
            index %= TUPLE_SIZE;
            current = toImpl<const ProtoTupleImplementation>(current->slot[child_index]);
        }
        return current->slot[index];
    }

    unsigned long ProtoTupleImplementation::implGetSize(ProtoContext* context) const {
        const ProtoTupleImplementation* current = this;
        unsigned long count = 0;
        while(current != nullptr && current->next != nullptr) {
            count += TUPLE_SIZE;
            current = current->next;
        }
        if (current) {
            for(int i = 0; i < TUPLE_SIZE; ++i) {
                if (current->slot[i]) {
                    count++;
                }
            }
        }
        return count;
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
        if (this->next) {
            method(context, self, this->next);
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
        return (new (context) ProtoTupleImplementation(context, nullptr, nullptr))->asProtoTuple(context);
    }
}
