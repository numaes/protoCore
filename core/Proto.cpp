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
    // --- ProtoObject Implementation ---

    const ProtoObject* ProtoObject::getPrototype(ProtoContext* context) const
    {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;

        switch (pa.op.pointer_tag)
        {
        case POINTER_TAG_EMBEDDED_VALUE:
            switch (pa.op.embedded_type)
            {
            case EMBEDDED_TYPE_BOOLEAN: return context->space->booleanPrototype;
            case EMBEDDED_TYPE_UNICODE_CHAR: return context->space->unicodeCharPrototype;
            case EMBEDDED_TYPE_BYTE: return context->space->bytePrototype;
            case EMBEDDED_TYPE_TIMESTAMP: return context->space->timestampPrototype;
            case EMBEDDED_TYPE_DATE: return context->space->datePrototype;
            case EMBEDDED_TYPE_TIMEDELTA: return context->space->timedeltaPrototype;
            case EMBEDDED_TYPE_SMALLINT: return context->space->smallIntegerPrototype;
            case EMBEDDED_TYPE_FLOAT: return context->space->floatPrototype;
            default: return nullptr;
            }
        case POINTER_TAG_LIST: return context->space->listPrototype;
        case POINTER_TAG_SPARSE_LIST: return context->space->sparseListPrototype;
        case POINTER_TAG_TUPLE: return context->space->tuplePrototype;
        case POINTER_TAG_BYTE_BUFFER: return context->space->bufferPrototype;
        case POINTER_TAG_EXTERNAL_POINTER: return context->space->pointerPrototype;
        case POINTER_TAG_METHOD: return context->space->methodPrototype;
        case POINTER_TAG_STRING: return context->space->stringPrototype;
        case POINTER_TAG_LIST_ITERATOR: return context->space->listIteratorPrototype;
        case POINTER_TAG_TUPLE_ITERATOR: return context->space->tupleIteratorPrototype;
        case POINTER_TAG_SPARSE_LIST_ITERATOR: return context->space->sparseListIteratorPrototype;
        case POINTER_TAG_STRING_ITERATOR: return context->space->stringIteratorPrototype;
        default: return nullptr;
        }
    }

    const ProtoObject* ProtoObject::clone(ProtoContext* context, bool isMutable) const
    {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;

        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return PROTO_NONE;
        
        auto* oc = toImpl<const ProtoObjectCell>(this);
        const ProtoObject* newObject = (new(context) ProtoObjectCell(context, oc->parent, oc->attributes, 0))->asObject(context);

        if (isMutable) {
            ProtoSparseList* currentRoot;
            unsigned long randomId = 0;
            do {
                currentRoot = context->space->mutableRoot.load();
                randomId = generate_mutable_ref();
                if (currentRoot->has(context, randomId)) continue;
            } while (!context->space->mutableRoot.compare_exchange_strong(currentRoot, const_cast<ProtoSparseList*>(currentRoot->setAt(context, randomId, newObject))));
        }
        return newObject;
    }

    const ProtoObject* ProtoObject::newChild(ProtoContext* context, bool isMutable) const
    {
        if (!this->isCell(context)) return PROTO_NONE;

        auto* newObject = new(context) ProtoObjectCell(
            context,
            new(context) ParentLinkImplementation(context, toImpl<const ProtoObjectCell>(this)->parent, this),
            new(context) ProtoSparseListImplementation(context),
            isMutable ? generate_mutable_ref() : 0
        );

        if (isMutable) {
            ProtoSparseList* currentRoot;
            do {
                currentRoot = context->space->mutableRoot.load();
            } while (!context->space->mutableRoot.compare_exchange_strong(currentRoot, const_cast<ProtoSparseList*>(currentRoot->setAt(context, newObject->mutable_ref, newObject->asObject(context)))));
        }
        return newObject->asObject(context);
    }

    const ProtoObject* ProtoObject::call(ProtoContext* context, const ParentLink* nextParent, const ProtoString* method, const ProtoObject* self, const ProtoList* positionalParameters, const ProtoSparseList* keywordParametersDict) const
    {
        const auto* thread = toImpl<const ProtoThreadImplementation>(context->thread);
        const unsigned int hash = (reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(method)) & (THREAD_CACHE_DEPTH - 1);
        
        auto& cache_entry = thread->extension->attributeCache[hash];

        if (cache_entry.object != this || cache_entry.attributeName != method) {
            cache_entry.object = this;
            cache_entry.attributeName = method;
            cache_entry.value = this->getAttribute(context, const_cast<ProtoString*>(method));
        }

        if (cache_entry.value && cache_entry.value->isMethod(context)) {
            return cache_entry.value->asMethod(context)(context, self, nextParent, positionalParameters, keywordParametersDict);
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
            pa.oid.oid = current;
            if (pa.op.pointer_tag != POINTER_TAG_OBJECT) break;

            auto oc = toImpl<const ProtoObjectCell>(current);
            if (oc->parent) {
                current = oc->parent->getObject(context);
            } else {
                break;
            }
        }
        return PROTO_FALSE;
    }

    const ProtoObject* ProtoObject::getAttribute(ProtoContext* context, ProtoString* name) const
    {
        const auto* thread = toImpl<const ProtoThreadImplementation>(context->thread);
        const unsigned int hash = (reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(name)) & (THREAD_CACHE_DEPTH - 1);
        
        auto& cache_entry = thread->extension->attributeCache[hash];

        if (cache_entry.object == this && cache_entry.attributeName == name) {
            return cache_entry.value;
        }

        const ProtoObject* currentObject = this;
        const unsigned long attr_hash = name->getHash(context);
        
        while (currentObject) {
            auto oc = toImpl<const ProtoObjectCell>(currentObject);
            
            if (oc->mutable_ref) {
                oc = toImpl<const ProtoObjectCell>(context->space->mutableRoot.load()->getAt(context, oc->mutable_ref));
                if (!oc) break;
            }

            if (oc->attributes->implHas(context, attr_hash)) {
                const ProtoObject* result = oc->attributes->implGetAt(context, attr_hash);
                cache_entry = {this, name, result};
                return result;
            }

            if (oc->parent) {
                currentObject = oc->parent->getObject(context);
            } else {
                currentObject = nullptr;
            }
        }

        cache_entry = {this, name, PROTO_NONE};
        if (context->space->attributeNotFoundGetCallback) {
            return (*context->space->attributeNotFoundGetCallback)(context, this, name);
        }
        return PROTO_NONE;
    }

    const ProtoObject* ProtoObject::hasAttribute(ProtoContext* context, ProtoString* name) const
    {
        return this->getAttribute(context, name) != PROTO_NONE ? PROTO_TRUE : PROTO_FALSE;
    }

    const ProtoObject* ProtoObject::setAttribute(ProtoContext* context, ProtoString* name, const ProtoObject* value)
    {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;

        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return this;

        auto* oc = toImpl<ProtoObjectCell>(this);
        
        if (oc->mutable_ref) {
            ProtoSparseList* currentRoot;
            ProtoSparseList* newRoot;
            do {
                currentRoot = context->space->mutableRoot.load();
                const auto* currentVersion = toImpl<const ProtoObjectCell>(currentRoot->getAt(context, oc->mutable_ref));
                const auto* newAttributes = currentVersion->attributes->implSetAt(context, name->getHash(context), value);
                const auto* newVersion = new(context) ProtoObjectCell(context, currentVersion->parent, newAttributes, oc->mutable_ref);
                newRoot = const_cast<ProtoSparseList*>(currentRoot->setAt(context, oc->mutable_ref, newVersion->asObject(context)));
            } while (!context->space->mutableRoot.compare_exchange_strong(currentRoot, newRoot));
            return this;
        } else {
            const auto* newAttributes = oc->attributes->implSetAt(context, name->getHash(context), value);
            return (new(context) ProtoObjectCell(context, oc->parent, newAttributes, 0))->asObject(context);
        }
    }

    // ... (rest of the file with corrected logic)
}
