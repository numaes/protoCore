/*
 * ProtoMultiset.cpp
 *
 *  Created on: 2024-05-28
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

namespace proto {

    ProtoMultisetImplementation::ProtoMultisetImplementation(
        ProtoContext* context,
        const ProtoSparseList* list,
        unsigned long size
    ) : Cell(context), list(list), size(size)
    {
    }

    void ProtoMultisetImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (list) method(context, self, reinterpret_cast<const ProtoObject*>(list)->asCell(context));
    }

    const ProtoObject* ProtoMultisetImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.multisetImplementation = this;
        p.op.pointer_tag = POINTER_TAG_MULTISET;
        return p.oid;
    }

    const ProtoMultiset* ProtoMultisetImplementation::asProtoMultiset(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.multisetImplementation = this;
        p.op.pointer_tag = POINTER_TAG_MULTISET;
        return p.multiset;
    }

    ProtoMultisetIteratorImplementation::ProtoMultisetIteratorImplementation(
        ProtoContext* context,
        const ProtoSparseListIteratorImplementation* it
    ) : Cell(context), iterator(it)
    {
    }

    int ProtoMultisetIteratorImplementation::implHasNext(ProtoContext* context) const {
        return iterator->implHasNext();
    }

    const ProtoObject* ProtoMultisetIteratorImplementation::implNext(ProtoContext* context) {
        return iterator->implNextValue();
    }

    const ProtoObject* ProtoMultisetIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.multisetIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_MULTISET_ITERATOR;
        return p.oid;
    }

    void ProtoMultisetIteratorImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (iterator) method(context, self, iterator);
    }
}
