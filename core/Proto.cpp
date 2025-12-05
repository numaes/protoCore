/*
 * Proto.cpp
 *
 *  Created on: 2020-5-1
 *      Author: gamarino
 *
 *  This file implements the public-facing API of the Proto library.
 *
 *  It serves as the "bridge" between the public API classes (e.g., `ProtoObject`,
 *  `ProtoList`), which are defined in `protoCore.h`, and their internal implementation
 *  classes (e.g., `ProtoObjectCell`, `ProtoListImplementation`), which are
 *  defined in `proto_internal.h`.
 *
 *  The methods here typically forward the call to the corresponding `impl...`
 *  method on the internal implementation object.
 */

#include "../headers/proto_internal.h"
#include <random>

namespace proto
{
    // A simple random number generator for mutable object IDs.
    // In a production system, this might use a more robust UUID library.
    unsigned long generate_mutable_ref() {
        return static_cast<unsigned long>(std::rand());
    }

    //=========================================================================
    // ProtoObject API Implementation
    //=========================================================================

    /**
     * @brief Retrieves the base prototype for any given object.
     * The prototype is determined by the object's type tag.
     */
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
        case POINTER_TAG_LARGE_INTEGER: return context->space->largeIntegerPrototype;
        default: return nullptr;
        }
    }

    /**
     * @brief Creates a new object with the same prototype chain and attributes.
     * This is a shallow copy. The underlying attribute list is shared.
     */
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

    /**
     * @brief Creates a new object that inherits from `this` object.
     * The new object's prototype chain will point to `this`.
     */
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

    /**
     * @brief The core method invocation logic.
     * It performs an optimized attribute lookup using a thread-local cache.
     * If the attribute is a method, it is invoked.
     */
    const ProtoObject* ProtoObject::call(ProtoContext* context, const ParentLink* nextParent, const ProtoString* method, const ProtoObject* self, const ProtoList* positionalParameters, const ProtoSparseList* keywordParametersDict) const
    {
        const auto* thread = toImpl<const ProtoThreadImplementation>(context->thread);
        const unsigned int hash = (reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(method)) & (THREAD_CACHE_DEPTH - 1);
        
        // The attributeCache is mutable, allowing for this performance optimization
        // within a const method.
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

    /**
     * @brief Checks if this object is an instance of a given prototype.
     * Traverses the prototype chain looking for a match.
     */
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

    /**
     * @brief Retrieves an attribute by name, traversing the prototype chain.
     * This method implements the core inheritance logic. It first checks for the
     * attribute on the object itself. If not found, it recursively checks each
     * object in the prototype chain.
     */
    const ProtoObject* ProtoObject::getAttribute(ProtoContext* context, const ProtoString* name) const
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
            
            // If the object is mutable, we must look at its most recent version
            // in the global mutable root.
            if (oc->mutable_ref) {
                oc = toImpl<const ProtoObjectCell>(context->space->mutableRoot.load()->getAt(context, oc->mutable_ref));
                if (!oc) break;
            }

            // Check own attributes.
            if (oc->attributes->implHas(context, attr_hash)) {
                const ProtoObject* result = oc->attributes->implGetAt(context, attr_hash);
                cache_entry = {this, name, result};
                return result;
            }

            // If not found, move to the next parent in the chain.
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

    /**
     * @brief Sets an attribute on an object.
     * If the object is immutable (default), this returns a new object with the
     * modified attribute. If the object is mutable, it updates the object
     * in-place via an atomic operation on the global mutable root.
     */
    const ProtoObject* ProtoObject::setAttribute(ProtoContext* context, const ProtoString* name, const ProtoObject* value) const
    {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;

        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return this;

        auto* oc = toImpl<ProtoObjectCell>(this);
        
        if (oc->mutable_ref) {
            // It's a mutable object, update the central repository atomically.
            ProtoSparseList* currentRoot;
            ProtoSparseList* newRoot;
            do {
                currentRoot = context->space->mutableRoot.load();
                const auto* currentVersion = toImpl<const ProtoObjectCell>(currentRoot->getAt(context, oc->mutable_ref));
                const auto* newAttributes = currentVersion->attributes->implSetAt(context, name->getHash(context), value);
                const auto* newVersion = new(context) ProtoObjectCell(context, currentVersion->parent, newAttributes, oc->mutable_ref);
                newRoot = const_cast<ProtoSparseList*>(currentRoot->setAt(context, oc->mutable_ref, newVersion->asObject(context)));
            } while (!context->space->mutableRoot.compare_exchange_strong(currentRoot, newRoot));
            return this; // Return the original mutable reference
        } else {
            // It's an immutable object, return a new one.
            const auto* newAttributes = oc->attributes->implSetAt(context, name->getHash(context), value);
            return (new(context) ProtoObjectCell(context, oc->parent, newAttributes, 0))->asObject(context);
        }
    }

    const ProtoObject* ProtoObject::hasAttribute(ProtoContext* context, const ProtoString* name) const
    {
        return this->getAttribute(context, name) != PROTO_NONE ? PROTO_TRUE : PROTO_FALSE;
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

    // --- Type Checking and Conversion ---

    int ProtoObject::isCell(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;
        return pa.op.pointer_tag != POINTER_TAG_EMBEDDED_VALUE;
    }

    const Cell* ProtoObject::asCell(ProtoContext* context) const {
        if (isCell(context)) {
            ProtoObjectPointer pa{};
            pa.oid.oid = this;
            return pa.cell.cell;
        }
        return nullptr;
    }

    const ProtoString* ProtoObject::asString(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_STRING) {
            return pa.string;
        }
        return nullptr;
    }

    ProtoMethod ProtoObject::asMethod(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_METHOD) {
            return toImpl<const ProtoMethodCell>(this)->method;
        }
        return nullptr;
    }

    bool ProtoObject::isNone(ProtoContext* context) const {
        return this == PROTO_NONE;
    }

    bool ProtoObject::isBoolean(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_BOOLEAN;
    }

    bool ProtoObject::isInteger(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_LARGE_INTEGER) return true;
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_SMALLINT) return true;
        return false;
    }

    bool ProtoObject::isString(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_STRING;
    }

    bool ProtoObject::isMethod(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_METHOD;
    }

    bool ProtoObject::isTuple(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_TUPLE;
    }

    unsigned long ProtoObject::getHash(ProtoContext* context) const {
        if (isCell(context)) {
            return asCell(context)->getHash(context);
        }
        // For embedded values, the pointer value itself is the hash
        return reinterpret_cast<uintptr_t>(this);
    }

    //=========================================================================
    // ProtoObject Coercion Implementation
    //=========================================================================
    const ProtoTuple* ProtoObject::asTuple(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_TUPLE) {
            return pa.tuple;
        }
        return nullptr;
    }

    bool ProtoObject::asBoolean(ProtoContext* context) const {
        if (this == PROTO_FALSE || this == PROTO_NONE) return false;
        if (this == PROTO_TRUE) return true;

        if (isInteger(context)) {
            return this->asLong(context) != 0;
        }
        if (isDouble(context)) {
            return this->asDouble(context) != 0.0;
        }
        // For collections, truthiness is based on size.
        if (isString(context)) {
            return this->asString(context)->getSize(context) > 0;
        }
        if (isTuple(context)) {
            return this->asTuple(context)->getSize(context) > 0;
        }

        // Default for all other objects is true.
        return true;
    }

    char ProtoObject::asByte(ProtoContext* context) const {
        return static_cast<char>(asLong(context));
    }

    const ProtoList* ProtoObject::asList(ProtoContext* context) const {
        // This logic should be handled by each specific type's implementation.
        // For now, we provide a default that prevents compilation errors.
        return context->newList()->appendLast(context, this);
    }
    // ProtoObject Double Implementation
    //=========================================================================

    double ProtoObject::asDouble(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_DOUBLE) {
            return toImpl<const DoubleImplementation>(this)->doubleValue;
        }
        return 0.0;
    }

    bool ProtoObject::isDouble(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_DOUBLE;
    }

    //=========================================================================
    // ProtoObject (Integer API) Implementation
    //=========================================================================

    long long ProtoObject::asLong(ProtoContext* context) const {
        return Integer::asLong(context, this);
    }

    int ProtoObject::compare(ProtoContext* context, const ProtoObject* other) const {
        return Integer::compare(context, this, other);
    }

    const ProtoObject* ProtoObject::negate(ProtoContext* context) const {
        return Integer::negate(context, this);
    }

    const ProtoObject* ProtoObject::abs(ProtoContext* context) const {
        return Integer::abs(context, this);
    }

    const ProtoObject* ProtoObject::add(ProtoContext* context, const ProtoObject* other) const {
        return Integer::add(context, this, other);
    }

    const ProtoObject* ProtoObject::subtract(ProtoContext* context, const ProtoObject* other) const {
        return Integer::subtract(context, this, other);
    }

    const ProtoObject* ProtoObject::multiply(ProtoContext* context, const ProtoObject* other) const {
        return Integer::multiply(context, this, other);
    }

    const ProtoObject* ProtoObject::divide(ProtoContext* context, const ProtoObject* other) const {
        return Integer::divide(context, this, other);
    }

    const ProtoObject* ProtoObject::modulo(ProtoContext* context, const ProtoObject* other) const {
        return Integer::modulo(context, this, other);
    }

    const ProtoObject* ProtoObject::bitwiseAnd(ProtoContext* context, const ProtoObject* other) const {
        return Integer::bitwiseAnd(context, this, other);
    }

    const ProtoObject* ProtoObject::bitwiseOr(ProtoContext* context, const ProtoObject* other) const {
        return Integer::bitwiseOr(context, this, other);
    }

    const ProtoObject* ProtoObject::bitwiseXor(ProtoContext* context, const ProtoObject* other) const {
        return Integer::bitwiseXor(context, this, other);
    }

    const ProtoObject* ProtoObject::bitwiseNot(ProtoContext* context) const {
        return Integer::bitwiseNot(context, this);
    }

    const ProtoObject* ProtoObject::shiftLeft(ProtoContext* context, int amount) const {
        return Integer::shiftLeft(context, this, amount);
    }

    const ProtoObject* ProtoObject::shiftRight(ProtoContext* context, int amount) const {
        return Integer::shiftRight(context, this, amount);
    }

    const ProtoObject* ProtoObject::divmod(ProtoContext* context, const ProtoObject* other) const {
        const ProtoObject* quotient = this->divide(context, other);
        const ProtoObject* remainder = this->modulo(context, other);
        const ProtoList* resultList = context->newList()->appendLast(context, quotient)->appendLast(context, remainder);
        return context->newTupleFromList(resultList)->asObject(context);
    }

    //=========================================================================
    // ProtoList API Implementation
    //=========================================================================

    const ProtoList* ProtoList::appendLast(ProtoContext* context, const ProtoObject* value) const {
        return toImpl<const ProtoListImplementation>(this)->implAppendLast(context, value)->asProtoList(context);
    }

    const ProtoList* ProtoList::extend(ProtoContext* context, const ProtoList* otherList) const {
        const auto* impl = toImpl<const ProtoListImplementation>(this);
        const auto* otherImpl = toImpl<const ProtoListImplementation>(otherList);
        const auto* resultImpl = impl->implExtend(context, otherImpl);
        return resultImpl->asProtoList(context);
    }

    //=========================================================================
    // ProtoListIterator API Implementation
    //=========================================================================

    int ProtoListIterator::hasNext(ProtoContext* context) const {
        return toImpl<const ProtoListIteratorImplementation>(this)->implHasNext(context);
    }

    const ProtoObject* ProtoListIterator::next(ProtoContext* context) const {
        return toImpl<const ProtoListIteratorImplementation>(this)->implNext(context);
    }

    const ProtoListIterator* ProtoListIterator::advance(ProtoContext* context) const {
        const auto* impl = toImpl<const ProtoListIteratorImplementation>(this)->implAdvance(context);
        return impl ? impl->asProtoListIterator(context) : nullptr;
    }

    //=========================================================================
    // ProtoString API Implementation
    //=========================================================================

    unsigned long ProtoString::getHash(ProtoContext* context) const {
        return toImpl<const ProtoStringImplementation>(this)->getHash(context);
    }
    unsigned long ProtoString::getSize(ProtoContext* context) const {
        return toImpl<const ProtoStringImplementation>(this)->implGetSize(context);
    }
    const ProtoObject* ProtoString::getAt(ProtoContext* context, int index) const {
        return toImpl<const ProtoStringImplementation>(this)->implGetAt(context, index);
    }
    const ProtoString* ProtoString::appendLast(ProtoContext* context, const ProtoString* otherString) const {
        const auto* impl = toImpl<const ProtoStringImplementation>(this)->implAppendLast(context, otherString);
        return impl->asProtoString(context);
    }

    //=========================================================================
    // ProtoSparseList API Implementation
    //=========================================================================

    bool ProtoSparseList::has(ProtoContext* context, unsigned long offset) const {
        return toImpl<const ProtoSparseListImplementation>(this)->implHas(context, offset);
    }
    const ProtoObject* ProtoSparseList::getAt(ProtoContext* context, unsigned long offset) const {
        return toImpl<const ProtoSparseListImplementation>(this)->implGetAt(context, offset);
    }
    const ProtoSparseList* ProtoSparseList::setAt(ProtoContext* context, unsigned long offset, const ProtoObject* value) const {
        return toImpl<const ProtoSparseListImplementation>(this)->implSetAt(context, offset, value)->asSparseList(context);
    }

    //=========================================================================
    // ProtoThread API Implementation
    //=========================================================================

    void ProtoThread::setCurrentContext(ProtoContext* context) {
        toImpl<ProtoThreadImplementation>(this)->implSetCurrentContext(context);
    }

    const ProtoObject* ProtoList::getAt(ProtoContext* context, int index) const {
        return toImpl<const ProtoListImplementation>(this)->implGetAt(context, index);
    }

    const ProtoObject* ProtoList::getFirst(ProtoContext* context) const {
        return toImpl<const ProtoListImplementation>(this)->implGetFirst(context);
    }

    const ProtoObject* ProtoList::getLast(ProtoContext* context) const {
        return toImpl<const ProtoListImplementation>(this)->implGetLast(context);
    }

    const ProtoList* ProtoList::getSlice(ProtoContext* context, int from, int to) const {
        return toImpl<const ProtoListImplementation>(this)->implGetSlice(context, from, to)->asProtoList(context);
    }

    unsigned long ProtoList::getSize(ProtoContext* context) const {
        return toImpl<const ProtoListImplementation>(this)->implGetSize(context);
    }

    bool ProtoList::has(ProtoContext* context, const ProtoObject* value) const {
        return toImpl<const ProtoListImplementation>(this)->implHas(context, value);
    }

    const ProtoObject* ProtoList::asObject(ProtoContext* context) const {
        return toImpl<const ProtoListImplementation>(this)->implAsObject(context);
    }

    const ProtoListIterator* ProtoList::getIterator(ProtoContext* context) const {
        return toImpl<const ProtoListImplementation>(this)->implGetIterator(context)->asProtoListIterator(context);
    }

    unsigned long ProtoList::getHash(ProtoContext* context) const {
        return toImpl<const ProtoListImplementation>(this)->getHash(context);
    }

    //=========================================================================
    // ProtoSparseListIterator API Implementation
    //=========================================================================

    int ProtoSparseListIterator::hasNext(ProtoContext* context) const {
        return toImpl<const ProtoSparseListIteratorImplementation>(this)->implHasNext(context);
    }

    unsigned long ProtoSparseListIterator::nextKey(ProtoContext* context) const {
        return toImpl<const ProtoSparseListIteratorImplementation>(this)->implNextKey(context);
    }

    const ProtoObject* ProtoSparseListIterator::nextValue(ProtoContext* context) const {
        return toImpl<const ProtoSparseListIteratorImplementation>(this)->implNextValue(context);
    }

    const ProtoSparseListIterator* ProtoSparseListIterator::advance(ProtoContext* context) {
        const auto* impl = toImpl<const ProtoSparseListIteratorImplementation>(this)->implAdvance(context);
        return impl ? (const ProtoSparseListIterator*)impl->implAsObject(context) : nullptr;
    }

    const ProtoObject* ProtoSparseListIterator::asObject(ProtoContext* context) const {
        return toImpl<const ProtoSparseListIteratorImplementation>(this)->implAsObject(context);
    }

    //=========================================================================
    // ProtoSparseList API Implementation (continued)
    //=========================================================================

    const ProtoObject* ProtoSparseList::asObject(ProtoContext* context) const {
        return toImpl<const ProtoSparseListImplementation>(this)->implAsObject(context);
    }

    const ProtoSparseListIterator* ProtoSparseList::getIterator(ProtoContext* context) const {
        const auto* impl = toImpl<const ProtoSparseListImplementation>(this)->implGetIterator(context);
        return impl ? (const ProtoSparseListIterator*)impl->implAsObject(context) : nullptr;
    }

    unsigned long ProtoSparseList::getHash(ProtoContext* context) const {
        return toImpl<const ProtoSparseListImplementation>(this)->getHash(context);
    }

    unsigned long ProtoSparseList::getSize(ProtoContext* context) const {
        return toImpl<const ProtoSparseListImplementation>(this)->implGetSize(context);
    }

} // namespace proto
