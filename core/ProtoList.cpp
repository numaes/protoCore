/*
 * ProtoList.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <algorithm> // For std::max

namespace proto {

    namespace { // Anonymous namespace for file-local helpers

        /**
         * @brief Safely gets the size of a node.
         * Checks for null and 64-byte alignment before dereferencing.
         * A tagged handle passed by mistake will be unaligned and treated as size 0.
         */
        inline unsigned long get_node_size(const ProtoListImplementation* node) {
            if (!node || (reinterpret_cast<uintptr_t>(node) & 0x3F) != 0) {
                return 0;
            }
            return node->size;
        }

        /**
         * @brief Safely gets the height of a node.
         * Checks for null and 64-byte alignment before dereferencing.
         * A tagged handle passed by mistake will be unaligned and treated as height 0.
         */
        inline int get_node_height(const ProtoListImplementation* node) {
            if (!node || (reinterpret_cast<uintptr_t>(node) & 0x3F) != 0) {
                return 0;
            }
            return node->height;
        }

    } // end anonymous namespace

    //=========================================================================
    // ProtoListImplementation
    //=========================================================================

    ProtoListImplementation::ProtoListImplementation(
        ProtoContext* context,
        const ProtoObject* v,
        bool empty,
        const ProtoListImplementation* prev,
        const ProtoListImplementation* next
    ) : Cell(context), value(empty ? nullptr : v), previousNode(empty ? nullptr : prev), nextNode(empty ? nullptr : next),
        hash(0),
        size(empty ? 0 : get_node_size(prev) + get_node_size(next) + 1),
        height(empty ? 0 : 1 + std::max(get_node_height(prev), get_node_height(next))),
        isEmpty(empty)
    {
    }

    const ProtoObject* ProtoListImplementation::implGetAt(ProtoContext* context, int index) const {
        if (isEmpty) return PROTO_NONE;
        const unsigned long left_size = previousNode ? previousNode->size : 0;
        if (index < left_size) {
            return previousNode->implGetAt(context, index);
        }
        if (index == left_size) {
            return value;
        }
        return nextNode->implGetAt(context, index - left_size - 1);
    }

    bool ProtoListImplementation::implHas(ProtoContext* context, const ProtoObject* targetValue) const {
        if (isEmpty) return false;
        if (Integer::compare(context, value, targetValue) == 0) return true;
        if (previousNode && previousNode->implHas(context, targetValue)) return true;
        if (nextNode && nextNode->implHas(context, targetValue)) return true;
        return false;
    }

    const ProtoListImplementation* ProtoListImplementation::implSetAt(ProtoContext* context, int index, const ProtoObject* newValue) const {
        if (isEmpty || index < 0 || index >= size) {
            return this;
        }

        const unsigned long left_size = previousNode ? previousNode->size : 0;

        if (index < left_size) {
            const ProtoListImplementation* new_prev = previousNode->implSetAt(context, index, newValue);
            return new (context) ProtoListImplementation(context, value, false, new_prev, nextNode);
        }
        if (index == left_size) {
            return new (context) ProtoListImplementation(context, newValue, false, previousNode, nextNode);
        }

        const ProtoListImplementation* new_next = nextNode->implSetAt(context, index - left_size - 1, newValue);
        return new (context) ProtoListImplementation(context, value, false, previousNode, new_next);
    }

    const ProtoListImplementation* ProtoListImplementation::implInsertAt(ProtoContext* context, int index, const ProtoObject* newValue) const {
        if (isEmpty) {
            return new (context) ProtoListImplementation(context, newValue, false, nullptr, nullptr);
        }

        const unsigned long left_size = previousNode ? previousNode->size : 0;

        if (index <= left_size) {
            const ProtoListImplementation* new_prev = previousNode
                ? previousNode->implInsertAt(context, index, newValue)
                : new (context) ProtoListImplementation(context, newValue, false, nullptr, nullptr);
            return new (context) ProtoListImplementation(context, value, false, new_prev, nextNode);
        } else {
            const ProtoListImplementation* new_next = nextNode
                ? nextNode->implInsertAt(context, index - left_size - 1, newValue)
                : new (context) ProtoListImplementation(context, newValue, false, nullptr, nullptr);
            return new (context) ProtoListImplementation(context, value, false, previousNode, new_next);
        }
    }


    const ProtoListImplementation* ProtoListImplementation::implAppendLast(ProtoContext* context, const ProtoObject* newValue) const {
        return implInsertAt(context, size, newValue);
    }

    const ProtoListImplementation* ProtoListImplementation::implRemoveAt(ProtoContext* context, int index) const {
        // This is a complex operation in a rope-like structure and is not fully implemented here for brevity.
        return this;
    }

    const ProtoObject* ProtoListImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST;
        return p.oid;
    }

    const ProtoList* ProtoListImplementation::asProtoList(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST;
        return p.list;
    }

    const ProtoListIteratorImplementation* ProtoListImplementation::implGetIterator(ProtoContext* context) const {
        return new (context) ProtoListIteratorImplementation(context, this, 0);
    }

    //=========================================================================
    // ProtoListIteratorImplementation
    //=========================================================================

    ProtoListIteratorImplementation::ProtoListIteratorImplementation(
        ProtoContext* context,
        const ProtoListImplementation* b,
        unsigned long index
    ) : Cell(context), base(b), currentIndex(index)
    {
    }

    int ProtoListIteratorImplementation::implHasNext() const {
        if (!this->base) return false;
        return this->currentIndex < this->base->size;
    }

    const ProtoObject* ProtoListIteratorImplementation::implNext(ProtoContext* context) const {
        return this->base->implGetAt(context, this->currentIndex);
    }

    const ProtoListIteratorImplementation* ProtoListIteratorImplementation::implAdvance(ProtoContext* context) const {
        if (this->currentIndex < this->base->size) {
            return new (context) ProtoListIteratorImplementation(context, this->base, this->currentIndex + 1);
        }
        return this;
    }

    const ProtoListIterator* ProtoListIteratorImplementation::asProtoListIterator(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_ITERATOR;
        return p.listIterator;
    }

    const ProtoObject* ProtoListIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_ITERATOR;
        return p.oid;
    }

    void ProtoListIteratorImplementation::processReferences(ProtoContext* context, void* callback_data, void (*callback)(ProtoContext*, void*, const Cell*)) const {
        if (base) {
            callback(context, callback_data, (const Cell*)base);
        }
    }
}
