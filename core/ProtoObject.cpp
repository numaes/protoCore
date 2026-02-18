/*
 * ProtoObject.cpp
 *
 *  Created on: Aug 6, 2017
 *      Author: gamarino
 *
 *  This file implements ProtoObjectCell (internal representation of a standard Proto object)
 *  and the ProtoObject external API (trampolines to implementation types).
 */

#include "../headers/proto_internal.h"

namespace proto
{
    /**
     * @class ProtoObjectCell
     * @brief The internal implementation of a standard, user-creatable object.
     *
     * This class is the concrete representation of a dynamic object in Proto.
     * It combines the two key aspects of the object model: its own attributes
     * and its link to a prototype chain for inheritance.
     */

    /**
     * @brief Constructs a new object cell.
     * @param context The current execution context.
     * @param parent A pointer to the `ParentLink` that forms the head of the prototype chain.
     * @param attributes A `ProtoSparseList` holding the object's own key-value attributes.
     * @param mutable_ref A non-zero ID if this object is a mutable reference, otherwise 0.
     */
    ProtoObjectCell::ProtoObjectCell(
        ProtoContext* context,
        const ParentLinkImplementation* parent,
        const ProtoSparseListImplementation* attributes,
        const unsigned long mutable_ref
    ) : Cell(context), parent(parent), mutable_ref(mutable_ref),
        attributes(attributes ? attributes : new(context) ProtoSparseListImplementation(context, 0, PROTO_NONE, nullptr, nullptr, true))
    {
    }

    /**
     * @brief Creates a new object that inherits from the current one.
     * This is a key part of the immutable API. It returns a new `ProtoObjectCell`
     * whose prototype chain includes the `newParentToAdd`.
     * @param context The current execution context.
     * @param newParentToAdd The object to be added as the immediate parent of the new object.
     * @return A new `ProtoObjectCell` instance.
     */
    const ProtoObjectCell* ProtoObjectCell::addParent(
        ProtoContext* context, const ProtoObject* newParentToAdd) const
    {
        const auto* newParentLink = new(context) ParentLinkImplementation(
            context,
            this->parent,
            newParentToAdd
        );

        return new(context) ProtoObjectCell(
            context,
            newParentLink,
            this->attributes,
            0 // Snapshot or child object should not have a mutable_ref by default
        );
    }

    /**
     * @brief Converts this internal implementation object into its public API handle.
     * This method sets the correct type tag on a `ProtoObjectPointer` union to create
     * a valid `ProtoObject*` that can be returned to the user.
     */
    const ProtoObject* ProtoObjectCell::asObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.objectCellImplementation = this;
        p.op.pointer_tag = POINTER_TAG_OBJECT;
        return p.oid;
    }

    /**
     * @brief Finalizer for the ProtoObjectCell.
     * This object holds no external resources, so the finalizer is empty.
     */
    void ProtoObjectCell::finalize(ProtoContext* context) const
    {
    };

    /**
     * @brief Informs the GC about the cells this object holds references to.
     * An object cell holds references to its parent link chain and its own
     * attribute list. Both must be reported to the GC to prevent them from
     * being prematurely collected.
     */
    void ProtoObjectCell::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const
    {
        // Report the head of the prototype chain.
        if (this->parent)
        {
            method(context, self, this->parent);
        }

        // Report the attribute list.
        if (this->attributes)
        {
            method(context, self, this->attributes);
        }
    }

    const ProtoObject* ProtoObjectCell::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.objectCellImplementation = this;
        p.op.pointer_tag = POINTER_TAG_OBJECT;
        return p.oid;
    }

    //=========================================================================
    // ProtoObject external API (trampolines to ProtoObjectCell and other impl types)
    //=========================================================================
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
            case 5: // EMBEDDED_TYPE_NONE (Sync with proto_internal.h)
                return context->space->nonePrototype;
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
        case POINTER_TAG_RANGE_ITERATOR: return context->space->rangeIteratorPrototype;
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
        unsigned long ref = isMutable ? generate_mutable_ref(context) : 0;
        auto* newObject = new(context) ProtoObjectCell(context, new(context) ParentLinkImplementation(context, oc->parent, this), toImpl<const ProtoSparseListImplementation>(context->newSparseList()), ref);
        const ProtoObject* result = newObject->asObject(context);
        return result;
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
        int iterationCount = 0;

        while (current) {
            if (++iterationCount > 50) {
                 return PROTO_FALSE;
            }
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
                      if (storedState != nullptr && storedState != current) {
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
                int sibCount = 0;
                while (sibling && plPtr < 64) {
                    if (++sibCount > 100) {
                        break;
                    }
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

    const ProtoObject* ProtoObject::getAttribute(ProtoContext* context, const ProtoString* name, bool callbacks) const
    {
        if (!this || !name) return nullptr;

        // Attribute Cache Setup
        AttributeCacheEntry* cache = nullptr;
        if (context->thread) {
            auto* threadImpl = toImpl<ProtoThreadImplementation>(context->thread);
            if (threadImpl->extension) {
                cache = threadImpl->extension->attributeCache;
            }
        }

        const ProtoObject* currentPointer = this;
        const ParentLinkImplementation* currentLink = nullptr;
        const unsigned long attr_hash = reinterpret_cast<uintptr_t>(name);
        int iterationCount = 0;

        while (currentPointer) {
            if (++iterationCount > 500) {
                 return PROTO_NONE;
            }

            // Resolve current state for the current pointer
            const ProtoObject* currentValue = currentPointer;
            ProtoObjectPointer pa_cur{};
            pa_cur.oid = currentPointer;
            
            if (pa_cur.op.pointer_tag == POINTER_TAG_OBJECT) {
                auto oc = toImpl<const ProtoObjectCell>(currentPointer);
                if (oc->mutable_ref > 0) {
                    ProtoSparseList* root = context->space->mutableRoot.load();
                    if (root != nullptr) {
                        const auto* mutableList = toImpl<const ProtoSparseListImplementation>(root);
                        const proto::ProtoObject* storedState = mutableList->implGetAt(context, oc->mutable_ref);
                        if (storedState != nullptr) {
                            currentValue = storedState;
                        }
                    }
                }
            }

            // Check Cache using currentPointer (the handle)
            unsigned long hash_idx = 0;
            if (cache) {
                hash_idx = (reinterpret_cast<uintptr_t>(currentPointer) ^ name->getHash(context)) % THREAD_CACHE_DEPTH;
                if (cache[hash_idx].object == currentPointer && cache[hash_idx].name == name) {
                    const auto* result = cache[hash_idx].result;
                    if (name) {
                        std::string ns;
                        name->toUTF8String(context, ns);
                        if (ns == "_splitext" || ns == "genericpath") {
                             fprintf(stderr, "DEBUG: getAttribute CACHE obj=%p name=%s hash=%p res=%p\n", (void*)currentPointer, ns.c_str(), (void*)attr_hash, (void*)result);
                        }
                    }
                    return result;
                }
            }

            // Check OWN Attributes in currentValue
            if (pa_cur.op.pointer_tag == POINTER_TAG_OBJECT) {
                auto ocValue = toImpl<const ProtoObjectCell>(currentValue);
                if (ocValue->attributes->implHas(context, attr_hash)) {
                    const auto* result = ocValue->attributes->implGetAt(context, attr_hash);
                    if (name) {
                        std::string ns;
                        name->toUTF8String(context, ns);
                        if (ns == "_splitext" || ns == "genericpath") {
                             fprintf(stderr, "DEBUG: getAttribute obj=%p name=%s hash=%p res=%p\n", (void*)currentPointer, ns.c_str(), (void*)attr_hash, (void*)result);
                        }
                    }
                    // Update Cache
                    if (cache) {
                        cache[hash_idx] = {currentPointer, result, name};
                    }
                    return result;
                }

                // Move to NEXT in linearized chain
                if (!currentLink) {
                    // We just checked 'this'. Now follow its parent links.
                    currentLink = ocValue->parent;
                } else {
                    // We are already in the middle of a ParentLink chain. Move to next link.
                    currentLink = currentLink->getParent(context);
                }

                if (currentLink) {
                    currentPointer = currentLink->getObject(context);
                } else {
                    currentPointer = nullptr;
                }
            } else {
                // Non-object tagged pointers (e.g. Integers, None) - use their prototype
                // Important: getPrototype for non-objects might NOT be linearized.
                // But for now we follow the existing pattern.
                const ProtoObject* nextProto = currentPointer->getPrototype(context);
                if (nextProto == currentPointer) break; 
                currentPointer = nextProto;
            }
        }
        return PROTO_NONE;
    }

    const ProtoObject* ProtoObject::setAttribute(ProtoContext* context, const ProtoString* name, const ProtoObject* value) const
    {
        if (!this || !name) return this;
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
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return this;
        auto* oc = toImpl<ProtoObjectCell>(this);

        // Handle Mutable Objects
        if (oc->mutable_ref > 0) {
             // 1. Get current object state
             const ProtoObject* currentObjState = this; 
             ProtoSparseList* root = context->space->mutableRoot.load();
             if (root != nullptr) {
                 const auto* currentMutableList = toImpl<const ProtoSparseListImplementation>(root);
                 const ProtoObject* storedState = currentMutableList->implGetAt(context, oc->mutable_ref);
                 if (storedState != nullptr) {
                      currentObjState = storedState;
                 }
             }

             // 2. Create new state with updated attribute
             auto* currentOc = toImpl<const ProtoObjectCell>(currentObjState);
             const auto* newAttributes = currentOc->attributes->implSetAt(context, reinterpret_cast<uintptr_t>(name), value);
             
             // CRITICAL: newState MUST have mutable_ref = 0 to avoid infinite loop during lookup
             auto* newState = (new(context) ProtoObjectCell(context, currentOc->parent, newAttributes, 0))->asObject(context);

             // 4. Update mutableRoot (Compare and Swap Loop)
             int casIteration = 0;
             while(true) {
                 if (++casIteration > 100) {
                     break;
                 }
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
        const ProtoObject* result = (new(context) ProtoObjectCell(context, oc->parent, newAttributes, 0))->asObject(context);
        return result;
    }

    const ProtoList* ProtoObject::getParents(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return context->newList();
        const auto* oc = toImpl<const ProtoObjectCell>(this);
        
        // Handle Mutable Objects
        if (oc->mutable_ref > 0) {
             ProtoSparseList* root = context->space->mutableRoot.load();
             if (root != nullptr) {
                  const auto* mutableList = toImpl<const ProtoSparseListImplementation>(root);
                  const proto::ProtoObject* storedState = mutableList->implGetAt(context, oc->mutable_ref);
                  if (storedState != nullptr && storedState != this) {
                      ProtoObjectPointer psa{};
                      psa.oid = storedState;
                      if (psa.op.pointer_tag == POINTER_TAG_OBJECT) {
                          oc = toImpl<const ProtoObjectCell>(storedState);
                      }
                  }
             }
        }

        const ProtoList* parents = context->newList();
        const ParentLinkImplementation* p = oc->parent;
        while (p) {
            parents = parents->appendLast(context, p->getObject(context));
            p = p->getParent(context);
        }
        return parents;
    }

    int ProtoObject::hasParent(ProtoContext* context, const ProtoObject* target) const {
        if (!this || !target) return 0;
        if (target == this) return 1;
        
        const ProtoList* pList = getParents(context);
        return pList && pList->has(context, target) ? 1 : 0;
    }

    const ProtoObject* ProtoObject::addParentInternal(ProtoContext* context, const ProtoObject* newParent) const {
        const auto* oc = toImpl<const ProtoObjectCell>(this);
        
        // Handle Mutable Objects
        if (oc->mutable_ref > 0) {
             // 1. Get current object state
             const ProtoObject* currentObjState = this; 
             ProtoSparseList* root = context->space->mutableRoot.load();
             if (root != nullptr) {
                 const auto* currentMutableList = toImpl<const ProtoSparseListImplementation>(root);
                 const ProtoObject* storedState = currentMutableList->implGetAt(context, oc->mutable_ref);
                 if (storedState != nullptr) {
                      currentObjState = storedState;
                 }
             }

             // 2. Create new state with added parent
             auto* currentOc = toImpl<const ProtoObjectCell>(currentObjState);
             auto* newState = currentOc->addParent(context, newParent)->asObject(context);

             // 3. Update mutableRoot (Compare and Swap Loop)
             int casIteration = 0;
             while(true) {
                 if (++casIteration > 100) {
                     break;
                 }
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

    const ProtoObject* ProtoObject::addParent(ProtoContext* context, const ProtoObject* newParent) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT || !newParent->isCell(context)) return this;
        
        const ProtoObject* result = this;
        const ProtoList* mroOfNew = newParent->getParents(context);

        // Add ancestors of newParent that are not in 'this' chain
        // We add them in reverse order to maintain 'newParent parent chain order' after prepending
        if (mroOfNew) {
            for (int i = static_cast<int>(mroOfNew->getSize(context)) - 1; i >= 0; --i) {
                const ProtoObject* ancestor = mroOfNew->getAt(context, i);
                if (ancestor != newParent && !result->hasParent(context, ancestor)) {
                    result = result->addParentInternal(context, ancestor);
                }
            }
        }
        
        // Finally add newParent itself
        if (!result->hasParent(context, newParent)) {
            result = result->addParentInternal(context, newParent);
        }
        
        return result;
    }

    int ProtoObject::isCell(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        return pa.op.pointer_tag != POINTER_TAG_EMBEDDED_VALUE;
    }

    const Cell* ProtoObject::asCell(ProtoContext* context) const {
        if (!this) return nullptr;
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_EMBEDDED_VALUE) {
            uintptr_t raw_ptr_value = reinterpret_cast<uintptr_t>(pa.oid) & ~0x3FUL;
            return reinterpret_cast<const Cell*>(raw_ptr_value);
        }
        return nullptr;
    }

    const ProtoString* ProtoObject::asString(ProtoContext* context) const {
        if (!this) return nullptr;
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_STRING) return reinterpret_cast<const ProtoString*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_INLINE_STRING) return reinterpret_cast<const ProtoString*>(this);
        
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->asString(context);
        }
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
        if (!this) return false;
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_STRING || (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_INLINE_STRING)) return true;
        
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->isString(context);
        }
        return false;
    }
    bool ProtoObject::isMethod(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_METHOD; }
    bool ProtoObject::isTuple(ProtoContext* context) const {
        if (!this) return false;
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_TUPLE) return true;
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->isTuple(context);
        }
        return false;
    }
    bool ProtoObject::isSet(ProtoContext* context) const {
        if (!this) return false;
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_SET) return true;
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->isSet(context);
        }
        return false;
    }
    bool ProtoObject::isMultiset(ProtoContext* context) const {
        if (!this) return false;
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_MULTISET) return true;
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->isMultiset(context);
        }
        return false;
    }

    unsigned long ProtoObject::getHash(ProtoContext* context) const {
        if (!this) return 0;

        ProtoObjectPointer pa{}; pa.oid = this;
        // If it's a wrapper object, we must extract the underlying value to hash it correctly.
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            if (isString(context)) {
                const ProtoString* s = asString(context);
                if (s) {
                    const ProtoObject* so = s->asObject(context);
                    if (so != this) return so->getHash(context);
                }
            }
            if (isTuple(context)) {
                const ProtoTuple* t = asTuple(context);
                if (t) {
                    const ProtoObject* to = t->asObject(context);
                    if (to != this) return to->getHash(context);
                }
            }
        }

        if (isString(context) && isInlineString(this)) return getProtoStringHash(context, this);
        
        if (isCell(context)) return asCell(context)->getHash(context);
        return reinterpret_cast<uintptr_t>(this);
    }

    const ProtoTuple* ProtoObject::asTuple(ProtoContext* context) const {
        if (!this) return nullptr;
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_TUPLE) return reinterpret_cast<const ProtoTuple*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->asTuple(context);
        }
        return nullptr;
    }
    const ProtoSet* ProtoObject::asSet(ProtoContext* context) const {
        if (!this) return nullptr;
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_SET) return reinterpret_cast<const ProtoSet*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->asSet(context);
        }
        return nullptr;
    }
    const ProtoSetIterator* ProtoObject::asSetIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_SET_ITERATOR ? reinterpret_cast<const ProtoSetIterator*>(this) : nullptr; }
    const ProtoMultiset* ProtoObject::asMultiset(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_MULTISET ? reinterpret_cast<const ProtoMultiset*>(this) : nullptr; }
    const ProtoMultisetIterator* ProtoObject::asMultisetIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_MULTISET_ITERATOR ? reinterpret_cast<const ProtoMultisetIterator*>(this) : nullptr; }
    const ProtoThread* ProtoObject::asThread(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_THREAD ? reinterpret_cast<const ProtoThread*>(this) : nullptr; }
    const ProtoListIterator* ProtoObject::asListIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_LIST_ITERATOR ? reinterpret_cast<const ProtoListIterator*>(this) : nullptr; }
    const ProtoTupleIterator* ProtoObject::asTupleIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_TUPLE_ITERATOR ? reinterpret_cast<const ProtoTupleIterator*>(this) : nullptr; }
    const ProtoStringIterator* ProtoObject::asStringIterator(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_STRING_ITERATOR ? reinterpret_cast<const ProtoStringIterator*>(this) : nullptr; }
    const ProtoSparseList* ProtoObject::asSparseList(ProtoContext* context) const {
        if (!this) return nullptr;
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_SPARSE_LIST) return reinterpret_cast<const ProtoSparseList*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
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
        if (!this) return nullptr;
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_LIST) return reinterpret_cast<const ProtoList*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_STRING) return toImpl<const ProtoStringImplementation>(this)->implAsList(context);
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_INLINE_STRING) return reinterpret_cast<const ProtoString*>(this)->asList(context);
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
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
    const ProtoObject* ProtoObject::multiply(ProtoContext* context, const ProtoObject* other) const {
        const ProtoString* s1 = this->asString(context);
        if (s1) {
            const ProtoString* res = s1->multiply(context, other);
            return res ? res->asObject(context) : Integer::multiply(context, this, other);
        }
        const ProtoString* s2 = other->asString(context);
        if (s2) {
            const ProtoString* res = s2->multiply(context, this);
            return res ? res->asObject(context) : Integer::multiply(context, this, other);
        }

        const ProtoList* l1 = this->asList(context);
        if (l1) {
            const ProtoList* res = l1->multiply(context, other);
            if (res) {
                ProtoObject* wrapper = const_cast<ProtoObject*>(context->newObject(true));
                if (context->space->listPrototype) {
                    wrapper = const_cast<ProtoObject*>(wrapper->addParent(context, context->space->listPrototype));
                }
                const ProtoString* dataName = context->space->literalData;
                return wrapper->setAttribute(context, dataName, res->asObject(context));
            }
            return Integer::multiply(context, this, other);
        }
        const ProtoList* l2 = other->asList(context);
        if (l2) {
            const ProtoList* res = l2->multiply(context, this);
            if (res) {
                ProtoObject* wrapper = const_cast<ProtoObject*>(context->newObject(true));
                if (context->space->listPrototype) {
                    wrapper = const_cast<ProtoObject*>(wrapper->addParent(context, context->space->listPrototype));
                }
                const ProtoString* dataName = context->space->literalData;
                return wrapper->setAttribute(context, dataName, res->asObject(context));
            }
            return Integer::multiply(context, this, other);
        }

        return Integer::multiply(context, this, other);
    }
    const ProtoObject* ProtoObject::divide(ProtoContext* context, const ProtoObject* other) const { return Integer::divide(context, this, other); }
    const ProtoObject* ProtoObject::modulo(ProtoContext* context, const ProtoObject* other) const {
        const ProtoString* s = this->asString(context);
        if (s) {
            return s->modulo(context, other);
        }
        return Integer::modulo(context, this, other);
    }
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

    const ProtoObject* ProtoObject::hasAttribute(ProtoContext* context, const ProtoString* name) const
    {
        if (!this) return PROTO_FALSE;

        const ProtoObject* currentObject = this;
        const unsigned long attr_hash = reinterpret_cast<uintptr_t>(name);

        const ParentLinkImplementation* plStack[64];
        int plPtr = 0;
        int iterationCount = 0;

        while (currentObject) {
            if (++iterationCount > 50) return PROTO_FALSE;

            ProtoObjectPointer pa_cur{};
            pa_cur.oid = currentObject;
            if (pa_cur.op.pointer_tag != POINTER_TAG_OBJECT) {
                currentObject = currentObject->getPrototype(context);
                continue;
            }
            auto oc = toImpl<const ProtoObjectCell>(currentObject);
            if (!oc) {
                return PROTO_FALSE;
            }
            const ProtoSparseListImplementation* attributes = oc->attributes;
            if (!attributes) {
                return PROTO_FALSE;
            }

            // Support for Mutable Objects
            if (oc->mutable_ref > 0) {
                 ProtoSparseList* root = context->space->mutableRoot.load();
                 if (root != nullptr) {
                      const auto* mutableList = toImpl<const ProtoSparseListImplementation>(root);
                      const proto::ProtoObject* storedState = mutableList->implGetAt(context, oc->mutable_ref);
                      if (storedState != nullptr && storedState != currentObject) {
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
                return PROTO_TRUE;
            }

            // Multiple inheritance support:
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
        return PROTO_FALSE;
    }
    
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
                if (storedState != nullptr) {
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
                if (storedState != nullptr) {
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
                if (storedState != nullptr) {
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

} // namespace proto
