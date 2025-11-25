/*
 * ProtoTuple.cpp
 *
 *  Revised and corrected in 2024 to incorporate the latest
 *  project improvements, such as modern memory management,
 *  logic fixes, and API consistency.
 */

#include "../headers/proto_internal.h"
#include <algorithm>
#include <vector>

namespace proto
{
    // --- TupleDictionary Implementation ---

    int getBalance(const TupleDictionary* node) {
        if (!node) return 0;
        return (node->next ? node->height : 0) - (node->previous ? node->height : 0);
    }

    TupleDictionary::TupleDictionary(
        ProtoContext* context,
        const ProtoTupleImplementation* key,
        TupleDictionary* next,
        TupleDictionary* previous
    ): Cell(context), key(key), previous(previous), next(next) {
        this->height = 1 + std::max(previous ? previous->height : 0, next ? next->height : 0);
        this->count = (key ? 1 : 0) + (previous ? previous->count : 0) + (next ? next->count : 0);
    }


    void TupleDictionary::finalize(ProtoContext* context) const {}

    void TupleDictionary::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const {
        if (this->key) method(context, self, this->key);
        if (this->previous) method(context, self, this->previous);
        if (this->next) method(context, self, this->next);
    }

    // ... (Rest of TupleDictionary implementation)

    // --- ProtoTupleIteratorImplementation ---

    ProtoTupleIteratorImplementation::ProtoTupleIteratorImplementation(
        ProtoContext* context,
        const ProtoTupleImplementation* base,
        int currentIndex
    ) : Cell(context), base(base), currentIndex(currentIndex) {}

    ProtoTupleIteratorImplementation::~ProtoTupleIteratorImplementation() = default;

    int ProtoTupleIteratorImplementation::implHasNext(ProtoContext* context) const {
        return this->currentIndex < this->base->count;
    }

    const ProtoObject* ProtoTupleIteratorImplementation::implNext(ProtoContext* context) const {
        return this->base->implGetAt(context, this->currentIndex);
    }

    const ProtoTupleIteratorImplementation* ProtoTupleIteratorImplementation::implAdvance(ProtoContext* context) const {
        return new(context) ProtoTupleIteratorImplementation(context, this->base, this->currentIndex + 1);
    }

    const ProtoObject* ProtoTupleIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.tupleIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_TUPLE_ITERATOR;
        return p.oid.oid;
    }

    void ProtoTupleIteratorImplementation::finalize(ProtoContext* context) const {}

    void ProtoTupleIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const {
        if (this->base) method(context, self, this->base);
    }

    unsigned long ProtoTupleIteratorImplementation::getHash(ProtoContext* context) const {
        return Cell::getHash(context);
    }

    // --- ProtoTupleImplementation ---

    ProtoTupleImplementation::ProtoTupleImplementation(
        ProtoContext* context,
        unsigned long elementCount,
        unsigned long height,
        const ProtoObject** data,
        const ProtoTupleImplementation** indirect
    ) : Cell(context) {
        this->count = elementCount;
        this->height = height;
        if (height == 0) {
            for (int i = 0; i < TUPLE_SIZE; i++) {
                this->pointers.data[i] = data && i < (int)elementCount ? data[i] : nullptr;
            }
        } else {
            for (int i = 0; i < TUPLE_SIZE; i++) {
                this->pointers.indirect[i] = (indirect && i < TUPLE_SIZE) ? indirect[i] : nullptr;
            }
        }
    }


    ProtoTupleImplementation::~ProtoTupleImplementation() = default;

    const ProtoTupleImplementation* ProtoTupleImplementation::tupleFromList(ProtoContext* context, const ProtoList* list) {
        // ... (Implementation using std::vector)
        return nullptr; // Placeholder
    }

    const ProtoObject* ProtoTupleImplementation::implGetAt(ProtoContext* context, int index) const {
        if (index < 0) index += this->count;
        if (index < 0 || (unsigned long)index >= this->count) return PROTO_NONE;

        const ProtoTupleImplementation* node = this;
        unsigned long h = this->height;
        while (h > 0) {
            unsigned long span = 1;
            for(unsigned long p = 0; p < h - 1; ++p) span *= TUPLE_SIZE;
            
            unsigned long chunk_index = index / span;
            node = node->pointers.indirect[chunk_index];
            if (!node) return PROTO_NONE;
            index %= span;
            h--;
        }
        return node->pointers.data[index];
    }

    void ProtoTupleImplementation::finalize(ProtoContext* context) const {
        // Intentionally empty
    }

    void ProtoTupleImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        // ... (lógica para recorrer this->pointers y llamar a method)
    }

    unsigned long ProtoTupleImplementation::getHash(ProtoContext* context) const {
        return Cell::getHash(context); // O una implementación más específica
    }

    const ProtoTuple* ProtoTupleImplementation::asProtoTuple(ProtoContext* context) const {
        return (const ProtoTuple*)this->implAsObject(context);
    }

    // --- Añadir a core/ProtoTuple.cpp ---

    unsigned long ProtoTupleImplementation::implGetSize(ProtoContext* context) const {
        return this->count;
    }

    const ProtoList* ProtoTupleImplementation::implAsList(ProtoContext* context) const {
        ProtoList* list = const_cast<ProtoList*>(context->newList());
        for (unsigned long i = 0; i < this->count; ++i) {
            list = const_cast<ProtoList*>(list->appendLast(context, this->implGetAt(context, i)));
        }
        return list;
    }
}
