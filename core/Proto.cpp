/*
 * Proto.cpp
 *
 *  Created on: 2020-5-1
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <random>

namespace proto
{
    unsigned long generate_mutable_ref() {
        return static_cast<unsigned long>(std::rand());
    }

    const ProtoObject* ProtoObject::getPrototype(ProtoContext* context) const
    {
        // TODO: completar todos los embeded, todos los pointer_tags
        ProtoObjectPointer pa{};
        pa.oid = this;

        switch (pa.op.pointer_tag)
        {
        case POINTER_TAG_EMBEDDED_VALUE:
            switch (pa.op.embedded_type)
            {
            case EMBEDDED_TYPE_BOOLEAN: return context->space->booleanPrototype;
            case EMBEDDED_TYPE_UNICODE_CHAR: return context->space->unicodeCharPrototype;
            default: return nullptr;
            }
        case POINTER_TAG_LIST: return context->space->listPrototype;
        case POINTER_TAG_SPARSE_LIST: return context->space->sparseListPrototype;
        case POINTER_TAG_TUPLE: return context->space->tuplePrototype;
        case POINTER_TAG_STRING: return context->space->stringPrototype;
        case POINTER_TAG_SET: return context->space->setPrototype;
        case POINTER_TAG_MULTISET: return context->space->multisetPrototype;
        default: return nullptr;
        }
    }

    const ProtoObject* ProtoObject::clone(ProtoContext* context, bool isMutable) const
    {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return PROTO_NONE;
        auto* oc = toImpl<const ProtoObjectCell>(this);
        return (new(context) ProtoObjectCell(context, oc->parent, oc->attributes, 0))->asObject(context);
    }

    const ProtoObject* ProtoObject::newChild(ProtoContext* context, bool isMutable) const
    {
        if (!this->isCell(context)) return PROTO_NONE;
        auto* newObject = new(context) ProtoObjectCell(context, new(context) ParentLinkImplementation(context, toImpl<const ProtoObjectCell>(this)->parent, this), toImpl<const ProtoSparseListImplementation>(context->newSparseList()), isMutable ? generate_mutable_ref() : 0);
        return newObject->asObject(context);
    }

    const ProtoObject* ProtoObject::call(ProtoContext* context, const ParentLink* nextParent, const ProtoString* method, const ProtoObject* self, const ProtoList* positionalParameters, const ProtoSparseList* keywordParametersDict) const
    {
        const auto* result = this->getAttribute(context, const_cast<ProtoString*>(method));
        if (result && result->isMethod(context)) {
            return result->asMethod(context)(context, self, nextParent, positionalParameters, keywordParametersDict);
        }
        if (context->space->nonMethodCallback) {
            return (*context->space->nonMethodCallback)(context, nextParent, method, self, positionalParameters, keywordParametersDict);
        }
        return PROTO_NONE;
    }

    const ProtoObject* ProtoObject::isInstanceOf(ProtoContext* context, const ProtoObject* prototype) const
    {
        const ProtoObject* current = this;
        while (current) {
            if (current == prototype) return PROTO_TRUE;
            ProtoObjectPointer pa{};
            pa.oid = current;
            if (pa.op.pointer_tag != POINTER_TAG_OBJECT) break;
            auto oc = toImpl<const ProtoObjectCell>(current);
            if (oc->parent) current = oc->parent->getObject(context);
            else break;
        }
        return PROTO_FALSE;
    }

    const ProtoObject* ProtoObject::getAttribute(ProtoContext* context, const ProtoString* name) const
    {
        const ProtoObject* currentObject = this;
        const unsigned long attr_hash = name->getHash(context);
        while (currentObject) {
            auto oc = toImpl<const ProtoObjectCell>(currentObject);
            if (oc->attributes->implHas(context, attr_hash)) {
                return oc->attributes->implGetAt(context, attr_hash);
            }
            if (oc->parent) currentObject = oc->parent->getObject(context);
            else currentObject = nullptr;
        }
        if (context->space->attributeNotFoundGetCallback) {
            return (*context->space->attributeNotFoundGetCallback)(context, this, name);
        }
        return PROTO_NONE;
    }

    const ProtoObject* ProtoObject::setAttribute(ProtoContext* context, const ProtoString* name, const ProtoObject* value) const
    {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return this;
        auto* oc = toImpl<ProtoObjectCell>(this);
        const auto* newAttributes = oc->attributes->implSetAt(context, name->getHash(context), value);
        return (new(context) ProtoObjectCell(context, oc->parent, newAttributes, 0))->asObject(context);
    }

    const ProtoList* ProtoObject::getParents(ProtoContext* context) const {
        if (!isCell(context)) return context->newList();
        const auto* oc = toImpl<const ProtoObjectCell>(this);
        const ProtoList* parents = context->newList();
        const ParentLinkImplementation* p = oc->parent;
        while (p) {
            parents = parents->appendLast(context, p->getObject(context));
            p = p->getParent(context);
        }
        return parents;
    }

    const ProtoObject* ProtoObject::addParent(ProtoContext* context, const ProtoObject* newParent) const {
        if (!isCell(context) || !newParent->isCell(context)) return this;
        const auto* oc = toImpl<const ProtoObjectCell>(this);
        return oc->addParent(context, newParent)->asObject(context);
    }

    int ProtoObject::isCell(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        return pa.op.pointer_tag != POINTER_TAG_EMBEDDED_VALUE;
    }

    const Cell* ProtoObject::asCell(ProtoContext* context) const {
        if (isCell(context)) {
            ProtoObjectPointer pa{};
            pa.oid = this;
            return (const Cell*)pa.voidPointer;
        }
        return nullptr;
    }

    const ProtoString* ProtoObject::asString(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_STRING ? reinterpret_cast<const ProtoString*>(this) : nullptr;
    }

    ProtoMethod ProtoObject::asMethod(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_METHOD ? toImpl<const ProtoMethodCell>(this)->method : nullptr;
    }

    bool ProtoObject::isNone(ProtoContext* context) const { return this == PROTO_NONE; }
    bool ProtoObject::isBoolean(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_BOOLEAN; }
    bool ProtoObject::isInteger(ProtoContext* context) const { return proto::isInteger(this); }
    bool ProtoObject::isString(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_STRING; }
    bool ProtoObject::isMethod(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_METHOD; }
    bool ProtoObject::isTuple(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_TUPLE; }
    bool ProtoObject::isSet(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_SET; }
    bool ProtoObject::isMultiset(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_MULTISET; }

    unsigned long ProtoObject::getHash(ProtoContext* context) const {
        if (isCell(context)) return asCell(context)->getHash(context);
        return reinterpret_cast<uintptr_t>(this);
    }

    const ProtoTuple* ProtoObject::asTuple(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_TUPLE ? reinterpret_cast<const ProtoTuple*>(this) : nullptr; }
    const ProtoSet* ProtoObject::asSet(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_SET ? reinterpret_cast<const ProtoSet*>(this) : nullptr; }
    const ProtoSetIterator* ProtoObject::asSetIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_SET_ITERATOR ? reinterpret_cast<const ProtoSetIterator*>(this) : nullptr; }
    const ProtoMultiset* ProtoObject::asMultiset(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_MULTISET ? reinterpret_cast<const ProtoMultiset*>(this) : nullptr; }
    const ProtoMultisetIterator* ProtoObject::asMultisetIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_MULTISET_ITERATOR ? reinterpret_cast<const ProtoMultisetIterator*>(this) : nullptr; }

    long long ProtoObject::asLong(ProtoContext* context) const { return Integer::asLong(context, this); }
    bool ProtoObject::asBoolean(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.booleanValue.booleanValue; }
    char ProtoObject::asByte(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.byteValue.byteData; }
    double ProtoObject::asDouble(ProtoContext* context) const { return toImpl<const DoubleImplementation>(this)->doubleValue; }
    bool ProtoObject::isDouble(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_DOUBLE; }
    const ProtoObject* ProtoObject::add(ProtoContext* context, const ProtoObject* other) const { return Integer::add(context, this, other); }
    const ProtoObject* ProtoObject::subtract(ProtoContext* context, const ProtoObject* other) const { return Integer::subtract(context, this, other); }
    const ProtoObject* ProtoObject::multiply(ProtoContext* context, const ProtoObject* other) const { return Integer::multiply(context, this, other); }
    const ProtoObject* ProtoObject::divide(ProtoContext* context, const ProtoObject* other) const { return Integer::divide(context, this, other); }
    const ProtoObject* ProtoObject::modulo(ProtoContext* context, const ProtoObject* other) const { return Integer::modulo(context, this, other); }
    int ProtoObject::compare(ProtoContext* context, const ProtoObject* other) const { return Integer::compare(context, this, other); }
    const ProtoObject* ProtoObject::bitwiseNot(ProtoContext* context) const { return Integer::bitwiseNot(context, this); }
    const ProtoObject* ProtoObject::bitwiseAnd(ProtoContext* context, const ProtoObject* other) const { return Integer::bitwiseAnd(context, this, other); }
    const ProtoObject* ProtoObject::bitwiseOr(ProtoContext* context, const ProtoObject* other) const { return Integer::bitwiseOr(context, this, other); }
    const ProtoObject* ProtoObject::shiftLeft(ProtoContext* context, int amount) const { return Integer::shiftLeft(context, this, amount); }
    const ProtoObject* ProtoObject::shiftRight(ProtoContext* context, int amount) const { return Integer::shiftRight(context, this, amount); }
    const ProtoList* ProtoObject::asList(ProtoContext* context) const { return toImpl<const ProtoStringImplementation>(this)->implAsList(context); }
    const ProtoObject* ProtoObject::hasAttribute(ProtoContext* context, const ProtoString* name) const { return context->fromBoolean(getAttribute(context, name) != PROTO_NONE); }
    const ProtoObject* ProtoObject::divmod(ProtoContext* context, const ProtoObject* other) const {
        const ProtoList* result = context->newList();
        result = result->appendLast(context, divide(context, other));
        result = result->appendLast(context, modulo(context, other));
        return result->asObject(context);
    }


    //=========================================================================
    // ProtoList API Implementation
    //=========================================================================
    unsigned long ProtoList::getSize(ProtoContext* context) const { return toImpl<const ProtoListImplementation>(this)->size; }
    const ProtoObject* ProtoList::getAt(ProtoContext* context, int index) const { return toImpl<const ProtoListImplementation>(this)->implGetAt(context, index); }
    bool ProtoList::has(ProtoContext* context, const ProtoObject* value) const { return toImpl<const ProtoListImplementation>(this)->implHas(context, value); }
    const ProtoList* ProtoList::setAt(ProtoContext* context, int index, const ProtoObject* value) const { return toImpl<const ProtoListImplementation>(this)->implSetAt(context, index, value)->asProtoList(context); }
    const ProtoList* ProtoList::insertAt(ProtoContext* context, int index, const ProtoObject* value) const { return toImpl<const ProtoListImplementation>(this)->implInsertAt(context, index, value)->asProtoList(context); }
    const ProtoList* ProtoList::appendLast(ProtoContext* context, const ProtoObject* value) const { return toImpl<const ProtoListImplementation>(this)->implAppendLast(context, value)->asProtoList(context); }
    const ProtoList* ProtoList::removeAt(ProtoContext* context, int index) const { return toImpl<const ProtoListImplementation>(this)->implRemoveAt(context, index)->asProtoList(context); }
    const ProtoList* ProtoList::getSlice(ProtoContext* context, int start, int end) const {
        const ProtoList* list = context->newList();
        for (int i = start; i < end; ++i) {
            list = list->appendLast(context, getAt(context, i));
        }
        return list;
    }
    const ProtoObject* ProtoList::asObject(ProtoContext* context) const { return toImpl<const ProtoListImplementation>(this)->implAsObject(context); }
    const ProtoListIterator* ProtoList::getIterator(ProtoContext* context) const { return toImpl<const ProtoListImplementation>(this)->implGetIterator(context)->asProtoListIterator(context); }

    //=========================================================================
    // ProtoListIterator API Implementation
    //=========================================================================
    int ProtoListIterator::hasNext(ProtoContext* context) const { return toImpl<const ProtoListIteratorImplementation>(this)->implHasNext(); }
    const ProtoObject* ProtoListIterator::next(ProtoContext* context) const { return toImpl<const ProtoListIteratorImplementation>(this)->implNext(context); }
    const ProtoListIterator* ProtoListIterator::advance(ProtoContext* context) const { return toImpl<const ProtoListIteratorImplementation>(this)->implAdvance(context)->asProtoListIterator(context); }


    //=========================================================================
    // ProtoSparseList API Implementation
    //=========================================================================
    bool ProtoSparseList::has(ProtoContext* context, unsigned long offset) const { return toImpl<const ProtoSparseListImplementation>(this)->implHas(context, offset); }
    const ProtoObject* ProtoSparseList::getAt(ProtoContext* context, unsigned long offset) const { return toImpl<const ProtoSparseListImplementation>(this)->implGetAt(context, offset); }
    const ProtoSparseList* ProtoSparseList::setAt(ProtoContext* context, unsigned long offset, const ProtoObject* value) const { return toImpl<const ProtoSparseListImplementation>(this)->implSetAt(context, offset, value)->asSparseList(context); }
    const ProtoSparseList* ProtoSparseList::removeAt(ProtoContext* context, unsigned long offset) const { return toImpl<const ProtoSparseListImplementation>(this)->implRemoveAt(context, offset)->asSparseList(context); }
    unsigned long ProtoSparseList::getSize(ProtoContext* context) const { return toImpl<const ProtoSparseListImplementation>(this)->size; }
    const ProtoObject* ProtoSparseList::asObject(ProtoContext* context) const { return toImpl<const ProtoSparseListImplementation>(this)->implAsObject(context); }
    const ProtoSparseListIterator* ProtoSparseList::getIterator(ProtoContext* context) const { const auto* impl = toImpl<const ProtoSparseListImplementation>(this)->implGetIterator(context); return impl ? reinterpret_cast<const ProtoSparseListIterator*>(impl->implAsObject(context)) : nullptr; }

    //=========================================================================
    // ProtoSparseListIterator API Implementation
    //=========================================================================
    int ProtoSparseListIterator::hasNext(ProtoContext* context) const { return toImpl<const ProtoSparseListIteratorImplementation>(this)->implHasNext(); }
    unsigned long ProtoSparseListIterator::nextKey(ProtoContext* context) const { return toImpl<const ProtoSparseListIteratorImplementation>(this)->implNextKey(); }
    const ProtoObject* ProtoSparseListIterator::nextValue(ProtoContext* context) const { return toImpl<const ProtoSparseListIteratorImplementation>(this)->implNextValue(); }
    const ProtoSparseListIterator* ProtoSparseListIterator::advance(ProtoContext* context) { return reinterpret_cast<const ProtoSparseListIterator*>(toImpl<ProtoSparseListIteratorImplementation>(this)->implAdvance(context)); }


    //=========================================================================
    // ProtoSet API Implementation
    //=========================================================================
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
        return (new (context) ProtoSetIteratorImplementation(context, toImpl<const ProtoSparseListIteratorImplementation>(list_iterator)))->asSetIterator(context);
    }

    //=========================================================================
    // ProtoMultiset API Implementation
    //=========================================================================
    const ProtoMultiset* ProtoMultiset::add(ProtoContext* context, const ProtoObject* value) const {
        const auto* current_list = toImpl<const ProtoMultisetImplementation>(this)->list;
        const auto* new_list = current_list->setAt(context, value->getHash(context), value);
        return (new (context) ProtoMultisetImplementation(context, new_list, new_list->getSize(context)))->asProtoMultiset(context);
    }

    const ProtoObject* ProtoMultiset::count(ProtoContext* context, const ProtoObject* value) const {
        // This is a simplified implementation. A real implementation would need to handle hash collisions.
        return toImpl<const ProtoMultisetImplementation>(this)->list->has(context, value->getHash(context)) ? context->fromInteger(1) : context->fromInteger(0);
    }

    const ProtoMultiset* ProtoMultiset::remove(ProtoContext* context, const ProtoObject* value) const {
        const auto* current_list = toImpl<const ProtoMultisetImplementation>(this)->list;
        const auto* new_list = toImpl<const ProtoSparseListImplementation>(current_list)->implRemoveAt(context, value->getHash(context));
        return (new (context) ProtoMultisetImplementation(context, new_list->asSparseList(context), (unsigned long)new_list->size))->asProtoMultiset(context);
    }
    unsigned long ProtoMultiset::getSize(ProtoContext* context) const { return toImpl<const ProtoMultisetImplementation>(this)->size; }
    const ProtoObject* ProtoMultiset::asObject(ProtoContext* context) const { return toImpl<const ProtoMultisetImplementation>(this)->implAsObject(context); }
    const ProtoMultisetIterator* ProtoMultiset::getIterator(ProtoContext* context) const {
        const auto* list_iterator = toImpl<const ProtoMultisetImplementation>(this)->list->getIterator(context);
        return (new (context) ProtoMultisetIteratorImplementation(context, toImpl<const ProtoSparseListIteratorImplementation>(list_iterator)))->asMultisetIterator(context);
    }

    //=========================================================================
    // ProtoString API
    //=========================================================================
    const ProtoString* ProtoString::fromUTF8String(ProtoContext* context, const char* str) {
        return toImpl<const ProtoStringImplementation>(context->fromUTF8String(str))->asProtoString(context);
    }
    unsigned long ProtoString::getHash(ProtoContext* context) const { return toImpl<const ProtoStringImplementation>(this)->getHash(context); }
    const ProtoString* ProtoString::appendLast(ProtoContext* context, const ProtoString* other) const { return toImpl<const ProtoStringImplementation>(this)->implAppendLast(context, other)->asProtoString(context); }
    const Cell* ProtoString::asCell(ProtoContext* context) const { return toImpl<const ProtoStringImplementation>(this); }

}
