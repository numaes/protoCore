/*
 * ProtoList.cpp
 *
 *  Created on: Aug 6, 2017
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <algorithm> // For std::max

namespace proto
{
    //=========================================================================
    // ProtoListIteratorImplementation
    //=========================================================================
    ProtoListIteratorImplementation::ProtoListIteratorImplementation(ProtoContext* context, const ProtoListImplementation* b, unsigned long index)
        : Cell(context), base(b), currentIndex(index) {}

    int ProtoListIteratorImplementation::implHasNext() const {
        return base && !base->isEmpty && currentIndex < base->size;
    }

    const ProtoObject* ProtoListIteratorImplementation::implNext(ProtoContext* context) const {
        if (!implHasNext()) return PROTO_NONE;
        return base->implGetAt(context, currentIndex);
    }

    const ProtoListIteratorImplementation* ProtoListIteratorImplementation::implAdvance(ProtoContext* context) const {
        return new(context) ProtoListIteratorImplementation(context, base, currentIndex + 1);
    }

    const ProtoObject* ProtoListIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.voidPointer = const_cast<ProtoListIteratorImplementation*>(this);
        p.op.pointer_tag = POINTER_TAG_LIST_ITERATOR;
        return p.oid;
    }

    const ProtoListIterator* ProtoListIteratorImplementation::asProtoListIterator(ProtoContext* context) const {
        return reinterpret_cast<const ProtoListIterator*>(implAsObject(context));
    }

    void ProtoListIteratorImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (base) method(context, self, base);
    }

    //=========================================================================
    // ProtoListImplementation
    //=========================================================================
    ProtoListImplementation::ProtoListImplementation(ProtoContext* context, const ProtoObject* v, bool empty, const ProtoListImplementation* prev, const ProtoListImplementation* next)
        : Cell(context), value(v), previousNode(prev), nextNode(next),
          hash(empty ? 0 : (v ? v->getHash(context) : 0) ^ (prev ? prev->hash : 0) ^ (next ? next->hash : 0)),
          size(empty ? 0 : 1 + (prev ? prev->size : 0) + (next ? next->size : 0)),
          height(empty ? 0 : 1 + std::max(prev ? prev->height : 0, next ? next->height : 0)),
          isEmpty(empty) {}

    namespace {
        int getHeight(const ProtoListImplementation* node) { return node ? node->height : 0; }

        const ProtoListImplementation* rotateRight(ProtoContext* context, const ProtoListImplementation* y) {
            const ProtoListImplementation* x = y->previousNode;
            const ProtoListImplementation* T2 = x->nextNode;
            return new (context) ProtoListImplementation(context, x->value, false, x->previousNode, new (context) ProtoListImplementation(context, y->value, false, T2, y->nextNode));
        }

        const ProtoListImplementation* rotateLeft(ProtoContext* context, const ProtoListImplementation* x) {
            const ProtoListImplementation* y = x->nextNode;
            const ProtoListImplementation* T2 = y->previousNode;
            return new (context) ProtoListImplementation(context, y->value, false, new (context) ProtoListImplementation(context, x->value, false, x->previousNode, T2), y->nextNode);
        }

        const ProtoListImplementation* rebalance(ProtoContext* context, const ProtoListImplementation* node) {
            int balance = getHeight(node->previousNode) - getHeight(node->nextNode);
            if (balance > 1) {
                if (getHeight(node->previousNode->nextNode) > getHeight(node->previousNode->previousNode)) {
                    return rotateRight(context, new(context) ProtoListImplementation(context, node->value, false, rotateLeft(context, node->previousNode), node->nextNode));
                }
                return rotateRight(context, node);
            }
            if (balance < -1) {
                if (getHeight(node->nextNode->previousNode) > getHeight(node->nextNode->nextNode)) {
                    return rotateLeft(context, new(context) ProtoListImplementation(context, node->value, false, node->previousNode, rotateRight(context, node->nextNode)));
                }
                return rotateLeft(context, node);
            }
            return node;
        }
    }

    const ProtoObject* ProtoListImplementation::implGetAt(ProtoContext* context, int index) const {
        if (isEmpty) return PROTO_NONE;
        if (index < 0) index += size;
        if (index < 0 || (unsigned)index >= size) return PROTO_NONE;

        const ProtoListImplementation* node = this;
        while (node) {
            unsigned long left_size = node->previousNode ? node->previousNode->size : 0;
            if ((unsigned)index < left_size) {
                node = node->previousNode;
            } else if ((unsigned)index > left_size) {
                index -= (left_size + 1);
                node = node->nextNode;
            } else {
                return node->value;
            }
        }
        return PROTO_NONE;
    }

    bool ProtoListImplementation::implHas(ProtoContext* context, const ProtoObject* targetValue) const {
        if (isEmpty) return false;
        if (value->compare(context, targetValue) == 0) return true;
        if (previousNode && previousNode->implHas(context, targetValue)) return true;
        if (nextNode && nextNode->implHas(context, targetValue)) return true;
        return false;
    }

    const ProtoListImplementation* ProtoListImplementation::implSetAt(ProtoContext* context, int index, const ProtoObject* newValue) const {
        if (isEmpty || index < 0 || (unsigned)index >= size) return this;
        unsigned long left_size = previousNode ? previousNode->size : 0;
        if ((unsigned)index < left_size) {
            return new (context) ProtoListImplementation(context, value, false, previousNode->implSetAt(context, index, newValue), nextNode);
        } else if ((unsigned)index > left_size) {
            return new (context) ProtoListImplementation(context, value, false, previousNode, nextNode->implSetAt(context, index - (left_size + 1), newValue));
        } else {
            return new (context) ProtoListImplementation(context, newValue, false, previousNode, nextNode);
        }
    }

    const ProtoListImplementation* ProtoListImplementation::implInsertAt(ProtoContext* context, int index, const ProtoObject* newValue) const {
        if (isEmpty) return new (context) ProtoListImplementation(context, newValue, false, nullptr, nullptr);
        unsigned long left_size = previousNode ? previousNode->size : 0;
        const ProtoListImplementation* newNode;
        if ((unsigned)index <= left_size) {
            newNode = new (context) ProtoListImplementation(context, value, false, previousNode ? previousNode->implInsertAt(context, index, newValue) : new (context) ProtoListImplementation(context, newValue, false, nullptr, nullptr), nextNode);
        } else {
            newNode = new (context) ProtoListImplementation(context, value, false, previousNode, nextNode ? nextNode->implInsertAt(context, index - (left_size + 1), newValue) : new (context) ProtoListImplementation(context, newValue, false, nullptr, nullptr));
        }
        return rebalance(context, newNode);
    }

    const ProtoListImplementation* ProtoListImplementation::implAppendLast(ProtoContext* context, const ProtoObject* newValue) const {
        return implInsertAt(context, size, newValue);
    }

    const ProtoListImplementation* findMin(const ProtoListImplementation* node) {
        while (node && node->previousNode && !node->previousNode->isEmpty) node = node->previousNode;
        return node;
    }

    const ProtoListImplementation* ProtoListImplementation::implRemoveAt(ProtoContext* context, int index) const {
        if (isEmpty || index < 0 || (unsigned)index >= size) return this;
        unsigned long left_size = previousNode ? previousNode->size : 0;
        const ProtoListImplementation* newNode;
        if ((unsigned)index < left_size) {
            newNode = new (context) ProtoListImplementation(context, value, false, previousNode->implRemoveAt(context, index), nextNode);
        } else if ((unsigned)index > left_size) {
            newNode = new (context) ProtoListImplementation(context, value, false, previousNode, nextNode->implRemoveAt(context, index - (left_size + 1)));
        } else {
            if (!previousNode || previousNode->isEmpty) return nextNode;
            if (!nextNode || nextNode->isEmpty) return previousNode;
            const ProtoListImplementation* successor = findMin(nextNode);
            newNode = new (context) ProtoListImplementation(context, successor->value, false, previousNode, nextNode->implRemoveAt(context, 0));
        }
        return rebalance(context, newNode);
    }

    const ProtoObject* ProtoListImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.voidPointer = const_cast<ProtoListImplementation*>(this);
        p.op.pointer_tag = POINTER_TAG_LIST;
        return p.oid;
    }

    const ProtoList* ProtoListImplementation::asProtoList(ProtoContext* context) const {
        return reinterpret_cast<const ProtoList*>(implAsObject(context));
    }

    const ProtoListIteratorImplementation* ProtoListImplementation::implGetIterator(ProtoContext* context) const {
        return new(context) ProtoListIteratorImplementation(context, this, 0);
    }
}
