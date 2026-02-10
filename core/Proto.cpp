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
    unsigned long generate_mutable_ref(ProtoContext* context) {
        return context->space->nextMutableRef++;
    }

    const ProtoObject* ProtoObject::getPrototype(ProtoContext* context) const
    {
        ProtoObjectPointer pa{};
        pa.oid = this;

        switch (pa.op.pointer_tag)
        {
        case POINTER_TAG_OBJECT: return context->space->objectPrototype;
        case POINTER_TAG_EMBEDDED_VALUE:
            switch (pa.op.embedded_type)
            {
            case EMBEDDED_TYPE_SMALLINT: return context->space->smallIntegerPrototype;
            case EMBEDDED_TYPE_BOOLEAN: return context->space->booleanPrototype;
            case EMBEDDED_TYPE_UNICODE_CHAR: return context->space->unicodeCharPrototype;
            case EMBEDDED_TYPE_INLINE_STRING: return context->space->stringPrototype;
            default: return context->space->objectPrototype; // Fallback for unknown embedded types
            }
        case POINTER_TAG_LIST: return context->space->listPrototype;
        case POINTER_TAG_LIST_ITERATOR: return context->space->listIteratorPrototype;
        case POINTER_TAG_SPARSE_LIST: return context->space->sparseListPrototype;
        case POINTER_TAG_SPARSE_LIST_ITERATOR: return context->space->sparseListIteratorPrototype;
        case POINTER_TAG_TUPLE: return context->space->tuplePrototype;
        case POINTER_TAG_TUPLE_ITERATOR: return context->space->tupleIteratorPrototype;
        case POINTER_TAG_STRING: return context->space->stringPrototype;
        case POINTER_TAG_STRING_ITERATOR: return context->space->stringIteratorPrototype;
        case POINTER_TAG_SET: return context->space->setPrototype;
        case POINTER_TAG_SET_ITERATOR: return context->space->setIteratorPrototype;
        case POINTER_TAG_MULTISET: return context->space->multisetPrototype;
        case POINTER_TAG_MULTISET_ITERATOR: return context->space->multisetIteratorPrototype;
        case POINTER_TAG_BYTE_BUFFER: return context->space->bufferPrototype;
        case POINTER_TAG_EXTERNAL_POINTER: return context->space->pointerPrototype;
        case POINTER_TAG_EXTERNAL_BUFFER: return context->space->pointerPrototype;
        case POINTER_TAG_METHOD: return context->space->methodPrototype;
        case POINTER_TAG_THREAD: return context->space->threadPrototype;
        case POINTER_TAG_LARGE_INTEGER: return context->space->largeIntegerPrototype;
        case POINTER_TAG_DOUBLE: return context->space->doublePrototype;
        default: return context->space->objectPrototype; // Default for unknown tags
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
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) {
             // If not an object, get its prototype and create a child of that
             const ProtoObject* prototype = getPrototype(context);
             return prototype ? prototype->newChild(context, isMutable) : PROTO_NONE;
        }
        auto* oc = toImpl<const ProtoObjectCell>(this);
        auto* newObject = new(context) ProtoObjectCell(context, new(context) ParentLinkImplementation(context, oc->parent, this), toImpl<const ProtoSparseListImplementation>(context->newSparseList()), isMutable ? generate_mutable_ref(context) : 0);
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
        const ParentLinkImplementation* plStack[64];
        int plPtr = 0;
        const ProtoObject* current = this;

        while (current) {
            if (current == prototype) return PROTO_TRUE;
            ProtoObjectPointer pa{};
            pa.oid = current;
            if (pa.op.pointer_tag != POINTER_TAG_OBJECT) {
                current = current->getPrototype(context);
                continue;
            }
            auto oc = toImpl<const ProtoObjectCell>(current);
            
            // Handle Mutable Objects
            if (oc->mutable_ref > 0) {
                 ProtoSparseList* root = context->space->mutableRoot.load();
                 if (root != nullptr) {
                      const auto* mutableList = toImpl<const ProtoSparseListImplementation>(root);
                      const proto::ProtoObject* storedState = mutableList->implGetAt(context, oc->mutable_ref);
                      if (storedState != PROTO_NONE) {
                          ProtoObjectPointer psa{};
                          psa.oid = storedState;
                          if (psa.op.pointer_tag == POINTER_TAG_OBJECT) {
                              oc = toImpl<const ProtoObjectCell>(storedState);
                          }
                      }
                 }
            }
            
            if (oc->parent) {
                const ParentLinkImplementation* sibling = oc->parent->getParent(context);
                while (sibling && plPtr < 64) {
                    plStack[plPtr++] = sibling;
                    sibling = sibling->getParent(context);
                }
                current = oc->parent->getObject(context);
            } else {
                if (plPtr > 0) {
                    const ParentLinkImplementation* top = plStack[--plPtr];
                    current = top->getObject(context);
                } else {
                    current = nullptr;
                }
            }
        }
        return PROTO_FALSE;
    }

    const ProtoObject* ProtoObject::getAttribute(ProtoContext* context, const ProtoString* name) const
    {
        // 1. Inline Cache Lookup
        AttributeCacheEntry* cache = nullptr;
        unsigned long hash_idx = 0;
        if (context->thread) {
            auto* threadImpl = toImpl<ProtoThreadImplementation>(context->thread);
            if (threadImpl->extension) {
                cache = threadImpl->extension->attributeCache;
                hash_idx = (reinterpret_cast<uintptr_t>(this) ^ name->getHash(context)) % THREAD_CACHE_DEPTH;
                if (cache[hash_idx].object == this && cache[hash_idx].name == name) {
                    return cache[hash_idx].result;
                }
            }
        }

        const ProtoObject* currentObject = this;
        const unsigned long attr_hash = reinterpret_cast<uintptr_t>(name);

        const ParentLinkImplementation* plStack[64];
        int plPtr = 0;

        while (currentObject) {

            ProtoObjectPointer pa_cur{};
            pa_cur.oid = currentObject;
            if (pa_cur.op.pointer_tag != POINTER_TAG_OBJECT) {
                currentObject = currentObject->getPrototype(context);
                continue;
            }
            auto oc = toImpl<const ProtoObjectCell>(currentObject);
            const ProtoSparseListImplementation* attributes = oc->attributes;

            // Check if this object is a mutable reference and has an updated state
            if (oc->mutable_ref > 0) {
                 ProtoSparseList* root = context->space->mutableRoot.load();
                 if (root != nullptr) {
                      const auto* mutableList = toImpl<const ProtoSparseListImplementation>(root);
                      const proto::ProtoObject* storedState = mutableList->implGetAt(context, oc->mutable_ref);
                      if (storedState != PROTO_NONE) {
                          ProtoObjectPointer psa{};
                          psa.oid = storedState;
                          if (psa.op.pointer_tag == POINTER_TAG_OBJECT) {
                              auto* storedOc = toImpl<const ProtoObjectCell>(storedState);
                              attributes = storedOc->attributes;
                              oc = storedOc;
                          }
                      }
                 }
            }

            if (attributes->implHas(context, attr_hash)) {
                const auto* result = attributes->implGetAt(context, attr_hash);
                // Update Cache
                if (cache) {
                    cache[hash_idx] = {this, result, name};
                }
                return result;
            }
            
            // Multiple inheritance support:
            // Follow first parent vertically, push siblings to stack for later.
            if (oc->parent) {
                const ParentLinkImplementation* sibling = oc->parent->getParent(context);
                while (sibling && plPtr < 64) {
                    plStack[plPtr++] = sibling;
                    sibling = sibling->getParent(context);
                }
                currentObject = oc->parent->getObject(context);
            } else {
                if (plPtr > 0) {
                    const ParentLinkImplementation* top = plStack[--plPtr];
                    currentObject = top->getObject(context);
                } else {
                    currentObject = nullptr;
                }
            }
        }
        if (context->space->attributeNotFoundGetCallback) {
            return (*context->space->attributeNotFoundGetCallback)(context, this, name);
        }
        return PROTO_NONE;
    }

    const ProtoObject* ProtoObject::setAttribute(ProtoContext* context, const ProtoString* name, const ProtoObject* value) const
    {
        // 1. Invalidate Cache
        if (context->thread) {
            auto* threadImpl = toImpl<ProtoThreadImplementation>(context->thread);
            if (threadImpl->extension) {
                unsigned long hash_idx = (reinterpret_cast<uintptr_t>(this) ^ name->getHash(context)) % THREAD_CACHE_DEPTH;
                if (threadImpl->extension->attributeCache[hash_idx].object == this &&
                    threadImpl->extension->attributeCache[hash_idx].name == name) {
                    threadImpl->extension->attributeCache[hash_idx] = {nullptr, nullptr, nullptr};
                }
            }
        }

        ProtoObjectPointer pa{};
        pa.oid = this;
        if (std::getenv("PROTO_ENV_DIAG")) {
             std::cerr << "[proto-mutable-diag] setAttribute tag=" << pa.op.pointer_tag << " obj=" << this << "\n" << std::flush;
        }
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return this;
        auto* oc = toImpl<ProtoObjectCell>(this);
        if (std::getenv("PROTO_ENV_DIAG")) {
             std::cerr << "[proto-mutable-diag] setAttribute oc=" << oc << " ref=" << oc->mutable_ref << "\n" << std::flush;
        }

        // Handle Mutable Objects
        if (oc->mutable_ref > 0) {
             // 1. Get current object state
             const ProtoObject* currentObjState = this; 
             ProtoSparseList* root = context->space->mutableRoot.load();
             if (root != nullptr) {
                 const auto* currentMutableList = toImpl<const ProtoSparseListImplementation>(root);
                 const ProtoObject* storedState = currentMutableList->implGetAt(context, oc->mutable_ref);
                 if (storedState != PROTO_NONE) {
                      currentObjState = storedState;
                 }
             }

             // 3. Create new state with updated attribute
             auto* currentOc = toImpl<const ProtoObjectCell>(currentObjState);
             const auto* newAttributes = currentOc->attributes->implSetAt(context, reinterpret_cast<uintptr_t>(name), value);
             auto* newState = (new(context) ProtoObjectCell(context, currentOc->parent, newAttributes, oc->mutable_ref))->asObject(context);

             // 4. Update mutableRoot (Compare and Swap Loop)
             while(true) {
                 ProtoSparseList* oldRoot = context->space->mutableRoot.load();
                 const auto* oldRootImpl = (oldRoot == nullptr) ? 
                     toImpl<const ProtoSparseListImplementation>(context->newSparseList()) :
                     toImpl<const ProtoSparseListImplementation>(oldRoot);
                 
                 auto* newRootImpl = oldRootImpl->implSetAt(context, oc->mutable_ref, newState);
                 ProtoSparseList* expected = oldRoot;
                 if (context->space->mutableRoot.compare_exchange_weak(expected, const_cast<ProtoSparseList*>(newRootImpl->asSparseList(context)))) {
                     break;
                 }
             }

             return this; // Return the same handle!
        }

        // Handle Immutable Objects (Copy-on-Write)
        const auto* newAttributes = oc->attributes->implSetAt(context, reinterpret_cast<uintptr_t>(name), value);
        return (new(context) ProtoObjectCell(context, oc->parent, newAttributes, 0))->asObject(context);
    }

    const ProtoList* ProtoObject::getParents(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return context->newList();
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
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT || !newParent->isCell(context)) return this;
        const auto* oc = toImpl<const ProtoObjectCell>(this);
        
        // Handle Mutable Objects
        if (oc->mutable_ref > 0) {
             // 1. Get current object state
             const ProtoObject* currentObjState = this; 
             ProtoSparseList* root = context->space->mutableRoot.load();
             if (root != nullptr) {
                 const auto* currentMutableList = toImpl<const ProtoSparseListImplementation>(root);
                 const ProtoObject* storedState = currentMutableList->implGetAt(context, oc->mutable_ref);
                 if (storedState != PROTO_NONE) {
                      currentObjState = storedState;
                 }
             }

             // 2. Create new state with added parent
             auto* currentOc = toImpl<const ProtoObjectCell>(currentObjState);
             auto* newState = currentOc->addParent(context, newParent)->asObject(context);

             // 3. Update mutableRoot (Compare and Swap Loop)
             while(true) {
                 ProtoSparseList* oldRoot = context->space->mutableRoot.load();
                 const auto* oldRootImpl = (oldRoot == nullptr) ? 
                     toImpl<const ProtoSparseListImplementation>(context->newSparseList()) :
                     toImpl<const ProtoSparseListImplementation>(oldRoot);
                 
                 auto* newRootImpl = oldRootImpl->implSetAt(context, oc->mutable_ref, newState);
                 ProtoSparseList* expected = oldRoot;
                 if (context->space->mutableRoot.compare_exchange_weak(expected, const_cast<ProtoSparseList*>(newRootImpl->asSparseList(context)))) {
                     break;
                 }
             }

             return this; // Return the same handle
        }
        
        return oc->addParent(context, newParent)->asObject(context);
    }

    int ProtoObject::isCell(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        // In this runtime, "isCell" typically means "is it a ProtoObjectCell"
        // (tag 0). Other cell types (methods, lists, etc.) should use their
        // specific check methods or handles.
        return pa.op.pointer_tag == POINTER_TAG_OBJECT;
    }

    const Cell* ProtoObject::asCell(ProtoContext* context) const {
        if (isCell(context)) {
            ProtoObjectPointer pa{};
            pa.oid = this;
            // Clear the tag bits (lowest 6 bits) to get the raw pointer to the Cell
            uintptr_t raw_ptr_value = reinterpret_cast<uintptr_t>(pa.oid) & ~0x3FUL;
            return reinterpret_cast<const Cell*>(raw_ptr_value);
        }
        return nullptr;
    }

    const ProtoString* ProtoObject::asString(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_STRING) return reinterpret_cast<const ProtoString*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_INLINE_STRING) return reinterpret_cast<const ProtoString*>(this);
        return nullptr;
    }

    ProtoMethod ProtoObject::asMethod(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_METHOD ? toImpl<const ProtoMethodCell>(this)->method : nullptr;
    }

    bool ProtoObject::isNone(ProtoContext* context) const { return this == PROTO_NONE; }
    bool ProtoObject::isBoolean(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_BOOLEAN; }
    bool ProtoObject::isInteger(ProtoContext* context) const { return proto::isInteger(this); }
    bool ProtoObject::isString(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_STRING || (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_INLINE_STRING);
    }
    bool ProtoObject::isMethod(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_METHOD; }
    bool ProtoObject::isTuple(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_TUPLE; }
    bool ProtoObject::isSet(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_SET; }
    bool ProtoObject::isMultiset(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_MULTISET; }

    unsigned long ProtoObject::getHash(ProtoContext* context) const {
        if (isString(context) && isInlineString(this)) return getProtoStringHash(context, this);
        if (isCell(context)) return asCell(context)->getHash(context);
        return reinterpret_cast<uintptr_t>(this);
    }

    const ProtoTuple* ProtoObject::asTuple(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_TUPLE) return reinterpret_cast<const ProtoTuple*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = proto::ProtoString::fromUTF8String(context, "__data__");
            const proto::ProtoObject* data = this->getAttribute(context, dataName);
            if (data && data != this) return data->asTuple(context);
        }
        return nullptr;
    }
    const ProtoSet* ProtoObject::asSet(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_SET ? reinterpret_cast<const ProtoSet*>(this) : nullptr; }
    const ProtoSetIterator* ProtoObject::asSetIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_SET_ITERATOR ? reinterpret_cast<const ProtoSetIterator*>(this) : nullptr; }
    const ProtoMultiset* ProtoObject::asMultiset(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_MULTISET ? reinterpret_cast<const ProtoMultiset*>(this) : nullptr; }
    const ProtoMultisetIterator* ProtoObject::asMultisetIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_MULTISET_ITERATOR ? reinterpret_cast<const ProtoMultisetIterator*>(this) : nullptr; }
    const ProtoThread* ProtoObject::asThread(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_THREAD ? reinterpret_cast<const ProtoThread*>(this) : nullptr; }
    const ProtoListIterator* ProtoObject::asListIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_LIST_ITERATOR ? reinterpret_cast<const ProtoListIterator*>(this) : nullptr; }
    const ProtoTupleIterator* ProtoObject::asTupleIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_TUPLE_ITERATOR ? reinterpret_cast<const ProtoTupleIterator*>(this) : nullptr; }
    const ProtoStringIterator* ProtoObject::asStringIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_STRING_ITERATOR ? reinterpret_cast<const ProtoStringIterator*>(this) : nullptr; }
    const ProtoSparseList* ProtoObject::asSparseList(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_SPARSE_LIST) return reinterpret_cast<const ProtoSparseList*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = proto::ProtoString::fromUTF8String(context, "__data__");
            const proto::ProtoObject* data = this->getAttribute(context, dataName);
            if (data && data != this) return data->asSparseList(context);
        }
        return nullptr;
    }
    const ProtoExternalPointer* ProtoObject::asExternalPointer(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_EXTERNAL_POINTER ? reinterpret_cast<const ProtoExternalPointer*>(this) : nullptr; }
    const ProtoExternalBuffer* ProtoObject::asExternalBuffer(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_EXTERNAL_BUFFER ? reinterpret_cast<const ProtoExternalBuffer*>(this) : nullptr; }
    void* ProtoObject::getRawPointerIfExternalBuffer(ProtoContext* context) const {
        const ProtoExternalBuffer* buf = asExternalBuffer(context);
        return buf ? reinterpret_cast<const ProtoExternalBuffer*>(buf)->getRawPointer(context) : nullptr;
    }
    const ProtoSparseListIterator* ProtoObject::asSparseListIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_SPARSE_LIST_ITERATOR ? reinterpret_cast<const ProtoSparseListIterator*>(this) : nullptr; }
    const ProtoList* ProtoObject::asList(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_LIST) return reinterpret_cast<const ProtoList*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_STRING) return toImpl<const ProtoStringImplementation>(this)->implAsList(context);
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_INLINE_STRING) return reinterpret_cast<const ProtoString*>(this)->asList(context);
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = proto::ProtoString::fromUTF8String(context, "__data__");
            const proto::ProtoObject* data = this->getAttribute(context, dataName);
            if (data && data != this) return data->asList(context);
        }
        return nullptr;
    }

    long long ProtoObject::asLong(ProtoContext* context) const { return Integer::asLong(context, this); }
    bool ProtoObject::asBoolean(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_BOOLEAN) {
            return pa.booleanValue.booleanValue;
        }
        // If it's not a boolean, perhaps throw an error or return a default.
        // For now, let's assume it's only called on actual booleans.
        return false; // Should not be reached if isBoolean() is checked first.
    }
    char ProtoObject::asByte(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.byteValue.byteData; }
    double ProtoObject::asDouble(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_DOUBLE) {
            return toImpl<const DoubleImplementation>(this)->doubleValue;
        } else if (isInteger(context)) {
            return static_cast<double>(asLong(context));
        }
        // If it's not a double or an integer, throw an error.
        if (context->space && context->space->invalidConversionCallback)
            (context->space->invalidConversionCallback)(context);
        return 0.0;
    }
    bool ProtoObject::isDouble(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_DOUBLE; }

    const ProtoObject* ProtoObject::add(ProtoContext* context, const ProtoObject* other) const { return Integer::add(context, this, other); }
    const ProtoObject* ProtoObject::subtract(ProtoContext* context, const ProtoObject* other) const { return Integer::subtract(context, this, other); }
    const ProtoObject* ProtoObject::multiply(ProtoContext* context, const ProtoObject* other) const { return Integer::multiply(context, this, other); }
    const ProtoObject* ProtoObject::divide(ProtoContext* context, const ProtoObject* other) const { return Integer::divide(context, this, other); }
    const ProtoObject* ProtoObject::modulo(ProtoContext* context, const ProtoObject* other) const { return Integer::modulo(context, this, other); }
    int ProtoObject::compare(ProtoContext* context, const ProtoObject* other) const {
        if (this->isString(context) && other->isString(context)) {
            return this->asString(context)->cmp_to_string(context, other->asString(context));
        }
        // Only perform double comparison if both are some kind of number
        bool thisIsNum = this->isDouble(context) || this->isInteger(context);
        bool otherIsNum = other->isDouble(context) || other->isInteger(context);
        if (thisIsNum && otherIsNum) {
            if (this->isDouble(context) || other->isDouble(context)) {
                double d1 = this->asDouble(context);
                double d2 = other->asDouble(context);
                return (d1 < d2) ? -1 : (d1 > d2) ? 1 : 0;
            }
            return Integer::compare(context, this, other);
        }
        if (this->isString(context) && other->isString(context)) {
            return this->asString(context)->cmp_to_string(context, other->asString(context));
        }
        return (this < other) ? -1 : (this > other) ? 1 : 0;
    }
    const ProtoObject* ProtoObject::bitwiseNot(ProtoContext* context) const { return Integer::bitwiseNot(context, this); }
    const ProtoObject* ProtoObject::bitwiseAnd(ProtoContext* context, const ProtoObject* other) const { return Integer::bitwiseAnd(context, this, other); }
    const ProtoObject* ProtoObject::bitwiseOr(ProtoContext* context, const ProtoObject* other) const { return Integer::bitwiseOr(context, this, other); }
    const ProtoObject* ProtoObject::shiftLeft(ProtoContext* context, int amount) const { return Integer::shiftLeft(context, this, amount); }
    const ProtoObject* ProtoObject::shiftRight(ProtoContext* context, int amount) const { return Integer::shiftRight(context, this, amount); }

    const ProtoObject* ProtoObject::hasAttribute(ProtoContext* context, const ProtoString* name) const { return context->fromBoolean(getAttribute(context, name) != PROTO_NONE); }
    
    const ProtoSparseList* ProtoObject::getAttributes(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) {
            const ProtoObject* prototype = getPrototype(context);
            return prototype ? prototype->getAttributes(context) : context->newSparseList();
        }
        auto oc = toImpl<const ProtoObjectCell>(this);
        const ProtoSparseListImplementation* attributes = oc->attributes;

        if (oc->mutable_ref > 0) {
            ProtoSparseList* root = context->space->mutableRoot.load();
            if (root != nullptr) {
                const auto* mutableList = toImpl<const ProtoSparseListImplementation>(root);
                const ProtoObject* storedState = mutableList->implGetAt(context, oc->mutable_ref);
                if (storedState != PROTO_NONE) {
                    auto* storedOc = toImpl<const ProtoObjectCell>(storedState);
                    attributes = storedOc->attributes;
                    oc = storedOc;
                }
            }
        }

        const ProtoSparseList* attrs = attributes->asSparseList(context);
        if (oc->parent) {
            const ProtoObject* parentObj = oc->parent->getObject(context);
            if (parentObj) {
                const ProtoSparseList* parentAttrs = parentObj->getAttributes(context);
                // Merge parent attributes with own attributes
                const ProtoSparseListIterator* it = parentAttrs->getIterator(context);
                while (it && it->hasNext(context)) {
                    unsigned long key = it->nextKey(context);
                    const ProtoObject* value = it->nextValue(context);
                    if (!attrs->has(context, key)) {
                        attrs = attrs->setAt(context, key, value);
                    }
                    it = const_cast<ProtoSparseListIterator*>(it)->advance(context);
                }
            }
        }
        return attrs;
    }
    
    const ProtoSparseList* ProtoObject::getOwnAttributes(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return context->newSparseList();
        auto oc = toImpl<const ProtoObjectCell>(this);
        const ProtoSparseListImplementation* attributes = oc->attributes;

        if (oc->mutable_ref > 0) {
            ProtoSparseList* root = context->space->mutableRoot.load();
            if (root != nullptr) {
                const auto* mutableList = toImpl<const ProtoSparseListImplementation>(root);
                const ProtoObject* storedState = mutableList->implGetAt(context, oc->mutable_ref);
                if (storedState != PROTO_NONE) {
                    auto* storedOc = toImpl<const ProtoObjectCell>(storedState);
                    attributes = storedOc->attributes;
                }
            }
        }
        return attributes->asSparseList(context);
    }
    
    const ProtoObject* ProtoObject::hasOwnAttribute(ProtoContext* context, const ProtoString* name) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return PROTO_FALSE;
        auto oc = toImpl<const ProtoObjectCell>(this);
        const ProtoSparseListImplementation* attributes = oc->attributes;

        if (oc->mutable_ref > 0) {
            ProtoSparseList* root = context->space->mutableRoot.load();
            if (root != nullptr) {
                const auto* mutableList = toImpl<const ProtoSparseListImplementation>(root);
                const ProtoObject* storedState = mutableList->implGetAt(context, oc->mutable_ref);
                if (storedState != PROTO_NONE) {
                    auto* storedOc = toImpl<const ProtoObjectCell>(storedState);
                    attributes = storedOc->attributes;
                }
            }
        }
        return context->fromBoolean(attributes->implHas(context, reinterpret_cast<uintptr_t>(name)));
    }
    
    const ProtoObject* ProtoObject::divmod(ProtoContext* context, const ProtoObject* other) const {
        const ProtoList* result = context->newList();
        result = result->appendLast(context, divide(context, other));
        result = result->appendLast(context, modulo(context, other));
        return context->newTupleFromList(result)->asObject(context);
    }
    
    bool ProtoObject::isFloat(ProtoContext* context) const {
        return isDouble(context); // Float and Double are the same in protoCore
    }
    
    bool ProtoObject::isDate(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == 4; // Date type
    }
    
    bool ProtoObject::isTimestamp(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.timestampValue.timestamp != 0;
    }
    
    bool ProtoObject::isTimeDelta(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.timedeltaValue.timedelta != 0;
    }
    
    void ProtoObject::asDate(ProtoContext* context, unsigned int& year, unsigned& month, unsigned& day) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE) {
            year = pa.date.year;
            month = pa.date.month;
            day = pa.date.day;
        } else {
            year = month = day = 0;
        }
    }
    
    unsigned long ProtoObject::asTimestamp(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE) {
            return pa.timestampValue.timestamp;
        }
        return 0;
    }
    
    long ProtoObject::asTimeDelta(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE) {
            return pa.timedeltaValue.timedelta;
        }
        return 0;
    }
    
    const ProtoObject* ProtoObject::negate(ProtoContext* context) const {
        return subtract(context, context->fromInteger(0));
    }
    
    const ProtoObject* ProtoObject::abs(ProtoContext* context) const {
        if (isInteger(context)) {
            long long val = asLong(context);
            return context->fromLong(val < 0 ? -val : val);
        } else if (isDouble(context)) {
            double val = asDouble(context);
            return context->fromDouble(val < 0 ? -val : val);
        }
        return this;
    }
    
    const ProtoObject* ProtoObject::bitwiseXor(ProtoContext* context, const ProtoObject* other) const {
        return Integer::bitwiseXor(context, this, other);
    }


    //=========================================================================
    // ProtoList API Implementation
    //=========================================================================
    unsigned long ProtoList::getSize(ProtoContext* context) const { return toImpl<const ProtoListImplementation>(this)->size; }
    const ProtoObject* ProtoList::getAt(ProtoContext* context, int index) const { return toImpl<const ProtoListImplementation>(this)->implGetAt(context, index); }
    const ProtoObject* ProtoList::getFirst(ProtoContext* context) const {
        unsigned long size = getSize(context);
        if (size == 0) return PROTO_NONE;
        return getAt(context, 0);
    }
    const ProtoObject* ProtoList::getLast(ProtoContext* context) const {
        unsigned long size = getSize(context);
        if (size == 0) return PROTO_NONE;
        return getAt(context, size - 1);
    }
    bool ProtoList::has(ProtoContext* context, const ProtoObject* value) const { return toImpl<const ProtoListImplementation>(this)->implHas(context, value); }
    const ProtoList* ProtoList::setAt(ProtoContext* context, int index, const ProtoObject* value) const { return toImpl<const ProtoListImplementation>(this)->implSetAt(context, index, value)->asProtoList(context); }
    const ProtoList* ProtoList::insertAt(ProtoContext* context, int index, const ProtoObject* value) const { return toImpl<const ProtoListImplementation>(this)->implInsertAt(context, index, value)->asProtoList(context); }
    const ProtoList* ProtoList::appendFirst(ProtoContext* context, const ProtoObject* value) const { return insertAt(context, 0, value); }
    const ProtoList* ProtoList::appendLast(ProtoContext* context, const ProtoObject* value) const { return toImpl<const ProtoListImplementation>(this)->implAppendLast(context, value)->asProtoList(context); }
    const ProtoList* ProtoList::extend(ProtoContext* context, const ProtoList* other) const {
        const ProtoList* result = const_cast<ProtoList*>(this);
        const ProtoListIterator* it = other->getIterator(context);
        while (it->hasNext(context)) {
            result = result->appendLast(context, it->next(context));
        }
        return result;
    }
    const ProtoList* ProtoList::splitFirst(ProtoContext* context, int index) const {
        unsigned long size = getSize(context);
        if (index <= 0) return context->newList();
        if (index >= (int)size) return const_cast<ProtoList*>(this);
        return getSlice(context, 0, index);
    }
    const ProtoList* ProtoList::splitLast(ProtoContext* context, int index) const {
        unsigned long size = getSize(context);
        if (index <= 0) return context->newList();
        if (index >= (int)size) return const_cast<ProtoList*>(this);
        return getSlice(context, size - index, size);
    }
    const ProtoList* ProtoList::removeFirst(ProtoContext* context) const {
        unsigned long size = getSize(context);
        if (size == 0) return const_cast<ProtoList*>(this);
        return removeAt(context, 0);
    }
    const ProtoList* ProtoList::removeLast(ProtoContext* context) const {
        unsigned long size = getSize(context);
        if (size == 0) return const_cast<ProtoList*>(this);
        return removeAt(context, size - 1);
    }
    const ProtoList* ProtoList::removeAt(ProtoContext* context, int index) const { return toImpl<const ProtoListImplementation>(this)->implRemoveAt(context, index)->asProtoList(context); }
    const ProtoList* ProtoList::removeSlice(ProtoContext* context, int from, int to) const {
        const ProtoList* result = const_cast<ProtoList*>(this);
        // Remove from end to start to preserve indices
        for (int i = to - 1; i >= from; --i) {
            result = result->removeAt(context, i);
        }
        return result;
    }
    const ProtoList* ProtoList::getSlice(ProtoContext* context, int start, int end) const {
        const ProtoList* list = context->newList();
        for (int i = start; i < end; ++i) {
            list = list->appendLast(context, getAt(context, i));
        }
        return list;
    }
    const ProtoObject* ProtoList::asObject(ProtoContext* context) const { return toImpl<const ProtoListImplementation>(this)->implAsObject(context); }
    const ProtoListIterator* ProtoList::getIterator(ProtoContext* context) const { return toImpl<const ProtoListImplementation>(this)->implGetIterator(context)->asProtoListIterator(context); }
    unsigned long ProtoList::getHash(ProtoContext* context) const { return toImpl<const ProtoListImplementation>(this)->getHash(context); }

    //=========================================================================
    // ProtoListIterator API Implementation
    //=========================================================================
    int ProtoListIterator::hasNext(ProtoContext* context) const { return toImpl<const ProtoListIteratorImplementation>(this)->implHasNext(); }
    const ProtoObject* ProtoListIterator::next(ProtoContext* context) const { return toImpl<const ProtoListIteratorImplementation>(this)->implNext(context); }
    const ProtoListIterator* ProtoListIterator::advance(ProtoContext* context) const { return toImpl<const ProtoListIteratorImplementation>(this)->implAdvance(context)->asProtoListIterator(context); }
    const ProtoObject* ProtoListIterator::asObject(ProtoContext* context) const { return toImpl<const ProtoListIteratorImplementation>(this)->implAsObject(context); }


    //=========================================================================
    // ProtoTuple API Implementation
    //=========================================================================
    const ProtoObject* ProtoTuple::getFirst(ProtoContext* context) const {
        unsigned long size = getSize(context);
        if (size == 0) return PROTO_NONE;
        return getAt(context, 0);
    }
    
    const ProtoObject* ProtoTuple::getLast(ProtoContext* context) const {
        unsigned long size = getSize(context);
        if (size == 0) return PROTO_NONE;
        return getAt(context, size - 1);
    }
    
    bool ProtoTuple::has(ProtoContext* context, const ProtoObject* value) const {
        unsigned long size = getSize(context);
        for (unsigned long i = 0; i < size; ++i) {
            const ProtoObject* elem = getAt(context, i);
            if (elem == value) {
                return true;
            }
            if (elem->isInteger(context) && value->isInteger(context)) {
                if (Integer::compare(context, elem, value) == 0) return true;
            } else if (elem->isString(context) && value->isString(context)) {
                if (elem->asString(context)->cmp_to_string(context, value->asString(context)) == 0) return true;
            }
        }
        return false;
    }

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
    const ProtoSparseListIterator* ProtoSparseListIterator::advance(ProtoContext* context) {
        const auto* nextImpl = toImpl<const ProtoSparseListIteratorImplementation>(this)->implAdvance(context);
        return nextImpl ? reinterpret_cast<const ProtoSparseListIterator*>(nextImpl->implAsObject(context)) : nullptr;
    }
    const ProtoObject* ProtoSparseListIterator::asObject(ProtoContext* context) const { return toImpl<const ProtoSparseListIteratorImplementation>(this)->implAsObject(context); }


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
        const auto* impl = toImpl<const ProtoMultisetImplementation>(this);
        const auto* current_list = impl->list;
        unsigned long hash = value->getHash(context);
        const ProtoObject* existing = current_list->getAt(context, hash);
        long long count = existing ? existing->asLong(context) : 0;
        
        const auto* new_list = current_list->setAt(context, hash, context->fromInteger(count + 1));
        return (new (context) ProtoMultisetImplementation(context, new_list, impl->size + 1))->asProtoMultiset(context);
    }

    const ProtoObject* ProtoMultiset::count(ProtoContext* context, const ProtoObject* value) const {
        const auto* current_list = toImpl<const ProtoMultisetImplementation>(this)->list;
        const ProtoObject* existing = current_list->getAt(context, value->getHash(context));
        return existing ? existing : context->fromInteger(0);
    }

    const ProtoMultiset* ProtoMultiset::remove(ProtoContext* context, const ProtoObject* value) const {
        const auto* impl = toImpl<const ProtoMultisetImplementation>(this);
        const auto* current_list = impl->list;
        unsigned long hash = value->getHash(context);
        const ProtoObject* existing = current_list->getAt(context, hash);
        if (!existing) return this;
        
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
        return (new (context) ProtoMultisetIteratorImplementation(context, toImpl<const ProtoSparseListIteratorImplementation>(list_iterator)))->asMultisetIterator(context);
    }

    //=========================================================================
    // ProtoString API
    //=========================================================================
    const ProtoString* ProtoString::fromUTF8String(ProtoContext* context, const char* str) {
        const ProtoObject* o = context->fromUTF8String(str);
        return o->asString(context);
    }
    unsigned long ProtoString::getHash(ProtoContext* context) const { return getProtoStringHash(context, reinterpret_cast<const ProtoObject*>(this)); }
    const Cell* ProtoString::asCell(ProtoContext* context) const { return isInlineString(reinterpret_cast<const ProtoObject*>(this)) ? nullptr : toImpl<const ProtoStringImplementation>(this); }
    const ProtoString* ProtoString::appendLast(ProtoContext* context, const ProtoString* other) const {
        if (isInlineString(reinterpret_cast<const ProtoObject*>(this))) {
            const ProtoObject* leftObj = reinterpret_cast<const ProtoObject*>(this);
            const ProtoObject* rightObj = other->asObject(context);
            const unsigned long leftSize = getSize(context);
            const unsigned long rightSize = other->getSize(context);
            const ProtoTupleImplementation* concatTuple = ProtoTupleImplementation::tupleConcat(context, leftObj, rightObj, leftSize + rightSize);
            return (new (context) ProtoStringImplementation(context, concatTuple))->asProtoString(context);
        }
        return toImpl<const ProtoStringImplementation>(this)->implAppendLast(context, other)->asProtoString(context);
    }

    //=========================================================================
    // ProtoTupleIterator API Implementation
    //=========================================================================
    int ProtoTupleIterator::hasNext(ProtoContext* context) const { return toImpl<const ProtoTupleIteratorImplementation>(this)->implHasNext(context); }
    const ProtoObject* ProtoTupleIterator::next(ProtoContext* context) { return toImpl<ProtoTupleIteratorImplementation>(this)->implNext(context); }
    const ProtoTupleIterator* ProtoTupleIterator::advance(ProtoContext* context) { return toImpl<ProtoTupleIteratorImplementation>(this)->implAdvance(context)->asProtoTupleIterator(context); }
    const ProtoObject* ProtoTupleIterator::asObject(ProtoContext* context) const { return toImpl<const ProtoTupleIteratorImplementation>(this)->implAsObject(context); }

    //=========================================================================
    // ProtoStringIterator API Implementation
    //=========================================================================
    int ProtoStringIterator::hasNext(ProtoContext* context) const { return toImpl<const ProtoStringIteratorImplementation>(this)->implHasNext(context); }
    const ProtoObject* ProtoStringIterator::next(ProtoContext* context) { return toImpl<ProtoStringIteratorImplementation>(this)->implNext(context); }
    const ProtoStringIterator* ProtoStringIterator::advance(ProtoContext* context) { return toImpl<ProtoStringIteratorImplementation>(this)->implAdvance(context)->asProtoStringIterator(context); }
    const ProtoObject* ProtoStringIterator::asObject(ProtoContext* context) const { return toImpl<const ProtoStringIteratorImplementation>(this)->implAsObject(context); }

    //=========================================================================
    // ProtoSetIterator API Implementation
    //=========================================================================
    int ProtoSetIterator::hasNext(ProtoContext* context) const { return toImpl<const ProtoSetIteratorImplementation>(this)->implHasNext(context); }
    const ProtoObject* ProtoSetIterator::next(ProtoContext* context) const { return const_cast<ProtoSetIteratorImplementation*>(toImpl<const ProtoSetIteratorImplementation>(this))->implNext(context); }
    const ProtoSetIterator* ProtoSetIterator::advance(ProtoContext* context) const {
        const auto* impl = toImpl<const ProtoSetIteratorImplementation>(this);
        const ProtoSparseListIteratorImplementation* advanced = const_cast<ProtoSparseListIteratorImplementation*>(impl->iterator)->implAdvance(context);
        return (new (context) ProtoSetIteratorImplementation(context, advanced))->asSetIterator(context);
    }
    const ProtoObject* ProtoSetIterator::asObject(ProtoContext* context) const { return toImpl<const ProtoSetIteratorImplementation>(this)->implAsObject(context); }

    //=========================================================================
    // ProtoMultisetIterator API Implementation
    //=========================================================================
    int ProtoMultisetIterator::hasNext(ProtoContext* context) const { return toImpl<const ProtoMultisetIteratorImplementation>(this)->implHasNext(context); }
    const ProtoObject* ProtoMultisetIterator::next(ProtoContext* context) const { return const_cast<ProtoMultisetIteratorImplementation*>(toImpl<const ProtoMultisetIteratorImplementation>(this))->implNext(context); }
    const ProtoMultisetIterator* ProtoMultisetIterator::advance(ProtoContext* context) const {
        const auto* impl = toImpl<const ProtoMultisetIteratorImplementation>(this);
        const ProtoSparseListIteratorImplementation* advanced = const_cast<ProtoSparseListIteratorImplementation*>(impl->iterator)->implAdvance(context);
        return (new (context) ProtoMultisetIteratorImplementation(context, advanced))->asMultisetIterator(context);
    }
    const ProtoObject* ProtoMultisetIterator::asObject(ProtoContext* context) const { return toImpl<const ProtoMultisetIteratorImplementation>(this)->implAsObject(context); }

}
