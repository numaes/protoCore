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
    namespace {
        /**
         * Per-thread cache lookup for a mutable object's current snapshot.
         *
         * Validation: the cached entry is live iff `mutable_ref` matches AND the cached
         * `shard_root` pointer equals the current `mutableRoot[shard].root`. Any successful
         * CAS by any thread (including this one) replaces the shard root pointer, which
         * naturally invalidates stale entries on the next lookup.
         *
         * On miss, performs the authoritative load + AVL `implGetAt` and refreshes the
         * cache. Returns nullptr if there is no current snapshot for `mutable_ref` (i.e.
         * the object has not yet been mutated since allocation).
         */
        inline const ProtoObject* resolveMutableSnapshot(ProtoContext* context,
                                                         unsigned long mutable_ref) {
            // Cache fast-path
            MutableValueCacheEntry* cache = nullptr;
            if (context->thread) {
                auto* threadImpl = toImpl<ProtoThreadImplementation>(context->thread);
                if (threadImpl->extension) {
                    cache = threadImpl->extension->mutableValueCache;
                }
            }
            int shard = mutable_ref % ProtoSpace::MUTABLE_ROOT_SHARDS;
            unsigned long idx = mutable_ref % MUTABLE_VALUE_CACHE_DEPTH;

            if (cache && cache[idx].mutable_ref == mutable_ref) {
                ProtoSparseList* live =
                    context->space->mutableRoot[shard].root.load(std::memory_order_acquire);
                if (live == cache[idx].shard_root) {
                    // Cached entry is valid; current_value may be nullptr (negative
                    // cache: object never mutated yet) — both states are correct hits.
                    return cache[idx].current_value;
                }
            }

            // Authoritative resolve
            ProtoSparseList* live =
                context->space->mutableRoot[shard].root.load(std::memory_order_acquire);
            const ProtoObject* snap = nullptr;
            if (live != nullptr) {
                const auto* mutableList = toImpl<const ProtoSparseListImplementation>(live);
                snap = mutableList->implGetAt(context, mutable_ref);
            }

            // Cache the result, INCLUDING the negative case (snap == nullptr).
            // Negative caching is critical: a mutable object that has not yet been
            // mutated (e.g. a fresh function created with mutable=true that no decorator
            // touched) would otherwise pay a fresh load + AVL implGetAt on every
            // attribute read forever. With negative caching, the second and later reads
            // hit the cache and return nullptr immediately; the caller falls back to
            // the original `this` pointer at zero extra cost.
            if (cache) {
                cache[idx] = {mutable_ref, live, snap};
            }
            return snap;
        }

        /**
         * Refresh the per-thread mutable-value cache after a successful CAS on the
         * shard root. Called on the writing thread; subsequent reads on this thread
         * will hit the cache with the freshly published `(new_root, new_value)` pair.
         */
        inline void refreshMutableCache(ProtoContext* context,
                                        unsigned long mutable_ref,
                                        ProtoSparseList* new_root,
                                        const ProtoObject* new_value) {
            if (!context->thread) return;
            auto* threadImpl = toImpl<ProtoThreadImplementation>(context->thread);
            if (!threadImpl->extension || !threadImpl->extension->mutableValueCache) return;
            unsigned long idx = mutable_ref % MUTABLE_VALUE_CACHE_DEPTH;
            threadImpl->extension->mutableValueCache[idx] = {mutable_ref, new_root, new_value};
        }
    }

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
        case POINTER_TAG_OBJECT: {
            auto* oc = toImpl<const ProtoObjectCell>(this);
            if (oc->parent && ((uintptr_t)oc->parent & 0x3F) == 0) {
                 auto* pl = toImpl<const ParentLinkImplementation>(oc->parent);
                 if (pl->getType() == CellType::ParentLink) return pl->getObject(context);
            }
            return context->space->objectPrototype;
        }
        case POINTER_TAG_EMBEDDED_VALUE:
            switch (pa.op.embedded_type)
            {
            case EMBEDDED_TYPE_SMALLINT: return context->space->smallIntegerPrototype;
            case EMBEDDED_TYPE_BOOLEAN: return context->space->booleanPrototype;
            case EMBEDDED_TYPE_UNICODE_CHAR: return context->space->unicodeCharPrototype;
            case EMBEDDED_TYPE_INLINE_STRING: return context->space->stringPrototype;
            case EMBEDDED_TYPE_NONE: return context->space->nonePrototype;
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
        const ProtoObject* current = this->getPrototype(context);
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
            
            // Handle Mutable Objects (cache-fast)
            if (oc->mutable_ref > 0) {
                 const proto::ProtoObject* storedState =
                     resolveMutableSnapshot(context, oc->mutable_ref);
                 if (storedState != nullptr && storedState != current) {
                      ProtoObjectPointer psa{};
                      psa.oid = storedState;
                      if (psa.op.pointer_tag == POINTER_TAG_OBJECT) {
                          oc = toImpl<const ProtoObjectCell>(storedState);
                      }
                 }
            }
            
            if (oc->parent && ((uintptr_t)oc->parent & 0x3F) == 0) {
                auto pl = toImpl<const ParentLinkImplementation>(oc->parent);
                if (pl->getType() == CellType::ParentLink) {
                    const ParentLinkImplementation* sibling = pl->getParent(context);
                    int sibCount = 0;
                    while (sibling && plPtr < 64 && ((uintptr_t)sibling & 0x3F) == 0) {
                        auto sl = toImpl<const ParentLinkImplementation>(sibling);
                        if (sl->getType() != CellType::ParentLink) break;
                        if (++sibCount > 100) break;
                        plStack[plPtr++] = sibling;
                        sibling = sl->getParent(context);
                    }
                    current = pl->getObject(context);
                } else {
                    current = nullptr;
                }
            } else {
                if (plPtr > 0) {
                    const ParentLinkImplementation* top = plStack[--plPtr];
                    if (top && ((uintptr_t)top & 0x3F) == 0) {
                        auto tl = toImpl<const ParentLinkImplementation>(top);
                        if (tl->getType() == CellType::ParentLink) {
                            current = tl->getObject(context);
                        } else {
                            current = nullptr;
                        }
                    } else {
                        current = nullptr;
                    }
                } else {
                    current = nullptr;
                }
            }
        }
        return PROTO_NONE;
    }

    const ProtoObject* ProtoObject::getAttribute(ProtoContext* context, const ProtoString* name, bool callbacks) const
    {
        if (!this || !name) return nullptr;

        // Look up the canonical symbol for this key without inserting.
        // If the key was never interned, it was never used as an attribute key,
        // so the attribute cannot exist — return nullptr immediately.
        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                const ProtoObject* sym = context->space->symbolTable->lookupByContent(
                    context, reinterpret_cast<const ProtoObject*>(name));
                if (!sym) return nullptr;
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

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
                if (proto::isObject(currentPointer)) {
                    auto oc = toImpl<const ProtoObjectCell>(currentPointer);
                    if (oc->mutable_ref > 0) {
                        const proto::ProtoObject* storedState =
                            resolveMutableSnapshot(context, oc->mutable_ref);
                        if (storedState != nullptr) {
                            currentValue = storedState;
                        }
                    }
                }
            }

            // Cache keyed on currentValue (the resolved immutable snapshot).
            // For immutable objects currentValue == currentPointer, so behaviour is unchanged.
            // For mutable objects currentValue is the snapshot stored in mutableRoot; when the
            // object mutates mutableRoot is updated to a new snapshot pointer, so the next lookup
            // gets a different currentValue → natural cache miss → correct re-lookup.
            // This avoids the former bug of skipping the cache for mutable objects entirely.
            unsigned long hash_idx = 0;
            if (cache) {
                hash_idx = (reinterpret_cast<uintptr_t>(currentValue) ^ name->getHash(context)) % THREAD_CACHE_DEPTH;
                if (cache[hash_idx].object == currentValue && cache[hash_idx].name == name) {
                    return cache[hash_idx].result;
                }
            }

            // Check OWN Attributes in currentValue
            if (proto::isObject(currentValue)) {
                auto ocValue = toImpl<const ProtoObjectCell>(currentValue);
                if (ocValue->attributes->implHas(context, attr_hash)) {
                    const auto* result = ocValue->attributes->implGetAt(context, attr_hash);
                    if (cache) {
                        cache[hash_idx] = {currentValue, result, name};
                    }
                    return result;
                }

                // Move to NEXT in linearized chain
                if (!currentLink) {
                    // We just checked 'this'. Now follow its parent links.
                    currentLink = ocValue->parent;
                } else {
                    // We are already in the middle of a ParentLink chain. Move to next link.
                    if (currentLink && ((uintptr_t)currentLink & 0x3F) == 0) {
                        auto cl = toImpl<const ParentLinkImplementation>(currentLink);
                        if (cl->getType() == CellType::ParentLink) {
                            currentLink = cl->getParent(context);
                        } else {
                            currentLink = nullptr;
                        }
                    } else {
                        currentLink = nullptr;
                    }
                }

                if (currentLink && ((uintptr_t)currentLink & 0x3F) == 0) {
                    auto cl = toImpl<const ParentLinkImplementation>(currentLink);
                    if (cl->getType() == CellType::ParentLink) {
                        currentPointer = cl->getObject(context);
                    } else {
                        currentPointer = nullptr;
                    }
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
        return nullptr;
    }

    const ProtoObject* ProtoObject::setAttribute(ProtoContext* context, const ProtoString* name, const ProtoObject* value) const {
        if (!this || !name) return this;

        // Auto-intern the key if it is a non-interned heap String (POINTER_TAG_STRING).
        // This ensures that the canonical symbol pointer is stored in the attribute
        // sparse list, so lookups using createSymbol() keys find the value by pointer.
        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                const ProtoObject* sym = context->space->symbolTable->intern(
                    context, reinterpret_cast<const ProtoObject*>(name), /*is_strong=*/false);
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

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

        if (!proto::isObject(this)) return this;
        auto* oc = toImpl<ProtoObjectCell>(this);

        // Handle Mutable Objects
        if (oc->mutable_ref > 0) {
             int shard = oc->mutable_ref % context->space->MUTABLE_ROOT_SHARDS;
             int casIteration = 0;
             while(true) {
                 if (++casIteration > 100) {
                     break; // Give up
                 }

                 // 1. Get current object state from the shard
                 const ProtoObject* currentObjState = this;
                 ProtoSparseList* oldRoot = context->space->mutableRoot[shard].root.load();
                 if (oldRoot != nullptr) {
                     const auto* currentMutableList = toImpl<const ProtoSparseListImplementation>(oldRoot);
                     const ProtoObject* storedState = currentMutableList->implGetAt(context, oc->mutable_ref);
                     if (storedState != nullptr) {
                          currentObjState = storedState;
                     }
                 }

                 // 2. Create new state with updated attribute
                 if (!proto::isObject(currentObjState)) {
                     return this; // Corruption detected or inconsistent state
                 }
                 auto* currentOc = toImpl<const ProtoObjectCell>(currentObjState);
                 const auto* newAttributes = currentOc->attributes->implSetAt(context, reinterpret_cast<uintptr_t>(name), value);

                 // CRITICAL: newState MUST have mutable_ref = 0 to avoid infinite loop during lookup
                 auto* newState = (new(context) ProtoObjectCell(context, currentOc->parent, newAttributes, 0))->asObject(context);

                 // 3. CAS only the shard that owns this mutable_ref
                 const auto* oldRootImpl = (oldRoot == nullptr) ?
                     toImpl<const ProtoSparseListImplementation>(context->newSparseList()) :
                     toImpl<const ProtoSparseListImplementation>(oldRoot);

                 auto* newRootImpl = oldRootImpl->implSetAt(context, oc->mutable_ref, newState);
                 ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(newRootImpl->asSparseList(context));
                 ProtoSparseList* expected = oldRoot;
                 if (context->space->mutableRoot[shard].root.compare_exchange_weak(expected, newRoot)) {
                     // Refresh per-thread cache so subsequent reads on this thread hit immediately.
                     refreshMutableCache(context, oc->mutable_ref, newRoot, newState);
                     break;
                 }
             }

             return this; // Return the same handle!
        }

        // Handle Immutable Objects (Copy-on-Write)
        const auto* newAttributes = oc->attributes->implSetAt(context, (uintptr_t)name, value);
        const ProtoObject* result = (new(context) ProtoObjectCell(context, oc->parent, newAttributes, 0))->asObject(context);
        return result;
    }

    const ProtoList* ProtoObject::getParents(ProtoContext* context) const {
        if (!proto::isObject(this)) return context->newList();
        const auto* oc = toImpl<const ProtoObjectCell>(this);

        // Handle Mutable Objects (cache-fast)
        if (oc->mutable_ref > 0) {
             const proto::ProtoObject* storedState =
                 resolveMutableSnapshot(context, oc->mutable_ref);
             if (storedState != nullptr && storedState != this) {
                 ProtoObjectPointer psa{};
                 psa.oid = storedState;
                 if (psa.op.pointer_tag == POINTER_TAG_OBJECT) {
                     oc = toImpl<const ProtoObjectCell>(storedState);
                 }
             }
        }

        const ProtoList* parents = context->newList();
        const ParentLinkImplementation* p = oc->parent;
        while (p && ((uintptr_t)p & 0x3F) == 0) {
            auto pl = toImpl<const ParentLinkImplementation>(p);
            if (pl->getType() != CellType::ParentLink) break;
            
            parents = parents->appendLast(context, pl->getObject(context));
            p = pl->getParent(context);
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
        if (!isCell(context)) return this;
        
        const auto* oc = toImpl<const ProtoObjectCell>(this);
        
        // Handle Mutable Objects
        if (oc->mutable_ref > 0) {
             int shard = oc->mutable_ref % context->space->MUTABLE_ROOT_SHARDS;
             int casIteration = 0;
             while(true) {
                 if (++casIteration > 100) {
                     break; // Give up
                 }

                 // 1. Get current object state from the shard
                 const ProtoObject* currentObjState = this;
                 ProtoSparseList* oldRoot = context->space->mutableRoot[shard].root.load();
                 if (oldRoot != nullptr) {
                     const auto* currentMutableList = toImpl<const ProtoSparseListImplementation>(oldRoot);
                     const ProtoObject* storedState = currentMutableList->implGetAt(context, oc->mutable_ref);
                     if (storedState != nullptr) {
                          currentObjState = storedState;
                     }
                 }

                 // 2. Create new state with added parent
                 auto* currentOc = toImpl<const ProtoObjectCell>(currentObjState);
                 auto* newState = currentOc->addParent(context, newParent)->asObject(context);

                 // 3. CAS only the shard that owns this mutable_ref
                 const auto* oldRootImpl = (oldRoot == nullptr) ?
                     toImpl<const ProtoSparseListImplementation>(context->newSparseList()) :
                     toImpl<const ProtoSparseListImplementation>(oldRoot);

                 auto* newRootImpl = oldRootImpl->implSetAt(context, oc->mutable_ref, newState);
                 ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(newRootImpl->asSparseList(context));
                 ProtoSparseList* expected = oldRoot;
                 if (context->space->mutableRoot[shard].root.compare_exchange_weak(expected, newRoot)) {
                     // Refresh per-thread cache so subsequent reads on this thread hit immediately.
                     refreshMutableCache(context, oc->mutable_ref, newRoot, newState);
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
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT || !ProtoObject::isCellPointer(newParent)) return this;
        
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
        return ProtoObject::isCellPointer(this);
    }

    const Cell* ProtoObject::asCell(ProtoContext* context) const {
        return ProtoObject::asCellPointer(this);
    }

    bool ProtoObject::isCellPointer(const ProtoObject* obj) {
        if (!obj) return false;
        ProtoObjectPointer pa{};
        pa.oid = obj;
        return pa.op.pointer_tag != POINTER_TAG_EMBEDDED_VALUE;
    }

    const Cell* ProtoObject::asCellPointer(const ProtoObject* obj) {
        if (!ProtoObject::isCellPointer(obj)) return nullptr;
        ProtoObjectPointer pa{};
        pa.oid = obj;
        uintptr_t raw = reinterpret_cast<uintptr_t>(pa.voidPointer) & ~0x3FUL;
        return reinterpret_cast<const Cell*>(raw);
    }

    const ProtoString* ProtoObject::asString(ProtoContext* context) const {
        if (!this) return nullptr;
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_STRING || pa.op.pointer_tag == POINTER_TAG_SYMBOL) return reinterpret_cast<const ProtoString*>(this);
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

    const ProtoObject* ProtoObject::asMethodSelf(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_METHOD ? toImpl<const ProtoMethodCell>(this)->implGetSelf(context) : nullptr;
    }

    bool ProtoObject::isNone(ProtoContext* context) const { return this == PROTO_NONE; }
    bool ProtoObject::isBoolean(ProtoContext* context) const { ProtoObjectPointer pa{}; pa.oid = this; return pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_BOOLEAN; }
    bool ProtoObject::isInteger(ProtoContext* context) const { return proto::isInteger(this); }
    bool ProtoObject::isString(ProtoContext* context) const {
        if (!this) return false;
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_STRING || pa.op.pointer_tag == POINTER_TAG_SYMBOL || (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_INLINE_STRING)) return true;
        
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

    bool ProtoObject::isByteBuffer(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_BYTE_BUFFER;
    }

    bool ProtoObject::isNativeRangeIterator(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = this;
        return pa.op.pointer_tag == POINTER_TAG_RANGE_ITERATOR;
    }

    const ProtoByteBuffer* ProtoObject::asByteBuffer(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_BYTE_BUFFER) return nullptr;
        return toImpl<const ProtoByteBufferImplementation>(this)->asByteBuffer(context);
    }

    const ProtoObject* ProtoObject::nextInNativeRange(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_RANGE_ITERATOR) return nullptr;
        pa.op.pointer_tag = 0;  // clear tag bits before pointer dereference (mirrors toImpl)
        auto* impl = const_cast<ProtoRangeIteratorImplementation*>(pa.rangeIteratorImplementation);
        return impl->implNext(context);
    }

    char* ProtoObject::getDataIfByteBuffer(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_BYTE_BUFFER) return nullptr;
        return toImpl<const ProtoByteBufferImplementation>(this)->implGetBuffer(context);
    }

    const ProtoObject* ProtoObject::getOwnAttributeDirect(ProtoContext* context, const ProtoString* name) const {
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return nullptr;
        auto oc = toImpl<const ProtoObjectCell>(this);
        const ProtoSparseListImplementation* attrs = oc->attributes;
        if (oc->mutable_ref > 0) {
            const ProtoObject* ss = resolveMutableSnapshot(context, oc->mutable_ref);
            if (ss != nullptr) {
                attrs = toImpl<const ProtoObjectCell>(ss)->attributes;
            }
        }
        return attrs->implGetAt(context, reinterpret_cast<uintptr_t>(name));
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
    int ProtoObject::integerSign(ProtoContext* context) const { return Integer::sign(context, this); }
    const ProtoString* ProtoObject::asIntegerString(ProtoContext* context, int base) const { return Integer::toString(context, this, base); }
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

        // Look up the canonical symbol for this key without inserting.
        // If the key was never interned, it was never used as an attribute key,
        // so the attribute cannot exist — return PROTO_FALSE immediately.
        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                const ProtoObject* sym = context->space->symbolTable->lookupByContent(
                    context, reinterpret_cast<const ProtoObject*>(name));
                if (!sym) return PROTO_FALSE;
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

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

            // Support for Mutable Objects (cache-fast)
            if (oc->mutable_ref > 0) {
                 const proto::ProtoObject* storedState =
                     resolveMutableSnapshot(context, oc->mutable_ref);
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

            if (attributes->implHas(context, attr_hash)) {
                return PROTO_TRUE;
            }

            // Multiple inheritance support:
            if (oc->parent && ((uintptr_t)oc->parent & 0x3F) == 0) {
                auto pl = toImpl<const ParentLinkImplementation>(oc->parent);
                if (pl->getType() == CellType::ParentLink) {
                    const ParentLinkImplementation* sibling = pl->getParent(context);
                    while (sibling && plPtr < 64 && ((uintptr_t)sibling & 0x3F) == 0) {
                        auto sl = toImpl<const ParentLinkImplementation>(sibling);
                        if (sl->getType() != CellType::ParentLink) break;
                        plStack[plPtr++] = sibling;
                        sibling = sl->getParent(context);
                    }
                    currentObject = pl->getObject(context);
                } else {
                    currentObject = nullptr;
                }
            } else {
                if (plPtr > 0) {
                    const ParentLinkImplementation* top = plStack[--plPtr];
                    if (top && ((uintptr_t)top & 0x3F) == 0) {
                        auto tl = toImpl<const ParentLinkImplementation>(top);
                        if (tl->getType() == CellType::ParentLink) {
                            currentObject = tl->getObject(context);
                        } else {
                            currentObject = nullptr;
                        }
                    } else {
                        currentObject = nullptr;
                    }
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
            const ProtoObject* storedState = resolveMutableSnapshot(context, oc->mutable_ref);
            if (storedState != nullptr) {
                auto* storedOc = toImpl<const ProtoObjectCell>(storedState);
                attributes = storedOc->attributes;
                oc = storedOc;
            }
        }

        const ProtoSparseList* attrs = attributes->asSparseList(context);
        if (oc->parent && ((uintptr_t)oc->parent & 0x3F) == 0) {
            auto pl = toImpl<const ParentLinkImplementation>(oc->parent);
            if (pl->getType() == CellType::ParentLink) {
                const ProtoObject* parentObj = pl->getObject(context);
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
            const ProtoObject* storedState = resolveMutableSnapshot(context, oc->mutable_ref);
            if (storedState != nullptr) {
                auto* storedOc = toImpl<const ProtoObjectCell>(storedState);
                attributes = storedOc->attributes;
            }
        }
        return attributes->asSparseList(context);
    }
    
    const ProtoObject* ProtoObject::hasOwnAttribute(ProtoContext* context, const ProtoString* name) const {
        // Look up the canonical symbol for this key without inserting.
        // If the key was never interned, it was never used as an attribute key,
        // so the attribute cannot exist — return PROTO_FALSE immediately.
        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                const ProtoObject* sym = context->space->symbolTable->lookupByContent(
                    context, reinterpret_cast<const ProtoObject*>(name));
                if (!sym) return PROTO_FALSE;
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return PROTO_FALSE;
        auto oc = toImpl<const ProtoObjectCell>(this);
        const ProtoSparseListImplementation* attributes = oc->attributes;

        if (oc->mutable_ref > 0) {
            const ProtoObject* storedState = resolveMutableSnapshot(context, oc->mutable_ref);
            if (storedState != nullptr) {
                auto* storedOc = toImpl<const ProtoObjectCell>(storedState);
                attributes = storedOc->attributes;
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
        if (isInteger(context)) return Integer::negate(context, this);
        if (isDouble(context)) return context->fromDouble(-asDouble(context));
        return this;
    }
    
    const ProtoObject* ProtoObject::abs(ProtoContext* context) const {
        if (isInteger(context)) return Integer::abs(context, this);
        if (isDouble(context)) {
            double val = asDouble(context);
            return context->fromDouble(val < 0 ? -val : val);
        }
        return this;
    }
    
    const ProtoObject* ProtoObject::bitwiseXor(ProtoContext* context, const ProtoObject* other) const {
        return Integer::bitwiseXor(context, this, other);
    }

} // namespace proto
