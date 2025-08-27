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
    ProtoObject* ProtoObject::getPrototype(const ProtoContext* context)
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
            case EMBEDDED_TYPE_UNICODECHAR:
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

    ProtoObject* ProtoObject::clone(ProtoContext* context, bool isMutable)
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = pa.objectCellImplementation;

            ProtoObject* newObject = (new(context) ProtoObjectCell(
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
                    (ProtoSparseList*)currentRoot->setAt(
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

    ProtoObject* ProtoObject::newChild(ProtoContext* context, bool isMutable)
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = pa.objectCellImplementation;

            auto* newObject = new(context) ProtoObjectCell(
                context,
                new(context) ParentLinkImplementation(
                    context,
                    oc->parent,
                    oc
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
                        (ProtoObject*)newObject
                    )
                ));
            }
            return (ProtoObject*)newObject;
        }
        return PROTO_NONE;
    }

    ProtoObject* ProtoObject::call(ProtoContext* context,
                          ParentLink* nextParent,
                          ProtoString* method,
                          ProtoObject* self,
                          ProtoList* unnamedParametersList,
                          ProtoSparseList* keywordParametersDict)
    {
        const auto* thread = toImpl<ProtoThreadImplementation>(context->thread);

        const unsigned int hash = (reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(method)) & (THREAD_CACHE_DEPTH - 1);

        auto& [object, attribute_name, value] = thread->attribute_cache[hash];

        if (object != this || attribute_name != method) [[unlikely]]
        {
            object = this;
            attribute_name = method;
            
            value = this->getAttribute(context, method);
        }

        if (value && value->isMethod(context)) {
            return value->asMethod(context)(context, self, nextParent, unnamedParametersList, keywordParametersDict);
        }
        
        return PROTO_NONE;
    }

    ProtoObject* ProtoObject::isInstanceOf(ProtoContext* context, const ProtoObject* prototype)
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = pa.objectCellImplementation;

            ParentLinkImplementation* currentParent = oc->parent;
            while (currentParent)
            {
                if (currentParent->object == prototype)
                    return PROTO_TRUE;
                currentParent = currentParent->parent;
            }
        }
        return PROTO_FALSE;
    }

    ProtoObject* ProtoObject::getAttribute(ProtoContext* context, ProtoString* name)
    {
        // This is a critical hot-path, optimized for cache hits.
        auto* thread = toImpl<ProtoThreadImplementation>(context->thread);

        // Calculate the cache slot index.
        const unsigned int hash =
            ((reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(name)) >> 4) & (THREAD_CACHE_DEPTH - 1);

        // Get a direct reference to the cache entry.
        auto& [object, attribute_name, value] = thread->attribute_cache[hash];

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
            auto oc = pa.objectCellImplementation;
            if (oc->mutable_ref)
            {
                pa.oid.oid = context->space->mutableRoot.load()->getAt(
                    context, oc->mutable_ref);
                oc = pa.objectCellImplementation;
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
                            oc = pa.objectCell;
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
            result = PROTO_NONE;

        // Populate the cache for the next lookup.
        object = this;
        attribute_name = name;
        value = result;

        return result;
    }

    ProtoObject* ProtoObject::hasAttribute(ProtoContext* context, ProtoString* name)
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = pa.objectCellImplementation;
            if (oc->mutable_ref)
            {
                pa.oid.oid = context->space->mutableRoot.load()->getAt(
                    context, oc->mutable_ref);
                oc = pa.objectCellImplementation;
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
                    auto pl = oc->parent;
                    while (pl)
                    {
                        pa.oid.oid = pl->object;
                        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
                        {
                            oc = pa.objectCell;
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

    ProtoObject* ProtoObject::setAttribute(ProtoContext* context, ProtoString* name, ProtoObject* value)
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = pa.objectCellImplementation;
            ProtoObjectCell* inmutableBase = nullptr;
            ProtoSparseList* currentRoot;
            if (oc->mutable_ref)
            {
                inmutableBase = oc;
                currentRoot = context->space->mutableRoot.load();
                pa.oid.oid = currentRoot->getAt(context, oc->mutable_ref);
                oc = pa.objectCellImplementation;
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
                        context, inmutableBase->mutable_ref, (ProtoObject*)newObject);
                }
                while (!context->space->mutableRoot.compare_exchange_strong(
                    currentRoot,
                    newRoot
                ));
                return this;
            }
            else
                return (ProtoObject*)newObject;
        }
        return this;
    }

    ProtoObject* ProtoObject::hasOwnAttribute(ProtoContext* context, ProtoString* name)
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            // FIX: 'oc' cannot be const because it is reassigned if the object is mutable.
            auto* oc = pa.objectCellImplementation;
            if (oc->mutable_ref)
            {
                auto currentRoot = context->space->mutableRoot.load();
                pa.oid.oid = currentRoot->getAt(context, oc->mutable_ref);
                oc = pa.objectCellImplementation;
            }

            // Calculate the cache slot index.
            const unsigned int hash =
                ((reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(name)) >> 4) & (THREAD_CACHE_DEPTH - 1);

            return oc->attributes->has(context, hash) ? PROTO_TRUE : PROTO_FALSE;
        }
        return PROTO_FALSE;
    }

    ProtoSparseList* ProtoObject::getAttributes(ProtoContext* context)
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = pa.objectCellImplementation;
            if (oc->mutable_ref)
            {
                auto currentRoot = context->space->mutableRoot.load();
                pa.oid.oid = currentRoot->getAt(context, oc->mutable_ref);
                oc = pa.objectCellImplementation;
            }

            ProtoSparseList* attributes = context->newSparseList();

            while (oc)
            {
                auto* ai = (oc->attributes->getIterator(context));
                while (ai->hasNext(context))
                {
                    const unsigned long attributeKey = (ai)->nextKey(context);
                    ProtoObject* attributeValue = oc->attributes->getAt(context, attributeKey);
                    attributes = (ProtoSparseList*)attributes->setAt(
                        context,
                        attributeKey,
                        attributeValue
                    );
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
                            oc = pa.objectCell;
                            break;
                        }
                        else
                            pl = pl->parent;
                    }

                    if (oc->mutable_ref) {
                        auto currentRoot = context->space->mutableRoot.load();
                        pa.oid.oid = currentRoot->getAt(context, oc->mutable_ref);
                        oc = pa.objectCellImplementation;
                    }
                }
                else
                {
                    // FIX: Break the loop if there are no more parents to avoid an infinite loop.
                    break;
                }
            }

            return attributes;
        }
        return nullptr;
    }

    ProtoSparseList* ProtoObject::getOwnAttributes(ProtoContext* context)
    {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            if (auto* oc = pa.objectCellImplementation; oc->mutable_ref)
            {
                // For mutable objects, we must look up the current immutable version
                // from the global mutable root.
                ProtoObject* currentVersion = context->space->mutableRoot.load()->getAt(context, oc->mutable_ref);
                
                // Cast the generic object pointer to its specific implementation type.
                auto* implementation = toImpl<ProtoObjectCell>(currentVersion);
                
                // Return its attributes. The result (ProtoSparseListImplementation*) is
                // safely convertible to the public API type (ProtoSparseList*).
                return implementation->attributes;
            }
            else
            {
                // For immutable objects, we can directly access their attributes.
                return oc->attributes;
            }
        }
        return nullptr; // Not an object, so it has no attributes.
    }

    ProtoList* ProtoObject::getParents(ProtoContext* context)
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            ProtoList* parents = new(context) ProtoListImplementation(context);

            auto* oc = pa.objectCellImplementation;
            auto* parent = (ParentLinkImplementation*)oc->parent;
            while (parent)
            {
                parents = (ProtoList*)parents->appendLast(context, parent->object);
                parent = parent->parent;
            }

            return parents;
        }
        return nullptr;
    }

    void processTail(ProtoContext* context,
                     ProtoSparseList** existingParents,
                     const ParentLinkImplementation* currentParent,
                     ParentLinkImplementation** newParentLink)
    {
        if (currentParent->parent)
            // FIX: The recursive call must be on the next parent, not the current one.
            processTail(context, existingParents, currentParent->parent, newParentLink);

        if (!(*existingParents)->has(context, currentParent->object->getObjectHash(context)))
        {
            *newParentLink = new(context) ParentLinkImplementation(
                context,
                *newParentLink,
                currentParent->object
            );
            (*existingParents) = (ProtoSparseList*)(*existingParents)->setAt(
                context,
                currentParent->object->getObjectHash(context),
                (*newParentLink)->asObject(context)
            );
        }
    }

    ProtoObject* ProtoObject::addParent(ProtoContext* context, ProtoObject* newParent)
    {
        ProtoObjectPointer pa{};

        pa.oid.oid = this;

        ProtoSparseList* existingParents = context->newSparseList();

        if (pa.op.pointer_tag == POINTER_TAG_OBJECT)
        {
            auto* oc = pa.objectCellImplementation;

            // Collect existing parents
            ParentLinkImplementation* currentParent = oc->parent;
            while (currentParent)
            {
                existingParents = (ProtoSparseList*)existingParents->setAt(
                    context,
                    currentParent->object->getObjectHash(context),
                    nullptr
                );
                currentParent = currentParent->parent;
            };

            auto* newParentLink = new(context) ParentLinkImplementation(
                context, oc->parent, reinterpret_cast<ProtoObjectCell*>(newParent)
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


    bool ProtoObject::isInteger(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
            p.op.embedded_type == EMBEDDED_TYPE_SMALLINT);
    }

    int ProtoObject::asInteger(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return p.si.smallInteger;
    }

    bool ProtoObject::isFloat(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
            p.op.embedded_type == EMBEDDED_TYPE_FLOAT);
    }

    float ProtoObject::asFloat(ProtoContext* context)
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

    bool ProtoObject::isBoolean(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
            p.op.embedded_type == EMBEDDED_TYPE_BOOLEAN);
    }

    bool ProtoObject::isMethod(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_METHOD);
    }

    ProtoMethod ProtoObject::asMethod(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        if (isMethod(context)) {
            return p.method;
        }
        return nullptr;
    }

    bool ProtoObject::asBoolean(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return p.booleanValue.booleanValue;
    }

    bool ProtoObject::isByte(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
            p.op.embedded_type == EMBEDDED_TYPE_BYTE);
    }

    char ProtoObject::asByte(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return p.byteValue.byteData;
    }

    bool ProtoObject::isDate(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
                p.op.embedded_type == EMBEDDED_TYPE_DATE);
    }

    void ProtoObject::asDate(ProtoContext* context, unsigned int& year, unsigned& month, unsigned& day)
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

    bool ProtoObject::isTimestamp(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
                p.op.embedded_type == EMBEDDED_TYPE_TIMESTAMP);
    }

    unsigned long ProtoObject::asTimestamp(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        if (isTimestamp(context)) {
            return p.timestampValue.timestamp;
        }
        return 0;
    }

    bool ProtoObject::isTimeDelta(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
                p.op.embedded_type == EMBEDDED_TYPE_TIMEDELTA);
    }

    long ProtoObject::asTimeDelta(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        if (isTimeDelta(context)) {
            return p.timedeltaValue.timedelta;
        }
        return 0;
    }

    ProtoList* ProtoObject::asList(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = (ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_LIST)
        {
            return p.list;
        }

        return nullptr;
    }

    ProtoListIterator* ProtoObject::asListIterator(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = (ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_LIST_ITERATOR)
        {
            return p.listIterator;
        }

        return nullptr;
    }

    ProtoTuple* ProtoObject::asTuple(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = (ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_TUPLE)
        {
            return p.tuple;
        }

        return nullptr;
    }

    ProtoTupleIterator* ProtoObject::asTupleIterator(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = (ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_TUPLE_ITERATOR)
        {
            return p.tupleIterator;
        }

        return nullptr;
    }

    ProtoString* ProtoObject::asString(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = (ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_STRING)
        {
            return p.string;
        }

        return nullptr;
    }

    ProtoStringIterator* ProtoObject::asStringIterator(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = (ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_STRING_ITERATOR)
        {
            return p.stringIterator;
        }

        return nullptr;
    }

    ProtoSparseList* ProtoObject::asSparseList(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = (ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_SPARSE_LIST)
        {
            return p.sparseList;
        }

        return nullptr;
    }

    ProtoSparseListIterator* ProtoObject::asSparseListIterator(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = (ProtoObject*)this;
        if (p.op.pointer_tag == POINTER_TAG_SPARSE_LIST_ITERATOR)
        {
            return p.sparseListIterator;
        }

        return nullptr;
    }


    unsigned long ProtoObject::getObjectHash(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return p.asHash.hash;
    }

    int ProtoObject::isCell(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return p.op.pointer_tag != POINTER_TAG_EMBEDDED_VALUE;
    }

    Cell* ProtoObject::asCell(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = this;
        return p.cell.cell;
    }

    /*
     * This block provides "stub" implementations for the public interface
     * classes defined in proto.h. Its sole purpose is to give the
     * compiler a place to generate the v-tables necessary for linking,
     * while respecting the implementation-hiding design.
     *
     * These functions should never be called directly. Polymorphism
     * should redirect calls to the actual implementation classes.
     */


    // ------------------- ParentLink -------------------

    ProtoObject* ParentLink::getObject(ProtoContext* context)
    {
        return toImpl<ParentLinkImplementation>(this)->asObject(context);
    }

    ParentLink* ParentLink::getParent(ProtoContext* context)
    {
        return toImpl<ParentLinkImplementation>(this)->parent;
    }

    // ------------------- ProtoListIterator -------------------

    int ProtoListIterator::hasNext(ProtoContext* context)
    {
        return toImpl<ProtoListIteratorImplementation>(this)->implHasNext(context);
    }

    ProtoObject* ProtoListIterator::next(ProtoContext* context)
    {
        return toImpl<ProtoListIteratorImplementation>(this)->implNext(context);
    }

    ProtoListIterator* ProtoListIterator::advance(ProtoContext* context)
    {
        return toImpl<ProtoListIteratorImplementation>(this)->implAdvance(context);
    }

    ProtoObject* ProtoListIterator::asObject(ProtoContext* context)
    {
        return toImpl<ProtoListIteratorImplementation>(this)->implAsObject(context);
    }

    // ------------------- ProtoList -------------------

    ProtoObject* ProtoList::getAt(ProtoContext* context, const int index)
    {
        return toImpl<ProtoListImplementation>(this)->implGetAt(context, index);
    }

    ProtoObject* ProtoList::getFirst(ProtoContext* context)
    {
        return toImpl<ProtoListImplementation>(this)->implGetFirst(context);
    }

    ProtoObject* ProtoList::getLast(ProtoContext* context)
    {
        return toImpl<ProtoListImplementation>(this)->implGetLast(context);
    }

    ProtoList* ProtoList::getSlice(ProtoContext* context, const int from, const int to)
    {
        return toImpl<ProtoListImplementation>(this)->implGetSlice(context, from, to);
    }

    unsigned long ProtoList::getSize(ProtoContext* context)
    {
        return toImpl<ProtoListImplementation>(this)->implGetSize(context);
    }

    bool ProtoList::has(ProtoContext* context, ProtoObject* value)
    {
        return toImpl<ProtoListImplementation>(this)->implHas(context, value);
    }

    ProtoList* ProtoList::setAt(ProtoContext* context, const int index, ProtoObject* value)
    {
        return toImpl<ProtoListImplementation>(this)->implSetAt(context, index, value);
    }

    ProtoList* ProtoList::insertAt(ProtoContext* context, const int index, ProtoObject* value)
    {
        return toImpl<ProtoListImplementation>(this)->implInsertAt(context, index, value);
    }

    ProtoList* ProtoList::appendFirst(ProtoContext* context, ProtoObject* value)
    {
        return toImpl<ProtoListImplementation>(this)->implAppendFirst(context, value);
    }

    ProtoList* ProtoList::appendLast(ProtoContext* context, ProtoObject* value)
    {
        return toImpl<ProtoListImplementation>(this)->implAppendLast(context, value);
    }

    ProtoList* ProtoList::extend(ProtoContext* context, ProtoList* other)
    {
        return toImpl<ProtoListImplementation>(this)->implExtend(context, other);
    }

    ProtoList* ProtoList::splitFirst(ProtoContext* context, const int index)
    {
        return toImpl<ProtoListImplementation>(this)->implSplitFirst(context, index);
    }

    ProtoList* ProtoList::splitLast(ProtoContext* context, const int index)
    {
        return toImpl<ProtoListImplementation>(this)->implSplitLast(context, index);
    }

    ProtoList* ProtoList::removeFirst(ProtoContext* context)
    {
        return toImpl<ProtoListImplementation>(this)->implRemoveFirst(context);
    }

    ProtoList* ProtoList::removeLast(ProtoContext* context)
    {
        return toImpl<ProtoListImplementation>(this)->implRemoveLast(context);
    }

    ProtoList* ProtoList::removeAt(ProtoContext* context, const int index)
    {
        return toImpl<ProtoListImplementation>(this)->implRemoveAt(context, index);
    }

    ProtoList* ProtoList::removeSlice(ProtoContext* context, const int from, const int to)
    {
        return toImpl<ProtoListImplementation>(this)->implRemoveSlice(context, from, to);
    }

    ProtoObject* ProtoList::asObject(ProtoContext* context)
    {
        return toImpl<ProtoListImplementation>(this)->implAsObject(context);
    }

    ProtoListIterator* ProtoList::getIterator(ProtoContext* context)
    {
        return toImpl<ProtoListImplementation>(this)->implGetIterator(context);
    }

    // ------------------- ProtoTupleIterator -------------------

    int ProtoTupleIterator::hasNext(ProtoContext* context)
    {
        return toImpl<ProtoTupleIteratorImplementation>(this)->implHasNext(context);
    }

    ProtoObject* ProtoTupleIterator::next(ProtoContext* context)
    {
        return toImpl<ProtoTupleIteratorImplementation>(this)->implNext(context);
    }

    ProtoTupleIterator* ProtoTupleIterator::advance(ProtoContext* context)
    {
        return toImpl<ProtoTupleIteratorImplementation>(this)->implAdvance(context);
    }

    ProtoObject* ProtoTupleIterator::asObject(ProtoContext* context)
    {
        return toImpl<ProtoTupleIteratorImplementation>(this)->implAsObject(context);
    }

    // ------------------- ProtoTuple -------------------

    ProtoObject* ProtoTuple::getAt(ProtoContext* context, const int index)
    {
        return toImpl<ProtoTupleImplementation>(this)->implGetAt(context, index);
    }

    ProtoObject* ProtoTuple::getFirst(ProtoContext* context)
    {
        return toImpl<ProtoTupleImplementation>(this)->implGetFirst(context);
    }

    ProtoObject* ProtoTuple::getLast(ProtoContext* context)
    {
        return toImpl<ProtoTupleImplementation>(this)->implGetLast(context);
    }

    ProtoObject* ProtoTuple::getSlice(ProtoContext* context, const int from, const int to)
    {
        return toImpl<ProtoTupleImplementation>(this)->implGetSlice(context, from, to)->implAsObject(context);
    }

    unsigned long ProtoTuple::getSize(ProtoContext* context)
    {
        return toImpl<ProtoTupleImplementation>(this)->implGetSize(context);
    }

    bool ProtoTuple::has(ProtoContext* context, ProtoObject* value)
    {
        return toImpl<ProtoTupleImplementation>(this)->implHas(context, value);
    }

    ProtoObject* ProtoTuple::setAt(ProtoContext* context, const int index, ProtoObject* value)
    {
        return toImpl<ProtoTupleImplementation>(this)->implSetAt(context, index, value)->implAsObject(context);
    }

    ProtoObject* ProtoTuple::insertAt(ProtoContext* context, const int index, ProtoObject* value)
    {
        return toImpl<ProtoTupleImplementation>(this)->implInsertAt(context, index, value)->implAsObject(context);
    }

    ProtoObject* ProtoTuple::appendFirst(ProtoContext* context, ProtoTuple* otherTuple)
    {
        return toImpl<ProtoTupleImplementation>(this)->implAppendFirst(context, otherTuple)->implAsObject(context);
    }

    ProtoObject* ProtoTuple::appendLast(ProtoContext* context, ProtoTuple* otherTuple)
    {
        return toImpl<ProtoTupleImplementation>(this)->implAppendLast(context, otherTuple)->implAsObject(context);
    }

    ProtoObject* ProtoTuple::splitFirst(ProtoContext* context, const int count)
    {
        return toImpl<ProtoTupleImplementation>(this)->implSplitFirst(context, count)->implAsObject(context);
    }

    ProtoObject* ProtoTuple::splitLast(ProtoContext* context, const int count)
    {
        return toImpl<ProtoTupleImplementation>(this)->implSplitLast(context, count)->implAsObject(context);
    }

    ProtoObject* ProtoTuple::removeFirst(ProtoContext* context, const int count)
    {
        return toImpl<ProtoTupleImplementation>(this)->implRemoveFirst(context, count)->implAsObject(context);
    }

    ProtoObject* ProtoTuple::removeLast(ProtoContext* context, const int count)
    {
        return toImpl<ProtoTupleImplementation>(this)->implRemoveLast(context, count)->implAsObject(context);
    }

    ProtoObject* ProtoTuple::removeAt(ProtoContext* context, const int index)
    {
        return toImpl<ProtoTupleImplementation>(this)->implRemoveAt(context, index)->implAsObject(context);
    }

    ProtoObject* ProtoTuple::removeSlice(ProtoContext* context, const int from, const int to)
    {
        return toImpl<ProtoTupleImplementation>(this)->implRemoveSlice(context, from, to)->implAsObject(context);
    }

    ProtoList* ProtoTuple::asList(ProtoContext* context)
    {
        return toImpl<ProtoTupleImplementation>(this)->implAsList(context);
    }

    ProtoObject* ProtoTuple::asObject(ProtoContext* context)
    {
        return toImpl<ProtoTupleImplementation>(this)->implAsObject(context);
    }

    ProtoTupleIterator* ProtoTuple::getIterator(ProtoContext* context)
    {
        return toImpl<ProtoTupleImplementation>(this)->implGetIterator(context);
    }

    // ------------------- ProtoStringIterator -------------------

    int ProtoStringIterator::hasNext(ProtoContext* context)
    {
        return toImpl<ProtoStringIteratorImplementation>(this)->implHasNext(context);
    }

    ProtoObject* ProtoStringIterator::next(ProtoContext* context)
    {
        return toImpl<ProtoStringIteratorImplementation>(this)->implNext(context);
    }

    ProtoStringIterator* ProtoStringIterator::advance(ProtoContext* context)
    {
        return toImpl<ProtoStringIteratorImplementation>(this)->implAdvance(context);
    }

    ProtoObject* ProtoStringIterator::asObject(ProtoContext* context)
    {
        return toImpl<ProtoStringIteratorImplementation>(this)->implAsObject(context);
    }

    // ------------------- ProtoString -------------------

    int ProtoString::cmp_to_string(ProtoContext* context, ProtoString* otherString)
    {
        return toImpl<ProtoStringImplementation>(this)->implCmpToString(context, otherString);
    }

    ProtoObject* ProtoString::getAt(ProtoContext* context, const int index)
    {
        return toImpl<ProtoStringImplementation>(this)->implGetAt(context, index);
    }

    ProtoString* ProtoString::setAt(ProtoContext* context, const int index, ProtoObject* character)
    {
        return toImpl<ProtoStringImplementation>(this)->implSetAt(context, index, character);
    }

    ProtoString* ProtoString::insertAt(ProtoContext* context, const int index, ProtoObject* character)
    {
        return toImpl<ProtoStringImplementation>(this)->implInsertAt(context, index, character);
    }

    unsigned long ProtoString::getSize(ProtoContext* context)
    {
        return toImpl<ProtoStringImplementation>(this)->implGetSize(context);
    }

    ProtoString* ProtoString::getSlice(ProtoContext* context, const int from, const int to)
    {
        return toImpl<ProtoStringImplementation>(this)->implGetSlice(context, from, to);
    }

    ProtoString* ProtoString::setAtString(ProtoContext* context, const int index, ProtoString* otherString)
    {
        return toImpl<ProtoStringImplementation>(this)->implSetAtString(context, index, otherString);
    }

    ProtoString* ProtoString::insertAtString(ProtoContext* context, const int index, ProtoString* otherString)
    {
        return toImpl<ProtoStringImplementation>(this)->implInsertAtString(context, index, otherString);
    }

    ProtoString* ProtoString::appendFirst(ProtoContext* context, ProtoString* otherString)
    {
        return toImpl<ProtoStringImplementation>(this)->implAppendFirst(context, otherString);
    }

    ProtoString* ProtoString::appendLast(ProtoContext* context, ProtoString* otherString)
    {
        return toImpl<ProtoStringImplementation>(this)->implAppendLast(context, otherString);
    }

    ProtoString* ProtoString::splitFirst(ProtoContext* context, const int count)
    {
        return toImpl<ProtoStringImplementation>(this)->implSplitFirst(context, count);
    }

    ProtoString* ProtoString::splitLast(ProtoContext* context, const int count)
    {
        return toImpl<ProtoStringImplementation>(this)->implSplitLast(context, count);
    }

    ProtoString* ProtoString::removeFirst(ProtoContext* context, const int count)
    {
        return toImpl<ProtoStringImplementation>(this)->implRemoveFirst(context, count);
    }

    ProtoString* ProtoString::removeLast(ProtoContext* context, const int count)
    {
        return toImpl<ProtoStringImplementation>(this)->implRemoveLast(context, count);
    }

    ProtoString* ProtoString::removeAt(ProtoContext* context, const int index)
    {
        return toImpl<ProtoStringImplementation>(this)->implRemoveAt(context, index);
    }

    ProtoString* ProtoString::removeSlice(ProtoContext* context, const int from, const int to)
    {
        return toImpl<ProtoStringImplementation>(this)->implRemoveSlice(context, from, to);
    }

    ProtoObject* ProtoString::asObject(ProtoContext* context)
    {
        return toImpl<ProtoStringImplementation>(this)->implAsObject(context);
    }

    ProtoList* ProtoString::asList(ProtoContext* context)
    {
        return toImpl<ProtoStringImplementation>(this)->implAsList(context);
    }

    ProtoStringIterator* ProtoString::getIterator(ProtoContext* context)
    {
        return toImpl<ProtoStringImplementation>(this)->implGetIterator(context);
    }

    // ------------------- ProtoSparseListIterator -------------------

    int ProtoSparseListIterator::hasNext(ProtoContext* context)
    {
        return toImpl<ProtoSparseListIteratorImplementation>(this)->implHasNext(context);
    }

    unsigned long ProtoSparseListIterator::nextKey(ProtoContext* context)
    {
        return toImpl<ProtoSparseListIteratorImplementation>(this)->implNextKey(context);
    }

    ProtoObject* ProtoSparseListIterator::nextValue(ProtoContext* context)
    {
        return toImpl<ProtoSparseListIteratorImplementation>(this)->implNextValue(context);
    }

    ProtoSparseListIterator* ProtoSparseListIterator::advance(ProtoContext* context)
    {
        return toImpl<ProtoSparseListIteratorImplementation>(this)->implAdvance(context);
    }

    ProtoObject* ProtoSparseListIterator::asObject(ProtoContext* context)
    {
        return toImpl<ProtoSparseListIteratorImplementation>(this)->implAsObject(context);
    }

    // ------------------- ProtoSparseList -------------------

    bool ProtoSparseList::has(ProtoContext* context, const unsigned long index)
    {
        return toImpl<ProtoSparseListImplementation>(this)->implHas(context, index);
    }

    ProtoObject* ProtoSparseList::getAt(ProtoContext* context, const unsigned long index)
    {
        return toImpl<ProtoSparseListImplementation>(this)->implGetAt(context, index);
    }

    ProtoSparseList* ProtoSparseList::setAt(ProtoContext* context, const unsigned long index, ProtoObject* value)
    {
        return toImpl<ProtoSparseListImplementation>(this)->implSetAt(context, index, value);
    }

    ProtoSparseList* ProtoSparseList::removeAt(ProtoContext* context, const unsigned long index)
    {
        return toImpl<ProtoSparseListImplementation>(this)->implRemoveAt(context, index);
    }

    bool ProtoSparseList::isEqual(ProtoContext* context, ProtoSparseList* otherDict)
    {
        ProtoObjectPointer p{};
        p.sparseList = otherDict;
        if (p.op.pointer_tag != POINTER_TAG_SPARSE_LIST)
            return false;

        p.op.pointer_tag = POINTER_TAG_OBJECT;
        return toImpl<ProtoSparseListImplementation>(this)->implIsEqual(context,
            toImpl<ProtoSparseListImplementation>(otherDict));
    }

    unsigned long ProtoSparseList::getSize(ProtoContext* context)
    {
        return toImpl<ProtoSparseListImplementation>(this)->implGetSize(context);
    }

    ProtoObject* ProtoSparseList::asObject(ProtoContext* context)
    {
        return toImpl<ProtoSparseListImplementation>(this)->implAsObject(context);
    }

    ProtoSparseListIterator* ProtoSparseList::getIterator(ProtoContext* context)
    {
        return toImpl<ProtoSparseListImplementation>(this)->implGetIterator(context);
    }

    void ProtoSparseList::processElements(ProtoContext* context, void* self,
                                          void (*method)(ProtoContext* context, void* self, unsigned long index,
                                                         ProtoObject* value))
    {
        toImpl<ProtoSparseListImplementation>(this)->implProcessElements(context, self, method);
    }

    void ProtoSparseList::processValues(ProtoContext* context, void* self,
                                        void (*method)(ProtoContext* context, void* self, ProtoObject* value))
    {
        toImpl<ProtoSparseListImplementation>(this)->implProcessValues(context, self, method);
    }

    // ------------------- ProtoByteBuffer -------------------

    unsigned long ProtoByteBuffer::getSize(ProtoContext* context)
    {
        return toImpl<ProtoByteBufferImplementation>(this)->implGetSize(context);
    }

    char* ProtoByteBuffer::getBuffer(ProtoContext* context)
    {
        return toImpl<ProtoByteBufferImplementation>(this)->implGetBuffer(context);
    }

    char ProtoByteBuffer::getAt(ProtoContext* context, const int index)
    {
        return toImpl<ProtoByteBufferImplementation>(this)->implGetAt(context, index);
    }

    void ProtoByteBuffer::setAt(ProtoContext* context, const int index, const char value)
    {
        toImpl<ProtoByteBufferImplementation>(this)->implSetAt(context, index, value);
    }

    ProtoObject* ProtoByteBuffer::asObject(ProtoContext* context)
    {
        return toImpl<ProtoByteBufferImplementation>(this)->implAsObject(context);
    }

    // ------------------- ProtoExternalPointer -------------------

    void* ProtoExternalPointer::getPointer(ProtoContext* context)
    {
        return toImpl<ProtoExternalPointerImplementation>(this)->implGetPointer(context);
    }

    ProtoObject* ProtoExternalPointer::asObject(ProtoContext* context)
    {
        return toImpl<ProtoExternalPointerImplementation>(this)->implAsObject(context);
    }

    // ------------------- ProtoThread -------------------

    ProtoThread* ProtoThread::getCurrentThread(ProtoContext* context)
    {
        return ProtoThreadImplementation::implGetCurrentThread(context);
    }

    void ProtoThread::detach(ProtoContext* context)
    {
        toImpl<ProtoThreadImplementation>(this)->implDetach(context);
    }

    void ProtoThread::join(ProtoContext* context)
    {
        toImpl<ProtoThreadImplementation>(this)->implJoin(context);
    }

    void ProtoThread::exit(ProtoContext* context)
    {
        toImpl<ProtoThreadImplementation>(this)->implExit(context);
    }

    ProtoObject* ProtoThread::getName(ProtoContext* context)
    {
        return reinterpret_cast<ProtoObject*>(toImpl<ProtoThreadImplementation>(this)->name);
    }

    ProtoObject* ProtoThread::asObject(ProtoContext* context)
    {
        return toImpl<ProtoThreadImplementation>(this)->implAsObject(context);
    }

    void ProtoThread::setCurrentContext(ProtoContext* context)
    {
        toImpl<ProtoThreadImplementation>(this)->implSetCurrentContext(context);
    }

    void ProtoThread::setManaged()
    {
        toImpl<ProtoThreadImplementation>(this)->implSetManaged();
    }

    void ProtoThread::setUnmanaged()
    {
        toImpl<ProtoThreadImplementation>(this)->implSetUnmanaged();
    }

    void ProtoThread::synchToGC()
    {
        toImpl<ProtoThreadImplementation>(this)->implSynchToGC();
    }
} // namespace proto
