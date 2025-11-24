/*
 * Proto.cpp
 *
 *  Created on: 2020-5-1
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <random>

using namespace std;

namespace proto
{
    const ProtoObject* ProtoObject::getPrototype(ProtoContext* context) const
    {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;

        switch (pa.op.pointer_tag)
        {
        case POINTER_TAG_EMBEDDED_VALUE:
            switch (pa.op.embedded_type)
            {
            case EMBEDDED_TYPE_BOOLEAN:
                return context->space->booleanPrototype;
            case EMBEDDED_TYPE_UNICODE_CHAR:
                return context->space->unicodeCharPrototype;
            // ... (cases for all embedded types)
            default:
                return nullptr;
            };
        case POINTER_TAG_LIST:
            return context->space->listPrototype;
        // ... (cases for all object types)
        default:
            return nullptr;
        }
    }

    const ProtoObject* ProtoObject::clone(ProtoContext* context, bool isMutable) const
    {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = toImpl<const ProtoObjectCell>(this);

            const ProtoObject* newObject = (new(context) ProtoObjectCell(
                context,
                oc->parent,
                oc->attributes,
                0
            ))->asObject(context);

            if (isMutable)
            {
                ProtoSparseList* currentRoot;
                unsigned long randomId = 0;
                do
                {
                    currentRoot = context->space->mutableRoot.load();
                    randomId = generate_mutable_ref();
                    if (currentRoot->has(context, randomId))
                        continue;
                }
                while (!context->space->mutableRoot.compare_exchange_strong(
                    currentRoot,
                    const_cast<ProtoSparseList*>(currentRoot->setAt(context, randomId, newObject))
                ));
            }
            return newObject;
        }
        return PROTO_NONE;
    }

    const ProtoObject* ProtoObject::newChild(ProtoContext* context, bool isMutable) const
    {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = toImpl<const ProtoObjectCell>(this);

            auto* newObject = new(context) ProtoObjectCell(
                context,
                new(context) ParentLinkImplementation(context, oc->parent, this),
                new(context) ProtoSparseListImplementation(context),
                0
            );

            if (isMutable)
            {
                ProtoSparseList* currentRoot;
                unsigned long randomId = 0;
                do
                {
                    currentRoot = context->space->mutableRoot.load();
                    randomId = generate_mutable_ref();
                    if (currentRoot->has(context, randomId))
                        continue;
                }
                while (!context->space->mutableRoot.compare_exchange_strong(
                    currentRoot,
                    const_cast<ProtoSparseList*>(currentRoot->setAt(context, randomId, newObject->asObject(context)))
                ));
            }
            return newObject->asObject(context);
        }
        return PROTO_NONE;
    }

    const ProtoObject* ProtoObject::call(ProtoContext* context,
                          const ParentLink* nextParent,
                          const ProtoString* method,
                          const ProtoObject* self,
                          const ProtoList* unnamedParametersList,
                          const ProtoSparseList* keywordParametersDict)
    {
        const auto* thread = toImpl<const ProtoThreadImplementation>(context->thread);
        const unsigned int hash = (reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(method)) & (THREAD_CACHE_DEPTH - 1);
        auto& [object, attribute_name, value] = thread->extension->attributeCache[hash];

        if (object != this || attribute_name != method)
        {
            object = this;
            attribute_name = method;
            value = this->getAttribute(context, const_cast<ProtoString*>(method));
        }

        if (value && value->isMethod(context)) {
            return value->asMethod(context)(context, const_cast<ProtoObject*>(self), const_cast<ParentLink*>(nextParent), const_cast<ProtoList*>(unnamedParametersList), const_cast<ProtoSparseList*>(keywordParametersDict));
        }

        if (context->space->nonMethodCallback)
            return (*context->space->nonMethodCallback)(context, const_cast<ParentLink*>(nextParent), const_cast<ProtoString*>(method), const_cast<ProtoObject*>(self), const_cast<ProtoList*>(unnamedParametersList), const_cast<ProtoSparseList*>(keywordParametersDict));

        return PROTO_NONE;
    }

    const ProtoObject* ProtoObject::isInstanceOf(ProtoContext* context, const ProtoObject* prototype) const
    {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = toImpl<const ProtoObjectCell>(this);
            const ParentLinkImplementation* currentParent = oc->parent;
            while (currentParent)
            {
                if (currentParent->object == prototype)
                    return PROTO_TRUE;
                currentParent = currentParent->getParent(context);
            }
        }
        return PROTO_FALSE;
    }

    // ... (And so on for the rest of the file, applying const corrections)
}
