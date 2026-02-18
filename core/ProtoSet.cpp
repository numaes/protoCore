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

    // ProtoSet / ProtoSetIterator external API trampolines
    const ProtoSet* ProtoSet::add(ProtoContext* context, const ProtoObject* value) const {
        const auto* current_list = toImpl<const ProtoSetImplementation>(this)->list;
        const auto* new_list = current_list->setAt(context, value->getHash(context), value);
        return (new (context) ProtoSetImplementation(context, new_list, new_list->getSize(context)))->asProtoSet(context);
    }

    const ProtoObject* ProtoSet::has(ProtoContext* context, const ProtoObject* value) const {
        return toImpl<const ProtoSetImplementation>(this)->list->has(context, value->getHash(context)) ? PROTO_TRUE : PROTO_FALSE;
    }

    const ProtoSet* ProtoSet::remove(ProtoContext* context, const ProtoObject* value) const {
        const auto* current_list = toImpl<const ProtoSetImplementation>(this)->list;
        const auto* new_list = toImpl<const ProtoSparseListImplementation>(current_list)->implRemoveAt(context, value->getHash(context));
        return (new (context) ProtoSetImplementation(context, new_list->asSparseList(context), (unsigned long)new_list->size))->asProtoSet(context);
    }

    unsigned long ProtoSet::getSize(ProtoContext* context) const { return toImpl<const ProtoSetImplementation>(this)->size; }
    const ProtoObject* ProtoSet::asObject(ProtoContext* context) const { return toImpl<const ProtoSetImplementation>(this)->implAsObject(context); }
    const ProtoSetIterator* ProtoSet::getIterator(ProtoContext* context) const {
        const auto* list_iterator = toImpl<const ProtoSetImplementation>(this)->list->getIterator(context);
        if (!list_iterator) return nullptr;
        return (new (context) ProtoSetIteratorImplementation(context, toImpl<const ProtoSparseListIteratorImplementation>(list_iterator)))->asSetIterator(context);
    }

    int ProtoSetIterator::hasNext(ProtoContext* context) const { if (!this) return 0; return toImpl<const ProtoSetIteratorImplementation>(this)->implHasNext(context); }
    const ProtoObject* ProtoSetIterator::next(ProtoContext* context) const { if (!this) return nullptr; return const_cast<ProtoSetIteratorImplementation*>(toImpl<const ProtoSetIteratorImplementation>(this))->implNext(context); }
    const ProtoSetIterator* ProtoSetIterator::advance(ProtoContext* context) const {
        if (!this) return nullptr;
        const auto* impl = toImpl<const ProtoSetIteratorImplementation>(this);
        const ProtoSparseListIteratorImplementation* advanced = const_cast<ProtoSparseListIteratorImplementation*>(impl->iterator)->implAdvance(context);
        return (new (context) ProtoSetIteratorImplementation(context, advanced))->asSetIterator(context);
    }
    const ProtoObject* ProtoSetIterator::asObject(ProtoContext* context) const { if (!this) return nullptr; return toImpl<const ProtoSetIteratorImplementation>(this)->implAsObject(context); }
}
