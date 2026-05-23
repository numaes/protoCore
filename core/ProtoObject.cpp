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
#include <thread>

namespace proto
{
    namespace {
        /**
         * Per-thread cache lookup for a mutable object's current snapshot AND the
         * shard root that produced it.  Returns both because writers (setAttribute,
         * addParentInternal) need the shard root pointer as the CAS `expected`
         * value, while readers only consume the snapshot.
         *
         * Validation: the cached entry is live iff `mutable_ref` matches AND the cached
         * `shard_root` pointer equals the current `mutableRoot[shard].root`. Any successful
         * CAS by any thread (including this one) replaces the shard root pointer, which
         * naturally invalidates stale entries on the next lookup.
         *
         * On miss, performs the authoritative load + AVL `implGetAt` and refreshes the
         * cache. `*outShardRoot` is filled with the LIVE shard root pointer regardless
         * of cache hit/miss. `*outCurrent` is the snapshot for `mutable_ref` in that
         * shard (nullptr when the object has not yet been mutated since allocation).
         */
        inline void resolveMutableState(ProtoContext* context,
                                          unsigned long mutable_ref,
                                          ProtoSparseList** outShardRoot,
                                          const ProtoObject** outCurrent) {
            // Hot path: pull the cache pointer directly from the
            // context's stashed slot.  Avoids the
            // `toImpl(context->thread) → threadImpl->extension →
            // mutableValueCache` chain (1 toImpl + 2 loads + 2
            // null-checks) that the previous code paid on EVERY
            // mutable read.  The slot is populated at context
            // construction and refreshed by ProtoThreadImplementation
            // when the extension is first wired up; nullptr in early
            // bootstrap / off-thread paths, falls into the slow path.
            MutableValueCacheEntry* cache =
                context ? context->mutableValueCache_ : nullptr;
            int shard = mutable_ref % ProtoSpace::MUTABLE_ROOT_SHARDS;
            unsigned long idx = mutable_ref % MUTABLE_VALUE_CACHE_DEPTH;

            // Validate cache entry by re-loading the live shard root.
            if (cache && cache[idx].mutable_ref == mutable_ref) {
                ProtoSparseList* live =
                    context->space->mutableRoot[shard].root.load(std::memory_order_relaxed);
                if (live == cache[idx].shard_root) {
                    if (outShardRoot) *outShardRoot = live;
                    if (outCurrent)   *outCurrent   = cache[idx].current_value;
                    return;
                }
            }

            // Authoritative resolve: live load + tag-dispatched raw read.
            // Nullptr-for-absent matters here so an entry whose value
            // happens to be PROTO_NONE (rare but legal) is not confused
            // with "no such mutable state".
            ProtoSparseList* live =
                context->space->mutableRoot[shard].root.load(std::memory_order_relaxed);
            const ProtoObject* snap = sparseListGetRaw(context, live, mutable_ref);

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
            if (outShardRoot) *outShardRoot = live;
            if (outCurrent)   *outCurrent   = snap;
        }

        /** Read-only convenience wrapper: returns just the current snapshot. */
        inline const ProtoObject* resolveMutableSnapshot(ProtoContext* context,
                                                         unsigned long mutable_ref) {
            const ProtoObject* snap = nullptr;
            resolveMutableState(context, mutable_ref, nullptr, &snap);
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
            // Same stashed-pointer fast-path as resolveMutableState:
            // skip the toImpl + extension chain, go straight to
            // context->mutableValueCache_.
            if (!context || !context->mutableValueCache_) return;
            unsigned long idx = mutable_ref % MUTABLE_VALUE_CACHE_DEPTH;
            context->mutableValueCache_[idx] = {mutable_ref, new_root, new_value};
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
    ) : Cell(context), parent(parent),
        attributes(attributes ? attributes : context->newSparseListImpl()),
        mutable_ref(mutable_ref)
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
     * @brief Replace the parent chain entirely.
     *
     * Builds a fresh ParentLink chain from `newParents` in reverse order
     * (the last list entry becomes the chain tail, the first entry the
     * chain head — matching getParents()'s emit order), then returns a
     * new ProtoObjectCell that shares this cell's attributes table but
     * uses the rebuilt chain.  Mutable-vs-immutable shard CAS happens
     * in the public ProtoObject::setParents trampoline; this helper is
     * purely the immutable-shape builder.
     *
     * Passing `newParents == nullptr` or an empty list clears the chain
     * (the resulting cell has no prototype).
     */
    const ProtoObjectCell* ProtoObjectCell::setParents(
        ProtoContext* context, const ProtoList* newParents) const
    {
        const ParentLinkImplementation* newChain = nullptr;
        if (newParents) {
            unsigned long n = newParents->getSize(context);
            // Walk right-to-left: getParents() emits in head-first
            // order, so to reproduce that we attach the LAST entry
            // first (it becomes the chain tail) and the FIRST entry
            // last (it becomes the chain head).
            for (long i = static_cast<long>(n) - 1; i >= 0; --i) {
                const ProtoObject* p = newParents->getAt(context, static_cast<int>(i));
                if (!p) continue;
                newChain = new(context) ParentLinkImplementation(
                    context, newChain, p);
            }
        }
        return new(context) ProtoObjectCell(
            context,
            newChain,
            this->attributes,
            0
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

        // Report the attribute list. `attributes` is now a raw
        // ProtoSparseListImplementation* (internal IMPL) — the same
        // type the GC tracer expects (Cell*-derived). No conversion.
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
        case POINTER_TAG_LIST_SMALL: return context->space->listPrototype;
        case POINTER_TAG_LIST_ITERATOR: return context->space->listIteratorPrototype;
        case POINTER_TAG_SPARSE_LIST: return context->space->sparseListPrototype;
        case POINTER_TAG_SPARSE_LIST_SMALL: return context->space->sparseListPrototype;
        case POINTER_TAG_SPARSE_LIST_ITERATOR: return context->space->sparseListIteratorPrototype;
        case POINTER_TAG_TUPLE: return context->space->tuplePrototype;
        case POINTER_TAG_TUPLE_ITERATOR: return context->space->tupleIteratorPrototype;
        case POINTER_TAG_STRING: return context->space->stringPrototype;
        case POINTER_TAG_SYMBOL: return context->space->stringPrototype;
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
        // GC critical section: this expression allocates three cells
        // (the empty SparseList, the ParentLinkImplementation, and the
        // outer ProtoObjectCell) in a single statement.  Argument
        // evaluation order is unspecified and the temporaries live in
        // the C++ stack between sub-expression results — none of them
        // are reachable from a GC root until the surrounding
        // ProtoObjectCell finishes constructing and links the chain
        // back together.  Without the guard a concurrent STW root scan
        // would observe a partial chain as candidate-but-unreachable
        // and sweep would free the SparseList or ParentLink under us.
        ProtoContext::CriticalSection cs(context);
        auto* newObject = new(context) ProtoObjectCell(context, new(context) ParentLinkImplementation(context, oc->parent, this), context->newSparseListImpl(), ref);
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
        if (!this || !name || !context) return nullptr;

        // Look up the canonical symbol for this key without inserting.
        // If the key was never interned, it was never used as an attribute key,
        // so the attribute cannot exist — return PROTO_NONE for "not found"
        // (the convention across the API; nullptr is reserved for invalid
        // input, not "missing").
        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                const ProtoObject* sym = context->space->symbolTable->lookupByContent(
                    context, reinterpret_cast<const ProtoObject*>(name));
                if (!sym) return PROTO_NONE;
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

        // Attribute Cache Setup.  ProtoThreadExtension::processReferences
        // pins every (object, result, name) entry as a GC root, so the
        // pointers in the cache cannot dangle: the cells they reference
        // stay alive, and the arena cannot recycle their addresses while
        // the cache holds them.  No GC-cycle invalidation is needed —
        // entries are naturally retired by hash-slot eviction on later
        // misses.
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

            // Pure 6-bit tag check — POINTER_TAG_OBJECT is 0, so
            // alignment-clear low bits identifies an object cell.
            // No virtual getCellTypeRaw() probe needed: every cell
            // the chain navigation reaches is invariantly a
            // ProtoObjectCell or a tagged primitive, and tagged
            // primitives are filtered here by the non-zero low bits.
            // For the FIRST iteration (currentPointer == this) the
            // caller may have passed any value — including primitives
            // like SmallInteger that go to getPrototype below — so
            // we still need the branch.  Subsequent iterations
            // visit either a getPrototype result (an *Prototype
            // ProtoObjectCell, tag 0) or a ParentLink->object
            // (always a ProtoObjectCell), so the tag check on those
            // is a single bit-and that the predictor learns to
            // handle as taken-not-taken in the steady state.
            const bool isObj = (reinterpret_cast<uintptr_t>(currentPointer) & 0x3FUL) == POINTER_TAG_OBJECT;
            if (!isObj) {
                // Non-object tagged pointers (e.g. Integers, None) — fall
                // through to their prototype.  Move out of the hot
                // attribute-lookup path entirely.
                const ProtoObject* nextProto = currentPointer->getPrototype(context);
                if (nextProto == currentPointer) break;
                currentPointer = nextProto;
                continue;
            }

            const ProtoObject* currentValue = currentPointer;
            auto oc = toImpl<const ProtoObjectCell>(currentPointer);
            auto ocValue = oc;  // Default: immutable case → currentValue == currentPointer.
            if (oc->mutable_ref > 0) {
                const proto::ProtoObject* storedState =
                    resolveMutableSnapshot(context, oc->mutable_ref);
                if (storedState != nullptr) {
                    currentValue = storedState;
                    ocValue = toImpl<const ProtoObjectCell>(currentValue);
                }
            }

            // Cache keyed on currentValue (the resolved immutable snapshot).
            // For immutable objects currentValue == currentPointer, so behaviour is unchanged.
            // For mutable objects currentValue is the snapshot stored in mutableRoot; when the
            // object mutates mutableRoot is updated to a new snapshot pointer, so the next lookup
            // gets a different currentValue → natural cache miss → correct re-lookup.
            //
            // hash_idx uses the name's pointer identity as the hash
            // component: attribute keys are auto-interned (perennial)
            // symbols (SymbolTable::intern), so two names with the same
            // content always share a pointer.  Avoids the
            // cross-DSO `name->getHash(context)` call, which on
            // rope-backed symbols re-traverses the tree and was costing
            // ~5 % of CPU per lookup before this change.  Right-shift by
            // 4 to spread the low alignment-zero bits into the index
            // range.
            unsigned long hash_idx = 0;
            bool cache_resolved = false;
            const proto::ProtoObject* result = nullptr;
            if (cache) {
                hash_idx = (reinterpret_cast<uintptr_t>(currentValue) ^
                              (reinterpret_cast<uintptr_t>(name) >> 4)) % THREAD_CACHE_DEPTH;
                if (cache[hash_idx].object == currentValue && cache[hash_idx].name == name) {
                    // Hit.  Cache stores OWN-attribute facts only:
                    //   result != nullptr → currentValue owns name with that value.
                    //   result == nullptr → currentValue confirmed-missing.
                    // Either way, this step is resolved without an AVL probe.
                    result = cache[hash_idx].result;
                    cache_resolved = true;
                }
            }

            // Check OWN attributes in currentValue.  ocValue was
            // computed up-front above (re-using `oc` in the immutable
            // case to avoid a redundant toImpl call on the hot path).
            // Single `implGetAt` walk: it already returns nullptr for
            // "not found", so calling `implHas` first would just walk
            // the AVL twice.
            if (!cache_resolved) {
                // Direct IMPL access: `attributes` is now a raw
                // ProtoSparseListImplementation* (AVL form), so the
                // lookup is a single C++ call into the AVL probe with
                // no toImpl, no tag dispatch. `implGetAt` returns
                // nullptr for absent keys and the actual value
                // (possibly PROTO_NONE) for present keys — the
                // distinction `x = None` vs `hasattr(x)` is preserved.
                result = ocValue->attributes
                    ? ocValue->attributes->implGetAt(context, attr_hash)
                    : nullptr;
                if (cache) {
                    // Persist the own-fact (positive OR negative).  A
                    // cached miss prevents the next chain walk from
                    // re-probing this step's AVL — the dominant cost
                    // for inherited-attribute lookups (a 10-level
                    // walk would otherwise pay 10 × implGetAt every
                    // time, even when the answer is "not here, walk
                    // up").  Pinning is via
                    // ProtoThreadExtension::processReferences, which
                    // traces all three slots of every cache entry as
                    // GC roots so neither object, name, nor result
                    // can be reclaimed while the entry is live.
                    cache[hash_idx] = {currentValue, result, name};
                }
            }
            if (result != nullptr) {
                return result;
            }

            // Move to NEXT in linearised chain.  First iteration
            // (currentLink == nullptr) starts from this object's
            // parent link; subsequent iterations dereference the
            // previous link's `parent` field directly.
            //
            // We skip the per-step `getType() == ParentLink` virtual
            // probe — every cell that the parent / parent->parent
            // chain reaches IS a ParentLinkImplementation by
            // construction (newChild and addParent only ever produce
            // those), so the cell-alignment check (`& 0x3F == 0`,
            // ruling out tagged pointers) is the only safety filter
            // we need here.  Saved cost: 2 virtual calls per chain
            // step.  On a 10-level inheritance walk that is ~20 ns
            // per call, exactly the gap between the post-revert
            // ~89 ns and the pre-revert ~37 ns 10-level numbers.
            const ParentLinkImplementation* nextLink =
                (currentLink == nullptr) ? ocValue->parent : currentLink->parent;
            if (nextLink && ((uintptr_t)nextLink & 0x3F) == 0) {
                currentPointer = nextLink->object;
                currentLink = nextLink;
            } else {
                currentPointer = nullptr;
                currentLink = nullptr;
            }
        }
        // Full prototype chain searched, attribute not found.  Return
        // PROTO_NONE per the API convention (nullptr is reserved for
        // invalid inputs at the top of this function).
        return PROTO_NONE;
    }

    const ProtoObject* ProtoObject::setAttribute(ProtoContext* context, const ProtoString* name, const ProtoObject* value) const {
        if (!this || !name) return this;

        // Auto-intern the key if it is a non-interned heap String (POINTER_TAG_STRING).
        //
        // Intern STRONGLY: the resulting Symbol becomes a permanent
        // root so that the attribute SparseList — which stores the
        // Symbol *pointer* as its key — can never see that pointer
        // dangle.  A weak intern would be reclaimed by the next GC
        // cycle (the SparseList's processReferences only traces
        // `value`, not `key`), the SymbolTable bucket would still
        // hold the freed pointer, and the next createSymbol() with
        // the same content would return a fresh pointer that no
        // longer matches the SparseList key — a missed lookup that
        // surfaces as `Array.isArray === undefined` after a few
        // hundred async operations have triggered a GC.
        //
        // Strong-interning attribute names is bounded by the
        // program's vocabulary; in practice the cost is negligible
        // and identical to manually pre-interning every key.
        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                const ProtoObject* sym = context->space->symbolTable->intern(
                    context, reinterpret_cast<const ProtoObject*>(name));
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

        // 1. Invalidate Cache
        if (context->thread) {
            auto* threadImpl = toImpl<ProtoThreadImplementation>(context->thread);
            if (threadImpl->extension) {
                unsigned long hash_idx = (reinterpret_cast<uintptr_t>(this) ^
                                            (reinterpret_cast<uintptr_t>(name) >> 4)) % THREAD_CACHE_DEPTH;
                if (threadImpl->extension->attributeCache[hash_idx].object == this &&
                    threadImpl->extension->attributeCache[hash_idx].name == name) {
                    threadImpl->extension->attributeCache[hash_idx] = {nullptr, nullptr, nullptr};
                }
            }
        }

        if (!proto::isObjectFast(this)) return this;
        auto* oc = toImpl<ProtoObjectCell>(this);

        // Handle Mutable Objects
        if (oc->mutable_ref > 0) {
             int shard = oc->mutable_ref % context->space->MUTABLE_ROOT_SHARDS;
             // Retry until the CAS succeeds.  Silently bailing out after a
             // fixed iteration cap loses the user's write — under enough
             // cross-thread contention (multiple JS deferreds resolving
             // concurrently and each updating an attribute on the global
             // mutable object) we hit that cap, which manifested as
             // attributes like Array.isArray reading back as undefined
             // because they were never installed.  Add a tiny pause every
             // few rounds so the writers don't burn CPU livelocking each
             // other on the same shard.
             //
             // GC critical section: every iteration of this loop allocates
             // a new SparseList tree (newAttributes), a new ProtoObjectCell
             // (newState), and a new outer SparseList tree (newRootImpl)
             // BEFORE any of them are reachable from a GC root — the only
             // root publish is the final compare_exchange_weak on
             // mutableRoot[shard].root.  Without the guard the per-context
             // allocation-threshold submission can land in dirtySegments
             // mid-construction and a concurrent STW + sweep would free
             // the half-built tree under the running mutator.
             ProtoContext::CriticalSection cs(context);
             int casIteration = 0;
             while (true) {
                 ++casIteration;

                 // 1. Get current object state from the shard.  Use the
                 //    per-thread mutable-value cache: on the common case
                 //    where this thread (or another that already validated
                 //    the same shard root) has the live snapshot cached,
                 //    this avoids the AVL implGetAt traversal entirely.
                 //    `resolveMutableState` validates the cache by
                 //    re-loading the shard root and invalidating stale
                 //    entries; on miss it falls back to the authoritative
                 //    load + AVL and refreshes the cache.
                 const ProtoObject* currentObjState = this;
                 ProtoSparseList* oldRoot = nullptr;
                 const ProtoObject* storedState = nullptr;
                 resolveMutableState(context, oc->mutable_ref, &oldRoot, &storedState);
                 if (storedState != nullptr) {
                     currentObjState = storedState;
                 }

                 // 2. Create new state with updated attribute
                 if (!proto::isObjectFast(currentObjState)) {
                     return this; // Corruption detected or inconsistent state
                 }
                 auto* currentOc = toImpl<const ProtoObjectCell>(currentObjState);
                 // Direct IMPL setAt: returns a new AVL impl pointer
                 // (raw, untagged), suitable for storage in the next
                 // ProtoObjectCell's `attributes` field.
                 const ProtoSparseListImplementation* newAttributes =
                     currentOc->attributes->implSetAt(context, reinterpret_cast<uintptr_t>(name), value);

                 // CRITICAL: newState MUST have mutable_ref = 0 to avoid infinite loop during lookup
                 auto* newState = (new(context) ProtoObjectCell(context, currentOc->parent, newAttributes, 0))->asObject(context);

                 // 3. CAS only the shard that owns this mutable_ref.
                 // The shard root is always handled via the public API so
                 // it transparently uses Small while the thread / mutable
                 // population stays small (≤ 3 references).
                 const ProtoSparseList* oldRootSL =
                     (oldRoot == nullptr) ? context->newSparseList()
                                          : oldRoot;
                 ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(
                     oldRootSL->setAt(context, oc->mutable_ref, newState));
                 ProtoSparseList* expected = oldRoot;
                 if (context->space->mutableRoot[shard].root.compare_exchange_weak(expected, newRoot)) {
                     // Refresh per-thread cache so subsequent reads on this thread hit immediately.
                     refreshMutableCache(context, oc->mutable_ref, newRoot, newState);
                     break;
                 }
                 // CAS lost — another writer beat us; back off briefly
                 // every 32 retries so we don't livelock on the same
                 // shard.  std::this_thread::yield is cheap and gives
                 // contending writers a chance to make progress.
                 if ((casIteration & 31) == 0) {
                     std::this_thread::yield();
                 }
             }

             return this; // Return the same handle!
        }

        // Handle Immutable Objects (Copy-on-Write).  Same critical-section
        // discipline as the mutable branch: newAttributes (a SparseList
        // tree) and the surrounding ProtoObjectCell are allocated and only
        // reachable through this function's return value once both are
        // wired up; sweeping mid-build would orphan them.
        ProtoContext::CriticalSection cs(context);
        const ProtoSparseListImplementation* newAttributes =
            oc->attributes->implSetAt(context, (uintptr_t)name, value);
        const ProtoObject* result = (new(context) ProtoObjectCell(context, oc->parent, newAttributes, 0))->asObject(context);
        return result;
    }

    bool ProtoObject::setAttributeIfEqual(ProtoContext* context, const ProtoString* name,
                                          const ProtoObject* expected,
                                          const ProtoObject* newValue) const {
        if (!this || !name) return false;

        // Auto-intern the key exactly as setAttribute does: the attribute
        // SparseList is keyed by the Symbol *pointer*, so a non-interned heap
        // String must be canonicalised first or the key would never match.
        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                const ProtoObject* sym = context->space->symbolTable->intern(
                    context, reinterpret_cast<const ProtoObject*>(name));
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

        if (!proto::isObjectFast(this)) return false;
        auto* oc = toImpl<ProtoObjectCell>(this);

        // A compare-and-swap on an attribute is only meaningful for a mutable
        // receiver — an immutable object's attributes can never change, so
        // the swap can never legitimately succeed.
        if (oc->mutable_ref == 0) return false;

        // Invalidate this thread's attribute cache for (this, name): a
        // successful swap changes the resolved value, mirroring setAttribute.
        if (context->thread) {
            auto* threadImpl = toImpl<ProtoThreadImplementation>(context->thread);
            if (threadImpl->extension) {
                unsigned long hash_idx = (reinterpret_cast<uintptr_t>(this) ^
                                            (reinterpret_cast<uintptr_t>(name) >> 4)) % THREAD_CACHE_DEPTH;
                if (threadImpl->extension->attributeCache[hash_idx].object == this &&
                    threadImpl->extension->attributeCache[hash_idx].name == name) {
                    threadImpl->extension->attributeCache[hash_idx] = {nullptr, nullptr, nullptr};
                }
            }
        }

        const int shard = oc->mutable_ref % context->space->MUTABLE_ROOT_SHARDS;
        const unsigned long key = reinterpret_cast<uintptr_t>(name);

        // Same critical-section discipline as setAttribute: every iteration
        // builds a new attribute tree + ProtoObjectCell + shard SparseList
        // that are unreachable from any GC root until the final
        // compare_exchange_weak publishes them.
        ProtoContext::CriticalSection cs(context);
        int casIteration = 0;
        while (true) {
            ++casIteration;

            // Resolve the live snapshot of this mutable object.
            const ProtoObject* currentObjState = this;
            ProtoSparseList* oldRoot = nullptr;
            const ProtoObject* storedState = nullptr;
            resolveMutableState(context, oc->mutable_ref, &oldRoot, &storedState);
            if (storedState != nullptr) {
                currentObjState = storedState;
            }
            if (!proto::isObjectFast(currentObjState)) {
                return false; // inconsistent state — treat as CAS failure
            }
            auto* currentOc = toImpl<const ProtoObjectCell>(currentObjState);

            // The current OWN value of `name` (nullptr == attribute absent).
            const ProtoObject* currentValue = currentOc->attributes
                ? currentOc->attributes->implGetAt(context, key)
                : nullptr;

            // Precondition: the attribute must still hold `expected`.  A
            // mismatch is a genuine concurrent write — report failure so the
            // caller can re-read and rebuild.  This is checked on every
            // retry, so a CAS lost to an *unrelated* shard write re-validates
            // it rather than blindly overwriting.
            if (currentValue != expected) {
                return false;
            }

            // Build the new snapshot with name := newValue.
            const ProtoSparseListImplementation* newAttributes =
                currentOc->attributes->implSetAt(context, key, newValue);
            auto* newState = (new(context) ProtoObjectCell(
                context, currentOc->parent, newAttributes, 0))->asObject(context);

            const ProtoSparseList* oldRootSL =
                (oldRoot == nullptr) ? context->newSparseList() : oldRoot;
            ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(
                oldRootSL->setAt(context, oc->mutable_ref, newState));
            ProtoSparseList* expectedRoot = oldRoot;
            if (context->space->mutableRoot[shard].root.compare_exchange_weak(expectedRoot, newRoot)) {
                refreshMutableCache(context, oc->mutable_ref, newRoot, newState);
                return true;
            }
            // CAS lost to a concurrent shard write; back off occasionally and
            // retry — the loop re-validates `expected` against the new state.
            if ((casIteration & 31) == 0) {
                std::this_thread::yield();
            }
        }
    }

    const ProtoObject* ProtoObject::removeAttribute(ProtoContext* context, const ProtoString* name) const {
        if (!this || !name) return this;

        // Auto-intern the key the same way setAttribute does, so the
        // SparseList key (Symbol pointer cast to ulong) matches the slot
        // setAttribute would have written.  A non-interned heap String
        // produces a different ProtoString* than the interned one, and
        // would silently miss the entry — turning every del into a no-op.
        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                const ProtoObject* sym = context->space->symbolTable->intern(
                    context, reinterpret_cast<const ProtoObject*>(name));
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

        // Invalidate any cached attribute hit on this thread for (this, name)
        // — same shape as setAttribute's invalidation path.  Stale cache
        // entries would resurrect the removed attribute on the next read.
        if (context->thread) {
            auto* threadImpl = toImpl<ProtoThreadImplementation>(context->thread);
            if (threadImpl->extension) {
                unsigned long hash_idx = (reinterpret_cast<uintptr_t>(this) ^
                                            (reinterpret_cast<uintptr_t>(name) >> 4)) % THREAD_CACHE_DEPTH;
                if (threadImpl->extension->attributeCache[hash_idx].object == this &&
                    threadImpl->extension->attributeCache[hash_idx].name == name) {
                    threadImpl->extension->attributeCache[hash_idx] = {nullptr, nullptr, nullptr};
                }
            }
        }

        if (!proto::isObjectFast(this)) return this;
        auto* oc = toImpl<ProtoObjectCell>(this);

        // Mutable path: same CAS structure as setAttribute, but the new
        // attribute table is built via SparseList::removeAt instead of setAt.
        if (oc->mutable_ref > 0) {
             int shard = oc->mutable_ref % context->space->MUTABLE_ROOT_SHARDS;
             ProtoContext::CriticalSection cs(context);
             int casIteration = 0;
             while (true) {
                 ++casIteration;

                 const ProtoObject* currentObjState = this;
                 ProtoSparseList* oldRoot = nullptr;
                 const ProtoObject* storedState = nullptr;
                 resolveMutableState(context, oc->mutable_ref, &oldRoot, &storedState);
                 if (storedState != nullptr) {
                     currentObjState = storedState;
                 }

                 if (!proto::isObjectFast(currentObjState)) {
                     return this;
                 }
                 auto* currentOc = toImpl<const ProtoObjectCell>(currentObjState);

                 // No-op fast path: if the name isn't present as OWN, skip
                 // the allocation+CAS entirely and return `this` unchanged.
                 // Matches the "no allocation if no change" contract callers
                 // can rely on for cheap idempotent del.
                 if (!currentOc->attributes->implHas(context, reinterpret_cast<uintptr_t>(name))) {
                     return this;
                 }

                 const ProtoSparseListImplementation* newAttributes =
                     currentOc->attributes->implRemoveAt(context, reinterpret_cast<uintptr_t>(name));

                 auto* newState = (new(context) ProtoObjectCell(context, currentOc->parent, newAttributes, 0))->asObject(context);

                 const ProtoSparseList* oldRootSL =
                     (oldRoot == nullptr) ? context->newSparseList()
                                          : oldRoot;
                 ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(
                     oldRootSL->setAt(context, oc->mutable_ref, newState));
                 ProtoSparseList* expected = oldRoot;
                 if (context->space->mutableRoot[shard].root.compare_exchange_weak(expected, newRoot)) {
                     refreshMutableCache(context, oc->mutable_ref, newRoot, newState);
                     break;
                 }
                 if ((casIteration & 31) == 0) {
                     std::this_thread::yield();
                 }
             }

             return this;
        }

        // Immutable path: copy-on-write.  No-op when the name isn't OWN.
        if (!oc->attributes->implHas(context, reinterpret_cast<uintptr_t>(name))) {
            return this;
        }
        ProtoContext::CriticalSection cs(context);
        const ProtoSparseListImplementation* newAttributes =
            oc->attributes->implRemoveAt(context, reinterpret_cast<uintptr_t>(name));
        const ProtoObject* result = (new(context) ProtoObjectCell(context, oc->parent, newAttributes, 0))->asObject(context);
        return result;
    }

    const ProtoObject* ProtoObject::getFirstParent(ProtoContext* context) const {
        if (!proto::isObjectFast(this)) return PROTO_NONE;
        const auto* oc = toImpl<const ProtoObjectCell>(this);

        // Resolve mutable to its current snapshot — same logic getParents uses.
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

        if (oc->parent && ((uintptr_t)oc->parent & 0x3F) == 0) {
            auto pl = toImpl<const ParentLinkImplementation>(oc->parent);
            if (pl->getType() == CellType::ParentLink) return pl->getObject(context);
        }
        return PROTO_NONE;
    }

    const ProtoList* ProtoObject::getParents(ProtoContext* context) const {
        if (!proto::isObjectFast(this)) return context->newList();
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

        // Handle Mutable Objects.  See setAttribute (above) for the
        // rationale on the unbounded-retry-with-backoff loop: an early
        // cap silently dropped writes when shards were contended.
        if (oc->mutable_ref > 0) {
             int shard = oc->mutable_ref % context->space->MUTABLE_ROOT_SHARDS;
             // GC critical section: every iteration allocates a new
             // ProtoObjectCell + a new ParentLinkImplementation + a new
             // outer SparseList tree before any of them are reachable
             // from a GC root — only the final compare_exchange_weak on
             // mutableRoot[shard].root publishes the new state.  Same
             // discipline as setAttribute (see core/ProtoObject.cpp
             // mutable branch); without it a concurrent STW root scan
             // would observe the half-built tree as candidate-but-
             // unreachable and sweep would free it.
             ProtoContext::CriticalSection cs(context);
             int casIteration = 0;
             while (true) {
                 ++casIteration;

                 // 1. Get current object state from the shard via the
                 //    per-thread mutable-value cache (see setAttribute).
                 const ProtoObject* currentObjState = this;
                 ProtoSparseList* oldRoot = nullptr;
                 const ProtoObject* storedState = nullptr;
                 resolveMutableState(context, oc->mutable_ref, &oldRoot, &storedState);
                 if (storedState != nullptr) {
                     currentObjState = storedState;
                 }

                 // 2. Create new state with added parent
                 auto* currentOc = toImpl<const ProtoObjectCell>(currentObjState);
                 auto* newState = currentOc->addParent(context, newParent)->asObject(context);

                 // 3. CAS only the shard that owns this mutable_ref via
                 // the public API so the small / AVL forms are honored.
                 const ProtoSparseList* oldRootSL =
                     (oldRoot == nullptr) ? context->newSparseList() : oldRoot;
                 ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(
                     oldRootSL->setAt(context, oc->mutable_ref, newState));
                 ProtoSparseList* expected = oldRoot;
                 if (context->space->mutableRoot[shard].root.compare_exchange_weak(expected, newRoot)) {
                     // Refresh per-thread cache so subsequent reads on this thread hit immediately.
                     refreshMutableCache(context, oc->mutable_ref, newRoot, newState);
                     break;
                 }
                 if ((casIteration & 31) == 0) {
                     std::this_thread::yield();
                 }
             }

             return this; // Return the same handle
        }

        // Immutable branch: build a new ProtoObjectCell with the added
        // parent.  The intermediate ParentLinkImplementation cell is
        // held in the constructor's argument list across the
        // ProtoObjectCell allocation; both must stay reachable until the
        // result is returned to the caller.  Critical section forbids
        // STW root scan during construction.
        ProtoContext::CriticalSection cs(context);
        return oc->addParent(context, newParent)->asObject(context);
    }

    const ProtoObject* ProtoObject::addParent(ProtoContext* context, const ProtoObject* newParent) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT || !ProtoObject::isCellPointer(newParent)) return this;

        // GC critical section: the loop below holds `result` (a
        // ProtoObject* returned by addParentInternal) in a C++ local
        // across multiple allocations — every iteration of the
        // ancestors loop allocates new cells via the inner
        // addParentInternal.  Without the guard a concurrent STW
        // between iterations would observe `result` only via this C++
        // local (it is not yet in any GC root) and a sweep could free
        // its underlying cell.
        ProtoContext::CriticalSection cs(context);

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

    /**
     * @brief Replace the parent chain wholesale.
     *
     * For embedders that need true reassignment semantics (e.g. a
     * Python-level `__bases__` setter) — `addParent` only extends the
     * chain, which leaves stale ancestors visible to raw chain-walks.
     *
     * The trampoline structure mirrors `addParentInternal` /
     * `setAttribute`:
     *   - Mutable objects (`mutable_ref > 0`) take the same per-shard
     *     CAS loop used by mutating writes, returning the SAME handle
     *     so callers don't need to reseat references.
     *   - Immutable objects build a fresh ProtoObjectCell with the
     *     rebuilt chain and return its handle.
     *
     * Type tag check on `this` is the only sanity gate — non-cell
     * pointers (embedded ints/floats/bools) silently no-op like
     * addParent does.
     */
    const ProtoObject* ProtoObject::setParents(ProtoContext* context, const ProtoList* newParents) const {
        if (!this || !context) return this;
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return this;

        const auto* oc = toImpl<const ProtoObjectCell>(this);

        if (oc->mutable_ref > 0) {
            int shard = oc->mutable_ref % context->space->MUTABLE_ROOT_SHARDS;
            ProtoContext::CriticalSection cs(context);
            int casIteration = 0;
            while (true) {
                ++casIteration;

                const ProtoObject* currentObjState = this;
                ProtoSparseList* oldRoot = nullptr;
                const ProtoObject* storedState = nullptr;
                resolveMutableState(context, oc->mutable_ref, &oldRoot, &storedState);
                if (storedState != nullptr) {
                    currentObjState = storedState;
                }

                auto* currentOc = toImpl<const ProtoObjectCell>(currentObjState);
                auto* newState = currentOc->setParents(context, newParents)->asObject(context);

                const ProtoSparseList* oldRootSL =
                    (oldRoot == nullptr) ? context->newSparseList() : oldRoot;
                ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(
                    oldRootSL->setAt(context, oc->mutable_ref, newState));
                ProtoSparseList* expected = oldRoot;
                if (context->space->mutableRoot[shard].root.compare_exchange_weak(expected, newRoot)) {
                    refreshMutableCache(context, oc->mutable_ref, newRoot, newState);
                    break;
                }
                if ((casIteration & 31) == 0) {
                    std::this_thread::yield();
                }
            }

            return this; // mutable: same handle
        }

        // Immutable branch: build a new ProtoObjectCell with the
        // rebuilt parent chain.  The chain construction inside
        // ProtoObjectCell::setParents allocates one
        // ParentLinkImplementation per entry, then the wrapping
        // ProtoObjectCell — all reachable through the C++ stack only
        // until asObject() returns.  Critical section forbids STW
        // root scan during the build.
        ProtoContext::CriticalSection cs(context);
        return oc->setParents(context, newParents)->asObject(context);
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
        // Direct IMPL lookup. Preserves the nullptr-for-absent contract:
        // an attribute legitimately set to PROTO_NONE (Python's
        // `x = None`) returns PROTO_NONE here, not nullptr — callers
        // that want existence semantics use hasOwnAttribute.
        return attrs ? attrs->implGetAt(context, reinterpret_cast<uintptr_t>(name)) : nullptr;
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
        if (pa.op.pointer_tag == POINTER_TAG_SPARSE_LIST ||
            pa.op.pointer_tag == POINTER_TAG_SPARSE_LIST_SMALL) {
            return reinterpret_cast<const ProtoSparseList*>(this);
        }
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
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) return reinterpret_cast<const ProtoList*>(this);
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
        // Fast-path: if the name is already a SYMBOL (interned), the
        // pointer is canonical — skip the SymbolTable lookup, which
        // would otherwise hash the rope and compare bucket entries.
        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                const ProtoObject* sym = context->space->symbolTable->lookupByContent(
                    context, reinterpret_cast<const ProtoObject*>(name));
                if (!sym) return PROTO_FALSE;
                name = reinterpret_cast<const ProtoString*>(sym);
            }
            // POINTER_TAG_SYMBOL pointers are already canonical; nothing to do.
        }

        const ProtoObject* currentObject = this;
        const unsigned long attr_hash = reinterpret_cast<uintptr_t>(name);

        const ParentLinkImplementation* plStack[64];
        int plPtr = 0;
        int iterationCount = 0;

        // A ParentLink pointer is valid when it is non-null,
        // 64-byte-aligned (cell-aligned, low 6 bits = 0), and its
        // cell type is actually ParentLink.  Used in a few places
        // below where the original code repeated all three checks.
        auto validLink = [](const ParentLinkImplementation* l) -> bool {
            return l && ((uintptr_t)l & 0x3F) == 0 &&
                   l->getType() == CellType::ParentLink;
        };

        while (currentObject) {
            if (++iterationCount > 50) return PROTO_FALSE;

            // Inline isObjectFast: any other tagged pointer (Integer,
            // None, etc.) falls through to its prototype.
            if (!proto::isObjectFast(currentObject)) {
                currentObject = currentObject->getPrototype(context);
                continue;
            }
            auto oc = toImpl<const ProtoObjectCell>(currentObject);
            const ProtoSparseListImplementation* attributes = oc->attributes;

            // Support for Mutable Objects (cache-fast).  resolveMutableSnapshot
            // never returns a non-Object pointer for a valid mutable_ref, so
            // the previous tag re-check on the snapshot was redundant.
            if (oc->mutable_ref > 0) {
                 const proto::ProtoObject* storedState =
                     resolveMutableSnapshot(context, oc->mutable_ref);
                 if (storedState != nullptr && storedState != currentObject) {
                     auto* storedOc = toImpl<const ProtoObjectCell>(storedState);
                     attributes = storedOc->attributes;
                     oc = storedOc;
                 }
            }

            // Direct IMPL probe. nullptr means absent; anything else
            // (including PROTO_NONE) means present. Preserves the
            // distinction so `attr = None` → `hasattr(x, 'attr')` is
            // True (vs missing).
            if (attributes && attributes->implGetAt(context, attr_hash) != nullptr) {
                return PROTO_TRUE;
            }

            // Multiple inheritance support: walk the linearised chain
            // through `oc->parent`, while pushing any sibling links on
            // a small stack so they can be revisited after the main
            // chain is exhausted.
            if (validLink(oc->parent)) {
                const ParentLinkImplementation* sibling = oc->parent->getParent(context);
                while (validLink(sibling) && plPtr < 64) {
                    plStack[plPtr++] = sibling;
                    sibling = sibling->getParent(context);
                }
                currentObject = oc->parent->getObject(context);
            } else if (plPtr > 0) {
                const ParentLinkImplementation* top = plStack[--plPtr];
                currentObject = validLink(top) ? top->getObject(context) : nullptr;
            } else {
                currentObject = nullptr;
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

        const ProtoSparseListImplementation* attrs = attributes;
        if (oc->parent && ((uintptr_t)oc->parent & 0x3F) == 0) {
            auto pl = toImpl<const ParentLinkImplementation>(oc->parent);
            if (pl->getType() == CellType::ParentLink) {
                const ProtoObject* parentObj = pl->getObject(context);
                if (parentObj) {
                    // Recurse to the public API form (which other callers
                    // may also receive); convert back to IMPL for the
                    // merge loop below by walking via the Iterator API
                    // (which works on either form via tag dispatch).
                    const ProtoSparseList* parentAttrs = parentObj->getAttributes(context);
                // Merge parent attributes with own attributes.  GC critical
                // section: the loop below builds `attrs` incrementally and
                // every implSetAt allocates new SparseList nodes; the
                // partial tree is reachable only via this C++ local until
                // the final `return`.
                ProtoContext::CriticalSection cs(context);
                const ProtoSparseListIterator* it = parentAttrs->getIterator(context);
                while (it && it->hasNext(context)) {
                    unsigned long key = it->nextKey(context);
                    const ProtoObject* value = it->nextValue(context);
                    if (!attrs || attrs->implGetAt(context, key) == nullptr) {
                        attrs = attrs ? attrs->implSetAt(context, key, value)
                                      : new(context) ProtoSparseListImplementation(context, key, value, nullptr, nullptr, false);
                    }
                    it = const_cast<ProtoSparseListIterator*>(it)->advance(context);
                }
                }
            }
        }
        // Convert the IMPL pointer to the public API tagged handle at
        // the boundary — this is the trampoline that returns to the
        // user/external callers.
        return attrs ? attrs->asSparseList(context) : context->newSparseList();
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
        // Trampoline boundary: convert the internal IMPL pointer to the
        // public-API tagged handle.
        return attributes ? attributes->asSparseList(context) : context->newSparseList();
    }
    
    const ProtoObject* ProtoObject::hasOwnAttribute(ProtoContext* context, const ProtoString* name) const {
        // Look up the canonical symbol for this key without inserting.
        // If the key was never interned, it was never used as an attribute key,
        // so the attribute cannot exist — return PROTO_FALSE immediately.
        // Fast-path: if the name is already a SYMBOL (interned), the
        // pointer is canonical — skip the SymbolTable lookup, which
        // would otherwise hash the rope and compare bucket entries.
        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                const ProtoObject* sym = context->space->symbolTable->lookupByContent(
                    context, reinterpret_cast<const ProtoObject*>(name));
                if (!sym) return PROTO_FALSE;
                name = reinterpret_cast<const ProtoString*>(sym);
            }
            // POINTER_TAG_SYMBOL pointers are already canonical; nothing to do.
        }

        if (!proto::isObjectFast(this)) return PROTO_FALSE;
        auto oc = toImpl<const ProtoObjectCell>(this);
        const ProtoSparseListImplementation* attributes = oc->attributes;

        if (oc->mutable_ref > 0) {
            const ProtoObject* storedState = resolveMutableSnapshot(context, oc->mutable_ref);
            if (storedState != nullptr) {
                attributes = toImpl<const ProtoObjectCell>(storedState)->attributes;
            }
        }
        // Direct IMPL probe — an attribute set to PROTO_NONE is still
        // "present" (implGetAt returns PROTO_NONE, not nullptr).
        return context->fromBoolean(
            attributes && attributes->implGetAt(context, reinterpret_cast<uintptr_t>(name)) != nullptr);
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
