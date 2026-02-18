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

    // ProtoMultiset / ProtoMultisetIterator external API trampolines
    const ProtoMultiset* ProtoMultiset::add(ProtoContext* context, const ProtoObject* value) const {
        const auto* impl = toImpl<const ProtoMultisetImplementation>(this);
        const auto* current_list = impl->list;
        unsigned long hash = value->getHash(context);
        const ProtoObject* existing = current_list->getAt(context, hash);
        long long count = (existing && existing != PROTO_NONE) ? existing->asLong(context) : 0;

        const auto* new_list = current_list->setAt(context, hash, context->fromInteger(count + 1));
        return (new (context) ProtoMultisetImplementation(context, new_list, impl->size + 1))->asProtoMultiset(context);
    }

    const ProtoObject* ProtoMultiset::count(ProtoContext* context, const ProtoObject* value) const {
        const auto* current_list = toImpl<const ProtoMultisetImplementation>(this)->list;
        const ProtoObject* existing = current_list->getAt(context, value->getHash(context));
        return (existing && existing != PROTO_NONE) ? existing : context->fromInteger(0);
    }

    const ProtoMultiset* ProtoMultiset::remove(ProtoContext* context, const ProtoObject* value) const {
        const auto* impl = toImpl<const ProtoMultisetImplementation>(this);
        const auto* current_list = impl->list;
        unsigned long hash = value->getHash(context);
        const ProtoObject* existing = current_list->getAt(context, hash);
        if (!existing || existing == PROTO_NONE) return this;

        long long count = existing->asLong(context);
        const ProtoSparseList* new_list;
        if (count > 1) {
             new_list = current_list->setAt(context, hash, context->fromInteger(count - 1));
        } else {
             new_list = current_list->removeAt(context, hash);
        }
        return (new (context) ProtoMultisetImplementation(context, new_list, impl->size - 1))->asProtoMultiset(context);
    }
    unsigned long ProtoMultiset::getSize(ProtoContext* context) const { return toImpl<const ProtoMultisetImplementation>(this)->size; }
    const ProtoObject* ProtoMultiset::asObject(ProtoContext* context) const { return toImpl<const ProtoMultisetImplementation>(this)->implAsObject(context); }
    const ProtoMultisetIterator* ProtoMultiset::getIterator(ProtoContext* context) const {
        const auto* list_iterator = toImpl<const ProtoMultisetImplementation>(this)->list->getIterator(context);
        if (!list_iterator) return nullptr;
        return (new (context) ProtoMultisetIteratorImplementation(context, toImpl<const ProtoSparseListIteratorImplementation>(list_iterator)))->asMultisetIterator(context);
    }

    int ProtoMultisetIterator::hasNext(ProtoContext* context) const { if (!this) return 0; return toImpl<const ProtoMultisetIteratorImplementation>(this)->implHasNext(context); }
    const ProtoObject* ProtoMultisetIterator::next(ProtoContext* context) const { if (!this) return nullptr; return const_cast<ProtoMultisetIteratorImplementation*>(toImpl<const ProtoMultisetIteratorImplementation>(this))->implNext(context); }
    const ProtoMultisetIterator* ProtoMultisetIterator::advance(ProtoContext* context) const {
        if (!this) return nullptr;
        const auto* impl = toImpl<const ProtoMultisetIteratorImplementation>(this);
        const ProtoSparseListIteratorImplementation* advanced = const_cast<ProtoSparseListIteratorImplementation*>(impl->iterator)->implAdvance(context);
        return (new (context) ProtoMultisetIteratorImplementation(context, advanced))->asMultisetIterator(context);
    }
    const ProtoObject* ProtoMultisetIterator::asObject(ProtoContext* context) const { if (!this) return nullptr; return toImpl<const ProtoMultisetIteratorImplementation>(this)->implAsObject(context); }
}
