/*
 * ProtoTuple.cpp
 *
 *  Created on: 2017-05-01
 *      Author: gamarino
 *
 *  This file implements the immutable tuple, its iterator, and the dictionary
 *  used for tuple interning.
 */

#include "../headers/proto_internal.h"
#include <algorithm>
#include <vector>

namespace proto
{
    //=========================================================================
    // TupleDictionary
    //=========================================================================

    /**
     * @class TupleDictionary
     * @brief An AVL tree used to "intern" tuples.
     *
     * To save memory, Proto ensures that any two tuples with identical content
     * point to the exact same object in memory. This dictionary stores all created
     * tuples. When a new tuple is requested, this dictionary is searched first.
     * If an identical tuple already exists, it is returned; otherwise, the new
     * tuple is created and added to the dictionary.
     */

    void TupleDictionary::finalize(ProtoContext* context) const {
        // No specific finalization needed
    }

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

    void TupleDictionary::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const {
        if (key) method(context, self, key);
        if (previous) method(context, self, previous);
        if (next) method(context, self, next);
    }

    //=========================================================================
    // ProtoTupleIteratorImplementation
    //=========================================================================

    ProtoTupleIteratorImplementation::ProtoTupleIteratorImplementation(
        ProtoContext* context,
        const ProtoTupleImplementation* base,
        int currentIndex
    ) : Cell(context), base(base), currentIndex(currentIndex) {}

    ProtoTupleIteratorImplementation::~ProtoTupleIteratorImplementation() = default;

    int ProtoTupleIteratorImplementation::implHasNext(ProtoContext* context) const {
        return this->currentIndex < (int)this->base->count;
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
        if (base) method(context, self, base);
    }

    unsigned long ProtoTupleIteratorImplementation::getHash(ProtoContext* context) const {
        return Cell::getHash(context);
    }

    //=========================================================================
    // ProtoTupleImplementation
    //=========================================================================

    /**
     * @class ProtoTupleImplementation
     * @brief An immutable, fixed-size sequence implemented as a rope (a tree of data blocks).
     *
     * This structure is highly optimized for memory usage and performance.
     * - If `height` is 0, it's a leaf node, and `pointers.data` holds direct
     *   pointers to the objects.
     * - If `height` > 0, it's an internal node, and `pointers.indirect` holds
     *   pointers to other `ProtoTupleImplementation` nodes.
     * This allows for efficient slicing and concatenation by manipulating tree
     * nodes instead of copying large blocks of memory.
     */

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

    namespace {
        // Función auxiliar recursiva para construir el árbol de la tupla (rope).
        const ProtoTupleImplementation* fromListRecursive(ProtoContext* context, const ProtoList* list, unsigned long start, unsigned long end) {
            const unsigned long count = end - start;
            if (count == 0) {
                return new(context) ProtoTupleImplementation(context, 0, 0, nullptr, nullptr);
            }

            if (count <= TUPLE_SIZE) {
                // Nodo hoja: contiene punteros directos a los objetos.
                const ProtoObject* data[TUPLE_SIZE] = {nullptr};
                for (unsigned long i = 0; i < count; ++i) {
                    data[i] = list->getAt(context, start + i);
                }
                return new(context) ProtoTupleImplementation(context, count, 0, data, nullptr);
            }

            // Nodo interno: contiene punteros a otros nodos de tupla.
            const ProtoTupleImplementation* indirect[TUPLE_SIZE] = {nullptr};
            unsigned long remaining = count;
            unsigned long current_pos = start;
            unsigned long child_height = 0;

            // This is a simplified balancing, a real rope would have a more complex algorithm
            unsigned long span = (count + TUPLE_SIZE - 1) / TUPLE_SIZE;

            for (int i = 0; i < TUPLE_SIZE && remaining > 0; ++i) {
                unsigned long chunk_size = std::min(span, remaining);
                indirect[i] = fromListRecursive(context, list, current_pos, current_pos + chunk_size);
                if(indirect[i]) child_height = std::max(child_height, indirect[i]->height);
                current_pos += chunk_size;
                remaining -= chunk_size;
            }
            return new(context) ProtoTupleImplementation(context, count, child_height + 1, nullptr, indirect);
        }
    }

    const ProtoTupleImplementation* ProtoTupleImplementation::tupleFromList(ProtoContext* context, const ProtoList* list) {
        if (!list || list->getSize(context) == 0) {
            return toImpl<const ProtoTupleImplementation>(context->newTuple());
        }
        return fromListRecursive(context, list, 0, list->getSize(context));
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

    const ProtoTupleImplementation* ProtoTupleImplementation::implAppendLast(
        ProtoContext* context,
        const ProtoTuple* otherTuple
    ) const {
        const ProtoTupleImplementation* other = toImpl<const ProtoTupleImplementation>(otherTuple);
        if (this->count == 0) return other;
        if (other->count == 0) return this;

        const unsigned long new_count = this->count + other->count;
        // Create a new internal node
        const ProtoTupleImplementation* indirect[TUPLE_SIZE] = {this, other};
        unsigned long new_height = std::max(this->height, other->height) + 1;
        return new(context) ProtoTupleImplementation(context, new_count, new_height, nullptr, indirect);
    }

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

    void ProtoTupleImplementation::finalize(ProtoContext* context) const {}

    void ProtoTupleImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (this->height == 0) {
            for (unsigned long i = 0; i < this->count; ++i) {
                if (this->pointers.data[i] && this->pointers.data[i]->isCell(context)) {
                    method(context, self, this->pointers.data[i]->asCell(context));
                }
            }
        } else {
            for (unsigned long i = 0; i < TUPLE_SIZE; ++i) {
                if (this->pointers.indirect[i]) {
                    method(context, self, this->pointers.indirect[i]);
                }
            }
        }
    }

    unsigned long ProtoTupleImplementation::getHash(ProtoContext* context) const {
        unsigned long hash = 0;
        for (unsigned long i = 0; i < this->count; ++i) {
            const ProtoObject* obj = this->implGetAt(context, i);
            if (obj) {
                hash ^= obj->getHash(context);
            }
        }
        return hash;
    }

    const ProtoObject* ProtoTupleImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p;
        p.tupleImplementation = this;
        p.op.pointer_tag = POINTER_TAG_TUPLE;
        return p.oid.oid;
    }

    const ProtoTuple* ProtoTupleImplementation::asProtoTuple(ProtoContext* context) const {
        return (const ProtoTuple*)this->implAsObject(context);
    }

    // --- Public API Trampolines ---

    unsigned long ProtoTuple::getSize(ProtoContext* context) const {
        return toImpl<const ProtoTupleImplementation>(this)->implGetSize(context);
    }

    const ProtoObject* ProtoTuple::getAt(ProtoContext* context, int index) const {
        return toImpl<const ProtoTupleImplementation>(this)->implGetAt(context, index);
    }

    const ProtoObject* ProtoTuple::getSlice(ProtoContext* context, int from, int to) const {
        const ProtoList* list = toImpl<const ProtoTupleImplementation>(this)->implAsList(context);
        const ProtoList* slice = list->getSlice(context, from, to);
        return context->newTupleFromList(slice)->asObject(context);
    }

    const ProtoObject* ProtoTuple::asObject(ProtoContext* context) const {
        return toImpl<const ProtoTupleImplementation>(this)->implAsObject(context);
    }
}
