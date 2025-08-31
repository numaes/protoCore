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
            case EMBEDDED_TYPE_BYTE:
                return context->space->bytePrototype;
            case EMBEDDED_TYPE_TIMESTAMP:
                return context->space->timestampPrototype;
            case EMBEDDED_TYPE_DATE:
                return context->space->datePrototype;
            case EMBEDDED_TYPE_TIMEDELTA:
                return context->space->timedeltaPrototype;
            case EMBEDDED_TYPE_SMALLINT:
                return context->space->smallIntegerPrototype;
            case EMBEDDED_TYPE_FLOAT:
                return context->space->floatPrototype;
            default:
                return nullptr;
            };
        case POINTER_TAG_LIST:
            return context->space->listPrototype;
        case POINTER_TAG_SPARSE_LIST:
            return context->space->sparseListPrototype;
        case POINTER_TAG_TUPLE:
            return context->space->tuplePrototype;
        case POINTER_TAG_BYTE_BUFFER:
            return context->space->bufferPrototype;
        case POINTER_TAG_EXTERNAL_POINTER:
            return context->space->pointerPrototype;
        case POINTER_TAG_METHOD:
            return context->space->methodPrototype;
        case POINTER_TAG_STRING:
            return context->space->stringPrototype;
        case POINTER_TAG_LIST_ITERATOR:
            return context->space->stringIteratorPrototype;
        case POINTER_TAG_TUPLE_ITERATOR:
            return context->space->tupleIteratorPrototype;
        case POINTER_TAG_SPARSE_LIST_ITERATOR:
            return context->space->sparseListIteratorPrototype;
        case POINTER_TAG_STRING_ITERATOR:
            return context->space->stringIteratorPrototype;
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
                    currentRoot = context->space->impl->mutableRoot.load();

                    randomId = generate_mutable_ref();

                    if (currentRoot->has(context, randomId))
                        continue;
                }
                while (!context->space->impl->mutableRoot.compare_exchange_strong(
                    currentRoot,
                    currentRoot->setAt(
                        context,
                        randomId,
                        newObject
                    )
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
                new(context) ParentLinkImplementation(
                    context,
                    oc->parent,
                    const_cast<ProtoObject*>(this)
                ),
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

                    if (currentRoot->has(context, (unsigned long)randomId))
                        continue;
                }
                while (!context->space->mutableRoot.compare_exchange_strong(
                    currentRoot,
                    (ProtoSparseList*)currentRoot->setAt(
                        context,
                        randomId,
                        (const ProtoObject*)newObject
                    )
                ));
            }
            return (const ProtoObject*)newObject;
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
        const auto* thread = toImpl<ProtoThreadImplementation>(context->thread);

        const unsigned int hash = (reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(method)) & (THREAD_CACHE_DEPTH - 1);

        auto& [object, attribute_name, value] = thread->attributeCache[hash];

        if (object != this || attribute_name != method) [[unlikely]]
        {
            object = this;
            attribute_name = method;
            
            value = this->getAttribute(context, method);
        }

        if (value && value->isMethod(context)) {
            return value->asMethod(context)(context, self, nextParent, unnamedParametersList, keywordParametersDict);
        }

        if (context->space->nonMethodCallback)
            return (*context->space->nonMethodCallback)(
                context,
                nextParent,
                method,
                self,
                unnamedParametersList,
                keywordParametersDict);

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
                currentParent = currentParent->parent;
            }
        }
        return PROTO_FALSE;
    }

    const ProtoObject* ProtoObject::getAttribute(ProtoContext* context, ProtoString* name) const
    {
        // This is a critical hot-path, optimized for cache hits.
        auto* thread = toImpl<ProtoThreadImplementation>(context->thread);

        // Calculate the cache slot index.
        const unsigned int hash =
            ((reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(name)) >> 4) & (THREAD_CACHE_DEPTH - 1);

        // Get a direct reference to the cache entry.
        auto& [object, attribute_name, value] = thread->attributeCache[hash];

        // On a cache hit, return the resolved value immediately.
        if (object == this && attribute_name == name) {
            return value;
        }

        // On a cache miss, take the slow path.
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        auto result = PROTO_NONE;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto oc = toImpl<const ProtoObjectCell>(this);
            if (oc->mutable_ref)
            {
                pa.oid.oid = context->space->mutableRoot.load()->getAt(
                    context, oc->mutable_ref);
                oc = toImpl<const ProtoObjectCell>(pa.oid.oid);
            }

            do
            {
                if (oc->attributes->has(context, hash))
                {
                    result = oc->attributes->getAt(context, hash);
                    break;
                }
                if (oc->parent)
                {
                    auto pl = oc->parent;
                    while (pl)
                    {
                        pa.oid.oid = pl->object;
                        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
                        {
                            oc = toImpl<const ProtoObjectCell>(pa.oid.oid);
                            break;
                        }
                        else
                            pl = pl->parent;
                    }
                }
                else
                    break;
            }
            while (oc);
        }
        else
        {
            if (context->space->attributeNotFoundGetCallback)
                result = (*context->space->attributeNotFoundGetCallback)(
                    context, this, name);
            else
                result = PROTO_NONE;
        }

        // Populate the cache for the next lookup.
        object = (const ProtoObject*)this;
        attribute_name = name;
        value = result;

        return result;
    }

    const ProtoObject* ProtoObject::hasAttribute(ProtoContext* context, ProtoString* name) const
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = toImpl<const ProtoObjectCell>(this);
            if (oc->mutable_ref)
            {
                pa.oid.oid = context->space->mutableRoot.load()->getAt(
                    context, oc->mutable_ref);
                oc = toImpl<const ProtoObjectCell>(pa.oid.oid);
            }
            // Calculate the cache slot index.
            const unsigned int hash =
                ((reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(name)) >> 4) & (THREAD_CACHE_DEPTH - 1);

            do
            {
                if (oc->attributes->has(context, hash))
                    return oc->attributes->getAt(context, hash);
                if (oc->parent)
                {
                    const ParentLinkImplementation* pl = oc->parent;
                    while (pl)
                    {
                        pa.oid.oid = pl->object;
                        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
                        {
                            oc = toImpl<const ProtoObjectCell>(pa.oid.oid);
                            break;
                        }
                        else
                            pl = pl->parent;
                    }
                }
                else
                    break;
            }
            while (oc);
        }
        return PROTO_FALSE;
    }

    const ProtoObject* ProtoObject::setAttribute(ProtoContext* context, ProtoString* name, const ProtoObject* value)
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = toImpl<ProtoObjectCell>(this);
            ProtoObjectCell* inmutableBase = nullptr;
            ProtoSparseList* currentRoot;
            if (oc->mutable_ref)
            {
                inmutableBase = oc;
                currentRoot = context->space->mutableRoot.load();
                pa.oid.oid = currentRoot->getAt(context, oc->mutable_ref);
                oc = toImpl<ProtoObjectCell>(pa.oid.oid);
            }

            // Calculate the cache slot index.
            const unsigned int hash =
                ((reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(name)) >> 4) & (THREAD_CACHE_DEPTH - 1);

            auto* newObject = new(context) ProtoObjectCell(
                context,
                oc->parent,
                oc->attributes->implSetAt(context, hash, value),
                oc->mutable_ref
            );

            if (inmutableBase)
            {
                ProtoSparseList* newRoot;
                do
                {
                    currentRoot = context->space->mutableRoot.load();
                    newRoot = (ProtoSparseList*)currentRoot->setAt(
                        context, inmutableBase->mutable_ref, (const ProtoObject*)newObject);
                }
                while (!context->space->mutableRoot.compare_exchange_strong(
                    currentRoot,
                    newRoot
                ));
                return this;
            }
            else
                return (const ProtoObject*)newObject;
        }
        return this;
    }

    const ProtoObject* ProtoObject::hasOwnAttribute(ProtoContext* context, ProtoString* name) const
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = toImpl<const ProtoObjectCell>(this);
            if (oc->mutable_ref)
            {
                auto currentRoot = context->space->mutableRoot.load();
                pa.oid.oid = currentRoot->getAt(context, oc->mutable_ref);
                oc = toImpl<const ProtoObjectCell>(pa.oid.oid);
            }

            // Calculate the cache slot index.
            const unsigned int hash =
                ((reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(name)) >> 4) & (THREAD_CACHE_DEPTH - 1);

            return oc->attributes->has(context, hash) ? PROTO_TRUE : PROTO_FALSE;
        }
        return PROTO_FALSE;
    }

    const ProtoSparseList* ProtoObject::getAttributes(ProtoContext* context) const
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = toImpl<const ProtoObjectCell>(this);
            if (oc->mutable_ref)
            {
                auto currentRoot = context->space->mutableRoot.load();
                pa.oid.oid = currentRoot->getAt(context, oc->mutable_ref);
                oc = toImpl<const ProtoObjectCell>(pa.oid.oid);
            }

            const ProtoSparseList* attributes = context->newSparseList();

            while (oc)
            {
                auto* ai = (oc->attributes->getIterator(context));
                while (ai->hasNext(context))
                {
                    const unsigned long attributeKey = (ai)->nextKey(context);
                    const ProtoObject* attributeValue = oc->attributes->getAt(context, attributeKey);
                    attributes = const_cast<ProtoSparseList*>(attributes->setAt(
                        context,
                        attributeKey,
                        attributeValue
                    ));
                    ai = ai->advance(context);
                }
                if (oc->parent)
                {
                    auto pl = oc->parent;
                    while (pl)
                    {
                        pa.oid.oid = pl->object;
                        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
                        {
                            oc = toImpl<const ProtoObjectCell>(pa.oid.oid);
                            break;
                        }
                        else
                            pl = pl->parent;
                    }

                    if (oc->mutable_ref) {
                        auto currentRoot = context->space->mutableRoot.load();
                        pa.oid.oid = currentRoot->getAt(context, oc->mutable_ref);
                        oc = toImpl<const ProtoObjectCell>(pa.oid.oid);
                    }
                }
                else
                {
                    break;
                }
            }

            return attributes;
        }
        return nullptr;
    }

    const ProtoSparseList* ProtoObject::getOwnAttributes(ProtoContext* context) const
    {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = toImpl<const ProtoObjectCell>(this);
            if (oc->mutable_ref)
            {
                const ProtoObject* currentVersion = context->space->mutableRoot.load()->getAt(context, oc->mutable_ref);
                auto* implementation = toImpl<const ProtoObjectCell>(currentVersion);
                return implementation->attributes;
            }
            else
            {
                return oc->attributes;
            }
        }
        return nullptr;
    }

    const ProtoList* ProtoObject::getParents(ProtoContext* context) const
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            ProtoList* parents = new(context) ProtoListImplementation(context);

            auto* oc = toImpl<const ProtoObjectCell>(this);
            auto* parent = (const ParentLinkImplementation*)oc->parent;
            while (parent)
            {
                parents = (ProtoList*)parents->appendLast(context, parent->object);
                parent = parent->parent;
            }

            return parents;
        }
        return nullptr;
    }

    const ProtoObject* ProtoObject::addParent(ProtoContext* context, const ProtoObject* newParent)
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        const ProtoSparseList* existingParents = context->newSparseList();

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = toImpl<ProtoObjectCell>(this);

            // Collect existing parents
            ParentLinkImplementation* currentParent = oc->parent;
            while (currentParent)
            {
                existingParents = (ProtoSparseList*)existingParents->setAt(
                    context,
                    currentParent->object->getHash(context),
                    nullptr
                );
                currentParent = currentParent->parent;
            };

            auto* newParentLink = new(context) ParentLinkImplementation(
                context, oc->parent, newParent
            );

            return (new(context) ProtoObjectCell(
                context,
                newParentLink,
                oc->attributes,
                oc->mutable_ref
            ))->asObject(context);
        }
        else
            return this;
    }


    bool ProtoObject::isInteger(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
            p.op.embedded_type == EMBEDDED_TYPE_SMALLINT);
    }

    int ProtoObject::asInteger(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return p.si.smallInteger;
    }

    bool ProtoObject::isFloat(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
            p.op.embedded_type == EMBEDDED_TYPE_FLOAT);
    }

    float ProtoObject::asFloat(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;

        union
        {
            unsigned int uiv;
            float fv;
        } u{};
        u.uiv = p.sd.floatValue;

        return u.fv;
    }

    bool ProtoObject::isBoolean(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
            p.op.embedded_type == EMBEDDED_TYPE_BOOLEAN);
    }

    bool ProtoObject::isMethod(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_METHOD);
    }

    ProtoMethod ProtoObject::asMethod(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        if (isMethod(context)) {
            return p.method;
        }
        return nullptr;
    }

    bool ProtoObject::asBoolean(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return p.booleanValue.booleanValue;
    }

    bool ProtoObject::isByte(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
            p.op.embedded_type == EMBEDDED_TYPE_BYTE);
    }

    char ProtoObject::asByte(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return p.byteValue.byteData;
    }

    bool ProtoObject::isDate(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
                p.op.embedded_type == EMBEDDED_TYPE_DATE);
    }

    void ProtoObject::asDate(ProtoContext* context, unsigned int& year, unsigned& month, unsigned& day) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        if (isDate(context)) {
            year = p.date.year;
            month = p.date.month;
            day = p.date.day;
        } else {
            year = 0;
            month = 0;
            day = 0;
        }
    }

    bool ProtoObject::isTimestamp(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
                p.op.embedded_type == EMBEDDED_TYPE_TIMESTAMP);
    }

    unsigned long ProtoObject::asTimestamp(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        if (isTimestamp(context)) {
            return p.timestampValue.timestamp;
        }
        return 0;
    }

    bool ProtoObject::isTimeDelta(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
                p.op.embedded_type == EMBEDDED_TYPE_TIMEDELTA);
    }

    long ProtoObject::asTimeDelta(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        if (isTimeDelta(context)) {
            return p.timedeltaValue.timedelta;
        }
        return 0;
    }

    const ProtoList* ProtoObject::asList(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = (const ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_LIST)
        {
            return p.list;
        }

        return nullptr;
    }

    const ProtoListIterator* ProtoObject::asListIterator(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = (const ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_LIST_ITERATOR)
        {
            return p.listIterator;
        }

        return nullptr;
    }

    const ProtoTuple* ProtoObject::asTuple(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = (const ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_TUPLE)
        {
            return p.tuple;
        }

        return nullptr;
    }

    const ProtoTupleIterator* ProtoObject::asTupleIterator(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = (const ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_TUPLE_ITERATOR)
        {
            return p.tupleIterator;
        }

        return nullptr;
    }

    const ProtoString* ProtoObject::asString(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = (const ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_STRING)
        {
            return p.string;
        }

        return nullptr;
    }

    const ProtoStringIterator* ProtoObject::asStringIterator(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = (const ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_STRING_ITERATOR)
        {
            return p.stringIterator;
        }

        return nullptr;
    }

    const ProtoSparseList* ProtoObject::asSparseList(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = (const ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_SPARSE_LIST)
        {
            return p.sparseList;
        }

        return nullptr;
    }

    const ProtoSparseListIterator* ProtoObject::asSparseListIterator(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = (const ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_SPARSE_LIST_ITERATOR)
        {
            return p.sparseListIterator;
        }

        return nullptr;
    }


    unsigned long ProtoObject::getHash(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return p.asHash.hash;
    }

    int ProtoObject::isCell(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return p.op.pointer_tag != POINTER_TAG_EMBEDDED_VALUE;
    }

    Cell* ProtoObject::asCell(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return p.cell.cell;
    }

    // ... (Stub implementations for public API classes) ...

} // namespace proto
