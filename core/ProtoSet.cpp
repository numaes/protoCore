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
        const ProtoSparseList* list,
        unsigned long size
    ) : Cell(context), list(list), size(size)
    {
    }

    void ProtoSetImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (list) method(context, self, reinterpret_cast<const ProtoObject*>(list)->asCell(context));
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
        const ProtoSparseListIteratorImplementation* it
    ) : Cell(context), iterator(it)
    {
    }

    int ProtoSetIteratorImplementation::implHasNext(ProtoContext* context) const {
        return iterator ? iterator->implHasNext() : false;
    }

    const ProtoObject* ProtoSetIteratorImplementation::implNext(ProtoContext* context) {
        return iterator ? iterator->implNextValue() : PROTO_NONE;
    }

    const ProtoObject* ProtoSetIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.setIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_SET_ITERATOR;
        return p.oid;
    }

    void ProtoSetIteratorImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (iterator) method(context, self, iterator);
    }
}
