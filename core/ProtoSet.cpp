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
        return iterator->implHasNext();
    }

    const ProtoObject* ProtoSetIteratorImplementation::implNext(ProtoContext* context) {
        return iterator->implNextValue();
    }

    const ProtoObject* ProtoSetIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.setIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_SET_ITERATOR;
        return p.oid;
    }
}
