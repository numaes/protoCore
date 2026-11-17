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
#include "SparseListAlgorithms.h"
#include "Sweep.h"
#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define PROTO_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define PROTO_LIKELY(x) (x)
#endif

#ifdef PROTO_CACHE_STATS
#include <atomic>
#include <cstdio>
#include <cstdlib>
namespace proto {
    std::atomic<proto_ulong> protoCacheStats_hits{0};
    std::atomic<proto_ulong> protoCacheStats_collisions{0};
    std::atomic<proto_ulong> protoCacheStats_coldMisses{0};

    // Per-call view of getAttribute (the counters above are per chain step).
    // Depth buckets: found at step 0, 1, 2, 3-4, 5+, and not found.
    std::atomic<proto_ulong> protoCacheStats_calls{0};
    std::atomic<proto_ulong> protoCacheStats_steps{0};
    std::atomic<proto_ulong> protoCacheStats_mutableSteps{0};
    std::atomic<proto_ulong> protoCacheStats_callsNoAvl{0};
    std::atomic<proto_ulong> protoCacheStats_depth[6]{};
    std::atomic<proto_ulong> protoCacheStats_directHits{0};
    std::atomic<proto_ulong> protoCacheStats_directMisses{0};
    // Collisions where the evicted entry had the same name: a different
    // object (or a newer snapshot of the same mutable) reading the same
    // attribute, as opposed to two unrelated keys sharing a slot.
    std::atomic<proto_ulong> protoCacheStats_collisionsSameName{0};

    static void protoCacheStatsRecordCall(proto_ulong steps, proto_ulong avlSteps,
                                          proto_ulong mutableSteps, bool found) {
        protoCacheStats_calls.fetch_add(1, std::memory_order_relaxed);
        protoCacheStats_steps.fetch_add(steps, std::memory_order_relaxed);
        protoCacheStats_mutableSteps.fetch_add(mutableSteps, std::memory_order_relaxed);
        if (avlSteps == 0) protoCacheStats_callsNoAvl.fetch_add(1, std::memory_order_relaxed);
        const proto_ulong d = steps ? steps - 1 : 0;
        const int bucket = !found ? 5 : d <= 2 ? static_cast<int>(d) : d <= 4 ? 3 : 4;
        protoCacheStats_depth[bucket].fetch_add(1, std::memory_order_relaxed);
    }

    namespace {
        struct ProtoCacheStatsDumper {
            ~ProtoCacheStatsDumper() {
                proto_ulong h = protoCacheStats_hits.load();
                proto_ulong c = protoCacheStats_collisions.load();
                proto_ulong m = protoCacheStats_coldMisses.load();
                proto_ulong total = h + c + m;
                std::fprintf(stderr,
                    "[proto-cache-stats] lookups=%" PROTO_FMT_U " hits=%" PROTO_FMT_U " collisions=%" PROTO_FMT_U " coldMisses=%" PROTO_FMT_U " hitRate=%.2f%% collisionRate=%.2f%%\n",
                    total, h, c, m,
                    total ? (100.0 * h / total) : 0.0,
                    total ? (100.0 * c / total) : 0.0);
                std::fprintf(stderr,
                    "[proto-cache-stats] collisionsSameName=%" PROTO_FMT_U " (%.2f%% of collisions)\n",
                    protoCacheStats_collisionsSameName.load(),
                    c ? 100.0 * protoCacheStats_collisionsSameName.load() / c : 0.0);
                proto_ulong calls = protoCacheStats_calls.load();
                proto_ulong steps = protoCacheStats_steps.load();
                proto_ulong noAvl = protoCacheStats_callsNoAvl.load();
                auto pct = [calls](proto_ulong v) { return calls ? 100.0 * v / calls : 0.0; };
                std::fprintf(stderr,
                    "[proto-cache-stats] getAttribute calls=%" PROTO_FMT_U " stepsPerCall=%.2f mutableStepRate=%.2f%% callsWithoutAvl=%.2f%%"
                    " foundAt0=%.2f%% at1=%.2f%% at2=%.2f%% at3-4=%.2f%% at5+=%.2f%% notFound=%.2f%%\n",
                    calls, calls ? static_cast<double>(steps) / calls : 0.0,
                    steps ? 100.0 * protoCacheStats_mutableSteps.load() / steps : 0.0,
                    pct(noAvl),
                    pct(protoCacheStats_depth[0].load()), pct(protoCacheStats_depth[1].load()),
                    pct(protoCacheStats_depth[2].load()), pct(protoCacheStats_depth[3].load()),
                    pct(protoCacheStats_depth[4].load()), pct(protoCacheStats_depth[5].load()));
                proto_ulong dh = protoCacheStats_directHits.load();
                proto_ulong dm = protoCacheStats_directMisses.load();
                std::fprintf(stderr,
                    "[proto-cache-stats] getOwnAttributeDirect calls=%" PROTO_FMT_U " hitRate=%.2f%%\n",
                    dh + dm, (dh + dm) ? 100.0 * dh / (dh + dm) : 0.0);
            }
        };
        static ProtoCacheStatsDumper s_dumper;
    }
}
#endif

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
                                          proto_ulong mutable_ref,
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
            proto_ulong idx = mutable_ref % MUTABLE_VALUE_CACHE_DEPTH;

            // Validate cache entry by re-loading the live shard root.
            if (cache && cache[idx].mutable_ref == mutable_ref) {
                ProtoSparseList* live =
                    globalMutableShards[shard].root.load(std::memory_order_acquire);
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
                globalMutableShards[shard].root.load(std::memory_order_acquire);
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
                                                         proto_ulong mutable_ref) {
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
                                        proto_ulong mutable_ref,
                                        ProtoSparseList* new_root,
                                        const ProtoObject* new_value) {
            // Same stashed-pointer fast-path as resolveMutableState:
            // skip the toImpl + extension chain, go straight to
            // context->mutableValueCache_.
            if (!context || !context->mutableValueCache_) return;
            proto_ulong idx = mutable_ref % MUTABLE_VALUE_CACHE_DEPTH;
            context->mutableValueCache_[idx] = {mutable_ref, new_root, new_value};
        }

        /**
         * Resolves `node` (assumed to be a real ProtoObjectCell — callers
         * check `isObjectFast` first) to the ProtoObjectCell whose `parent`
         * field reflects its CURRENT state: for a mutable object this is
         * the live mutable snapshot, exactly as getAttribute / getParents /
         * getFirstParent resolve it. Unlike `getPrototype`, which reads the
         * handle cell's own (possibly stale, pre-first-mutation) `parent`
         * field directly, this always answers for the object's current
         * version — required for `newChild` (a mutable prototype's child
         * must capture the class's CURRENT ancestors, not its birth state)
         * and for `isInstanceOf`/`hasParent` (to see parents added via
         * `addParent`/`setParents` on a mutable receiver).
         */
        inline const ProtoObjectCell* resolveOwnCell(ProtoContext* context, const ProtoObject* node) {
            auto* oc = toImpl<const ProtoObjectCell>(node);
            if (oc->isMutableCell()) {
                const auto* moc = static_cast<const ProtoMutableObjectCell*>(oc);
                const ProtoObject* storedState = resolveMutableSnapshot(context, moc->mutable_ref);
                if (storedState != nullptr && storedState != node) {
                    ProtoObjectPointer psa{};
                    psa.oid = storedState;
                    if (psa.op.pointer_tag == POINTER_TAG_OBJECT) {
                        return toImpl<const ProtoObjectCell>(storedState);
                    }
                }
                if (moc->initialState != nullptr) {
                    return moc->initialState;
                }
            }
            return oc;
        }

        /**
         * Is `oc->parent` a genuine, well-formed ParentLink cell (as opposed
         * to absent, misaligned, or — impossible in practice, but checked
         * anyway — some other cell type)? The same alignment-plus-getType()
         * probe the chain-navigation loops use (chainHasAncestor above,
         * getAttribute's walk) rather than a bare truthy check on the raw
         * pointer.
         */
        inline bool hasValidParentLink(const ProtoObjectCell* oc) {
            if (!oc->parent || ((uintptr_t)oc->parent & 0x3F) != 0) return false;
            return toImpl<const ParentLinkImplementation>(oc->parent)->getType() == CellType::ParentLink;
        }

        inline AttributeCacheEntry* getShapeCache(ProtoContext* context) {
            if (!context) return nullptr;
            if (AttributeCacheEntry* c = shapeCacheOf(context->mutableValueCache_)) return c;
            if (context->thread) {
                auto* threadImpl = toImpl<ProtoThreadImplementation>(context->thread);
                if (threadImpl->extension) return threadImpl->extension->attributeCache;
            }
            return nullptr;
        }

        inline proto_ulong shapeCacheSlot(const ProtoShape* shape, const ProtoString* name) {
            const uint64_t h = (reinterpret_cast<uint64_t>(shape) * 0x9E3779B97F4A7C15ULL) ^
                               (reinterpret_cast<uint64_t>(name)  * 0xBF58476D1CE4E5B9ULL);
            return static_cast<proto_ulong>((h >> 54) ^ (h >> 32)) % THREAD_CACHE_DEPTH;
        }

        /**
         * Resolves the attribute offset (0 .. shape->count - 1) of `name` in
         * `shape`, or -1 when `name` is not in `shape`.
         *
         * Uses the per-thread shape cache keyed on (shape, name). On a cache
         * miss at the head `shape`, walks `b = shape; b != nullptr; b = b->next`;
         * if any suffix block `b` is already in cache, stops immediately with
         * that offset, and caches the resolved offset at the head `shape` (and
         * at the defining block `b` when found) without polluting the cache
         * with every intermediate block on deep chains.
         */
        inline proto_long findAttributeOffset(AttributeCacheEntry* cache,
                                              const ProtoShape* shape,
                                              const ProtoString* name,
                                              proto_ulong& outCount) {
            if (!shape) return -1;

            proto_ulong headIdx = 0;
            if (cache) {
                headIdx = shapeCacheSlot(shape, name);
                if (cache[headIdx].shape == shape && cache[headIdx].name == name) {
#ifdef PROTO_CACHE_STATS
                    ++protoCacheStats_hits;
#endif
                    outCount = cache[headIdx].count;
                    return cache[headIdx].offset;
                }
#ifdef PROTO_CACHE_STATS
                if (cache[headIdx].shape != nullptr) {
                    ++protoCacheStats_collisions;
                    if (cache[headIdx].name == name) ++protoCacheStats_collisionsSameName;
                } else {
                    ++protoCacheStats_coldMisses;
                }
#endif
            }

            outCount = shape->count;
            proto_long foundOffset = -1;
            const ProtoShape* foundBlock = nullptr;

            for (const ProtoShape* b = shape; b != nullptr; b = b->next) {
                if (b != shape && cache) {
                    const proto_ulong bIdx = shapeCacheSlot(b, name);
                    if (cache[bIdx].shape == b && cache[bIdx].name == name) {
                        foundOffset = cache[bIdx].offset;
                        break;
                    }
                }
                const proto_ulong baseOffset = (b->count - 1) & ~PROTO_UL(3);
                if (b->names[3] == name) { foundOffset = static_cast<proto_long>(baseOffset + 0); foundBlock = b; break; }
                if (b->names[2] == name) { foundOffset = static_cast<proto_long>(baseOffset + 1); foundBlock = b; break; }
                if (b->names[1] == name) { foundOffset = static_cast<proto_long>(baseOffset + 2); foundBlock = b; break; }
                if (b->names[0] == name) { foundOffset = static_cast<proto_long>(baseOffset + 3); foundBlock = b; break; }
            }

            if (cache) {
                if (foundBlock && foundBlock != shape) {
                    const proto_ulong fIdx = shapeCacheSlot(foundBlock, name);
                    cache[fIdx] = {foundBlock, name, foundOffset, foundBlock->count};
                }
                cache[headIdx] = {shape, name, foundOffset, outCount};
            }
            return foundOffset;
        }

        inline proto_long findAttributeOffset(AttributeCacheEntry* cache,
                                              const ProtoShape* shape,
                                              const ProtoString* name) {
            proto_ulong dummyCount = 0;
            return findAttributeOffset(cache, shape, name, dummyCount);
        }

        /**
         * Unrolled read of slot `offset` (0 <= offset < count) from `oc`.
         * Depths 0, 1, 2, 3 (count <= 4, 24, 144, 864) use straight-line code
         * with compile-time divisors (6, 36, 216); deeper trees use a loop.
         */
        inline const ProtoObject* getValueAt(const ProtoObjectCell* oc,
                                             proto_ulong count,
                                             proto_ulong offset) {
            if (PROTO_LIKELY(count <= 4)) {
                return oc->values[offset];
            }
            if (PROTO_LIKELY(count <= 24)) {
                const auto* leaf = static_cast<const ProtoAttrLeafCell*>(oc->blocks[offset / 6]);
                return leaf ? leaf->values[offset % 6] : nullptr;
            }
            if (PROTO_LIKELY(count <= 144)) {
                const auto* b1 = static_cast<const ProtoAttrBranchCell*>(oc->blocks[offset / 36]);
                if (!b1) return nullptr;
                const proto_ulong r1 = offset % 36;
                const auto* leaf = static_cast<const ProtoAttrLeafCell*>(b1->children[r1 / 6]);
                return leaf ? leaf->values[r1 % 6] : nullptr;
            }
            if (PROTO_LIKELY(count <= 864)) {
                const auto* b1 = static_cast<const ProtoAttrBranchCell*>(oc->blocks[offset / 216]);
                if (!b1) return nullptr;
                const proto_ulong r1 = offset % 216;
                const auto* b2 = static_cast<const ProtoAttrBranchCell*>(b1->children[r1 / 36]);
                if (!b2) return nullptr;
                const proto_ulong r2 = r1 % 36;
                const auto* leaf = static_cast<const ProtoAttrLeafCell*>(b2->children[r2 / 6]);
                return leaf ? leaf->values[r2 % 6] : nullptr;
            }
            proto_ulong step = 1296; // 6^4
            unsigned depth = 4;
            while (count > 4 * step) {
                step *= 6;
                ++depth;
            }
            const Cell* cur = oc->blocks[offset / step];
            proto_ulong rem = offset % step;
            for (unsigned level = 1; level < depth; ++level) {
                if (!cur) return nullptr;
                step /= 6;
                cur = static_cast<const ProtoAttrBranchCell*>(cur)->children[rem / step];
                rem %= step;
            }
            return cur ? static_cast<const ProtoAttrLeafCell*>(cur)->values[rem] : nullptr;
        }

        const Cell* setSubtreeAt(ProtoContext* context, const Cell* node, unsigned depth,
                                 proto_ulong step, proto_ulong rem, const ProtoObject* value) {
            if (depth == 1) {
                const auto* oldLeaf = static_cast<const ProtoAttrLeafCell*>(node);
                return oldLeaf
                    ? oldLeaf->withValueAt(context, rem, value)
                    : new(context) ProtoAttrLeafCell(context, rem, value);
            }
            const proto_ulong nextStep = step / 6;
            const proto_ulong idx = rem / nextStep;
            const proto_ulong nextRem = rem % nextStep;
            const auto* oldBranch = static_cast<const ProtoAttrBranchCell*>(node);
            const Cell* oldChild = oldBranch ? oldBranch->children[idx] : nullptr;
            const Cell* newChild = setSubtreeAt(context, oldChild, depth - 1, nextStep, nextRem, value);
            return oldBranch
                ? oldBranch->withChildAt(context, idx, newChild)
                : new(context) ProtoAttrBranchCell(context, idx, newChild);
        }

        /**
         * Unrolled path-copy setting slot `offset` to `value` on an object of
         * `count` attributes and `targetShape` (where `oc` already has the
         * same tree depth as `count`).
         */
        inline const ProtoObjectCell* setSlotAt(ProtoContext* context,
                                                const ProtoObjectCell* oc,
                                                const ProtoShape* targetShape,
                                                proto_ulong count,
                                                proto_ulong offset,
                                                const ProtoObject* value) {
            if (PROTO_LIKELY(count <= 4)) {
                const ProtoObject* v0 = (offset == 0) ? value : oc->values[0];
                const ProtoObject* v1 = (offset == 1) ? value : oc->values[1];
                const ProtoObject* v2 = (offset == 2) ? value : oc->values[2];
                const ProtoObject* v3 = (offset == 3) ? value : oc->values[3];
                return new(context) ProtoObjectCell(context, oc->parent, targetShape, v0, v1, v2, v3);
            }
            if (PROTO_LIKELY(count <= 24)) {
                const proto_ulong top = offset / 6;
                const proto_ulong rem = offset % 6;
                const auto* oldLeaf = static_cast<const ProtoAttrLeafCell*>(oc->blocks[top]);
                const auto* newLeaf = oldLeaf
                    ? oldLeaf->withValueAt(context, rem, value)
                    : new(context) ProtoAttrLeafCell(context, rem, value);
                const Cell* b0 = (top == 0) ? newLeaf : oc->blocks[0];
                const Cell* b1 = (top == 1) ? newLeaf : oc->blocks[1];
                const Cell* b2 = (top == 2) ? newLeaf : oc->blocks[2];
                const Cell* b3 = (top == 3) ? newLeaf : oc->blocks[3];
                return new(context) ProtoObjectCell(context, oc->parent, targetShape, b0, b1, b2, b3);
            }
            if (PROTO_LIKELY(count <= 144)) {
                const proto_ulong top = offset / 36;
                const proto_ulong r1 = offset % 36;
                const proto_ulong mid = r1 / 6;
                const proto_ulong rem = r1 % 6;
                const auto* oldB1 = static_cast<const ProtoAttrBranchCell*>(oc->blocks[top]);
                const auto* oldLeaf = oldB1 ? static_cast<const ProtoAttrLeafCell*>(oldB1->children[mid]) : nullptr;
                const auto* newLeaf = oldLeaf
                    ? oldLeaf->withValueAt(context, rem, value)
                    : new(context) ProtoAttrLeafCell(context, rem, value);
                const auto* newB1 = oldB1
                    ? oldB1->withChildAt(context, mid, newLeaf)
                    : new(context) ProtoAttrBranchCell(context, mid, newLeaf);
                const Cell* b0 = (top == 0) ? newB1 : oc->blocks[0];
                const Cell* b1 = (top == 1) ? newB1 : oc->blocks[1];
                const Cell* b2 = (top == 2) ? newB1 : oc->blocks[2];
                const Cell* b3 = (top == 3) ? newB1 : oc->blocks[3];
                return new(context) ProtoObjectCell(context, oc->parent, targetShape, b0, b1, b2, b3);
            }
            if (PROTO_LIKELY(count <= 864)) {
                const proto_ulong top = offset / 216;
                const proto_ulong r1 = offset % 216;
                const proto_ulong m1 = r1 / 36;
                const proto_ulong r2 = r1 % 36;
                const proto_ulong m2 = r2 / 6;
                const proto_ulong rem = r2 % 6;
                const auto* oldB1 = static_cast<const ProtoAttrBranchCell*>(oc->blocks[top]);
                const auto* oldB2 = oldB1 ? static_cast<const ProtoAttrBranchCell*>(oldB1->children[m1]) : nullptr;
                const auto* oldLeaf = oldB2 ? static_cast<const ProtoAttrLeafCell*>(oldB2->children[m2]) : nullptr;
                const auto* newLeaf = oldLeaf
                    ? oldLeaf->withValueAt(context, rem, value)
                    : new(context) ProtoAttrLeafCell(context, rem, value);
                const auto* newB2 = oldB2
                    ? oldB2->withChildAt(context, m2, newLeaf)
                    : new(context) ProtoAttrBranchCell(context, m2, newLeaf);
                const auto* newB1 = oldB1
                    ? oldB1->withChildAt(context, m1, newB2)
                    : new(context) ProtoAttrBranchCell(context, m1, newB2);
                const Cell* b0 = (top == 0) ? newB1 : oc->blocks[0];
                const Cell* b1 = (top == 1) ? newB1 : oc->blocks[1];
                const Cell* b2 = (top == 2) ? newB1 : oc->blocks[2];
                const Cell* b3 = (top == 3) ? newB1 : oc->blocks[3];
                return new(context) ProtoObjectCell(context, oc->parent, targetShape, b0, b1, b2, b3);
            }
            proto_ulong step = 1296;
            unsigned depth = 4;
            while (count > 4 * step) {
                step *= 6;
                ++depth;
            }
            const proto_ulong top = offset / step;
            const proto_ulong rem = offset % step;
            const Cell* newSub = setSubtreeAt(context, oc->blocks[top], depth, step, rem, value);
            const Cell* b0 = (top == 0) ? newSub : oc->blocks[0];
            const Cell* b1 = (top == 1) ? newSub : oc->blocks[1];
            const Cell* b2 = (top == 2) ? newSub : oc->blocks[2];
            const Cell* b3 = (top == 3) ? newSub : oc->blocks[3];
            return new(context) ProtoObjectCell(context, oc->parent, targetShape, b0, b1, b2, b3);
        }

        /**
         * Prepends `name` to `oldShape`, packing up to 4 names per ProtoShape cell
         * from `names[3]` down to `names[0]`.
         */
        inline const ProtoShape* extendShape(ProtoContext* context,
                                             const ProtoShape* oldShape,
                                             const ProtoString* name) {
            if (!oldShape) {
                return new(context) ProtoShape(context, 1, nullptr, nullptr, nullptr, nullptr, name);
            }
            const proto_ulong oldCount = oldShape->count;
            const proto_ulong newCount = oldCount + 1;
            const unsigned usedInHead = static_cast<unsigned>(((oldCount - 1) & 3UL) + 1);
            if (usedInHead < 4) {
                const ProtoString* n0 = (usedInHead == 3) ? name : oldShape->names[0];
                const ProtoString* n1 = (usedInHead == 2) ? name : oldShape->names[1];
                const ProtoString* n2 = (usedInHead == 1) ? name : oldShape->names[2];
                const ProtoString* n3 = oldShape->names[3];
                return new(context) ProtoShape(context, newCount, oldShape->next, n0, n1, n2, n3);
            }
            return new(context) ProtoShape(context, newCount, oldShape, nullptr, nullptr, nullptr, name);
        }

        /**
         * Appends a brand-new attribute value at `offset = oldCount` (`newCount = oldCount + 1`),
         * handling indirection level transitions (4 -> 5, 24 -> 25, 144 -> 145, 4*6^d -> 4*6^d + 1)
         * without copying existing full subtrees.
         */
        inline const ProtoObjectCell* appendValueAt(ProtoContext* context,
                                                    const ProtoObjectCell* oc,
                                                    const ProtoShape* newShape,
                                                    proto_ulong oldCount,
                                                    const ProtoObject* value) {
            const proto_ulong newCount = oldCount + 1;
            if (PROTO_LIKELY(newCount <= 4)) {
                const ProtoObject* v0 = (oldCount == 0) ? value : oc->values[0];
                const ProtoObject* v1 = (oldCount == 1) ? value : (oldCount > 1 ? oc->values[1] : nullptr);
                const ProtoObject* v2 = (oldCount == 2) ? value : (oldCount > 2 ? oc->values[2] : nullptr);
                const ProtoObject* v3 = (oldCount == 3) ? value : nullptr;
                return new(context) ProtoObjectCell(context, oc->parent, newShape, v0, v1, v2, v3);
            }
            if (oldCount == 4) {
                // Transition d: 0 -> 1 (inline 4 values -> first 6-value leaf)
                const auto* leaf = new(context) ProtoAttrLeafCell(
                    context, oc->values[0], oc->values[1], oc->values[2], oc->values[3], value, nullptr);
                return new(context) ProtoObjectCell(context, oc->parent, newShape, leaf, nullptr, nullptr, nullptr);
            }
            // Check if oldCount is exactly 4 * 6^d (24, 144, 864, ...):
            // if so, the 4 full subtrees in oc->blocks[0..3] become children[0..3]
            // of the new root branch at blocks[0], and children[4] gets a fresh spine!
            proto_ulong cap = 24;
            unsigned prevDepth = 1;
            while (cap < oldCount) {
                cap *= 6;
                ++prevDepth;
            }
            if (oldCount == cap) {
                const Cell* spine = new(context) ProtoAttrLeafCell(context, 0, value);
                for (unsigned l = 1; l < prevDepth; ++l) {
                    spine = new(context) ProtoAttrBranchCell(context, 0, spine);
                }
                const auto* rootBranch = new(context) ProtoAttrBranchCell(
                    context, oc->blocks[0], oc->blocks[1], oc->blocks[2], oc->blocks[3], spine, nullptr);
                return new(context) ProtoObjectCell(context, oc->parent, newShape, rootBranch, nullptr, nullptr, nullptr);
            }
            return setSlotAt(context, oc, newShape, newCount, oldCount, value);
        }
    }

    ProtoShape::ProtoShape(ProtoContext* context, proto_ulong count, const ProtoShape* next,
                           const ProtoString* n0, const ProtoString* n1,
                           const ProtoString* n2, const ProtoString* n3)
        : Cell(context), count(count), next(next), names{n0, n1, n2, n3} {}

    void ProtoShape::processReferences(ProtoContext* context, void* self,
                                       void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (this->next) {
            method(context, self, this->next);
        }
    }

    const ProtoObject* ProtoShape::implAsObject(ProtoContext*) const {
        return PROTO_NONE;
    }

    ProtoAttrLeafCell::ProtoAttrLeafCell(ProtoContext* context,
                                         const ProtoObject* v0, const ProtoObject* v1,
                                         const ProtoObject* v2, const ProtoObject* v3,
                                         const ProtoObject* v4, const ProtoObject* v5)
        : Cell(context), values{v0, v1, v2, v3, v4, v5} {}

    ProtoAttrLeafCell::ProtoAttrLeafCell(ProtoContext* context, proto_ulong idx, const ProtoObject* val)
        : Cell(context),
          values{idx == 0 ? val : nullptr, idx == 1 ? val : nullptr,
                 idx == 2 ? val : nullptr, idx == 3 ? val : nullptr,
                 idx == 4 ? val : nullptr, idx == 5 ? val : nullptr} {}

    const ProtoAttrLeafCell* ProtoAttrLeafCell::withValueAt(ProtoContext* context,
                                                            proto_ulong idx,
                                                            const ProtoObject* val) const {
        return new(context) ProtoAttrLeafCell(
            context,
            idx == 0 ? val : values[0],
            idx == 1 ? val : values[1],
            idx == 2 ? val : values[2],
            idx == 3 ? val : values[3],
            idx == 4 ? val : values[4],
            idx == 5 ? val : values[5]);
    }

    void ProtoAttrLeafCell::processReferences(ProtoContext* context, void* self,
                                              void (*method)(ProtoContext*, void*, const Cell*)) const {
        for (unsigned i = 0; i < 6; ++i) {
            if (const Cell* c = ProtoObject::asCellPointer(this->values[i])) {
                method(context, self, c);
            }
        }
    }

    const ProtoObject* ProtoAttrLeafCell::implAsObject(ProtoContext*) const {
        return PROTO_NONE;
    }

    ProtoAttrBranchCell::ProtoAttrBranchCell(ProtoContext* context,
                                             const Cell* c0, const Cell* c1, const Cell* c2,
                                             const Cell* c3, const Cell* c4, const Cell* c5)
        : Cell(context), children{c0, c1, c2, c3, c4, c5} {}

    ProtoAttrBranchCell::ProtoAttrBranchCell(ProtoContext* context, proto_ulong idx, const Cell* child)
        : Cell(context),
          children{idx == 0 ? child : nullptr, idx == 1 ? child : nullptr,
                   idx == 2 ? child : nullptr, idx == 3 ? child : nullptr,
                   idx == 4 ? child : nullptr, idx == 5 ? child : nullptr} {}

    const ProtoAttrBranchCell* ProtoAttrBranchCell::withChildAt(ProtoContext* context,
                                                                proto_ulong idx,
                                                                const Cell* child) const {
        return new(context) ProtoAttrBranchCell(
            context,
            idx == 0 ? child : children[0],
            idx == 1 ? child : children[1],
            idx == 2 ? child : children[2],
            idx == 3 ? child : children[3],
            idx == 4 ? child : children[4],
            idx == 5 ? child : children[5]);
    }

    void ProtoAttrBranchCell::processReferences(ProtoContext* context, void* self,
                                                void (*method)(ProtoContext*, void*, const Cell*)) const {
        for (unsigned i = 0; i < 6; ++i) {
            if (this->children[i]) {
                method(context, self, this->children[i]);
            }
        }
    }

    const ProtoObject* ProtoAttrBranchCell::implAsObject(ProtoContext*) const {
        return PROTO_NONE;
    }

    ProtoObjectCell::ProtoObjectCell(ProtoContext* context,
                                     const ParentLinkImplementation* parent,
                                     const ProtoShape* shape,
                                     uintptr_t s0, uintptr_t s1, uintptr_t s2, uintptr_t s3)
        : Cell(context), parent(parent), shape(shape), rawSlots{s0, s1, s2, s3} {}

    ProtoObjectCell::ProtoObjectCell(ProtoContext* context,
                                     const ParentLinkImplementation* parent,
                                     const ProtoShape* shape,
                                     const ProtoObject* v0, const ProtoObject* v1,
                                     const ProtoObject* v2, const ProtoObject* v3)
        : Cell(context), parent(parent), shape(shape), values{v0, v1, v2, v3} {}

    ProtoObjectCell::ProtoObjectCell(ProtoContext* context,
                                     const ParentLinkImplementation* parent,
                                     const ProtoShape* shape,
                                     const Cell* b0, const Cell* b1, const Cell* b2, const Cell* b3)
        : Cell(context), parent(parent), shape(shape), blocks{b0, b1, b2, b3} {}

    ProtoMutableObjectCell::ProtoMutableObjectCell(ProtoContext* context,
                                                   const ParentLinkImplementation* parent,
                                                   proto_ulong ref,
                                                   const ProtoObjectCell* init)
        : ProtoObjectCell(context, parent, nullptr,
                          static_cast<uintptr_t>(ref),
                          reinterpret_cast<uintptr_t>(init),
                          0, 0) {}

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
            this->shape,
            this->rawSlots[0],
            this->rawSlots[1],
            this->rawSlots[2],
            this->rawSlots[3]
        );
    }

    namespace {
        /**
         * Fixed-capacity inline buffer with heap fallback: the common case
         * (a parent list of a few dozen to a couple hundred entries) never
         * touches the heap; an arbitrarily large one grows into it (like
         * `std::vector`, but `std::vector` always heap-allocates once it
         * holds anything, even one element — the whole point here is to
         * avoid that for the overwhelmingly common, small case). `T` must
         * be trivially copyable; only ever instantiated with
         * `const ProtoObject*`.
         */
        template <typename T, size_t InlineCapacity>
        class SmallVector {
        public:
            SmallVector() : data_(inline_), capacity_(InlineCapacity) {}
            ~SmallVector() { if (data_ != inline_) delete[] data_; }
            SmallVector(const SmallVector&) = delete;
            SmallVector& operator=(const SmallVector&) = delete;

            void reserve(size_t n) { if (n > capacity_) growTo(n); }
            void push_back(T v) {
                if (size_ == capacity_) growTo(capacity_ * 2);
                data_[size_++] = v;
            }
            size_t size() const { return size_; }
            T& operator[](size_t i) { return data_[i]; }
            const T& operator[](size_t i) const { return data_[i]; }

        private:
            void growTo(size_t newCapacity) {
                if (newCapacity < capacity_ * 2) newCapacity = capacity_ * 2;
                T* newData = new T[newCapacity];
                for (size_t i = 0; i < size_; ++i) newData[i] = data_[i];
                if (data_ != inline_) delete[] data_;
                data_ = newData;
                capacity_ = newCapacity;
            }

            T inline_[InlineCapacity];
            T* data_;
            size_t capacity_;
            size_t size_ = 0;
        };

        /**
         * Small open-addressing pointer set with the same inline-capacity-
         * then-heap-fallback shape as SmallVector above, used to make
         * setParents's flattening dedup O(1) amortised per lookup instead
         * of an O(current-size) linear scan of the accumulator per
         * candidate — the linear scan made a single setParents call
         * O(total-ancestor-count^2) (3ms for a 4,000-parent list; see the
         * CHANGELOG for the re-measured numbers after this fix). Linear
         * probing, power-of-two sizing, grows (rehashes into a larger
         * `new[]`-allocated table) once the load factor would exceed 1/2.
         */
        class ObjectPointerSet {
        public:
            ObjectPointerSet() : table_(inlineTable_), capacity_(kInlineCapacity) {
                for (size_t i = 0; i < capacity_; ++i) table_[i] = nullptr;
            }
            ~ObjectPointerSet() { if (table_ != inlineTable_) delete[] table_; }
            ObjectPointerSet(const ObjectPointerSet&) = delete;
            ObjectPointerSet& operator=(const ObjectPointerSet&) = delete;

            // Inserts `key`. Returns true iff it was NOT already present
            // (i.e. it was newly inserted).
            bool insert(const ProtoObject* key) {
                if ((count_ + 1) * 2 > capacity_) grow();
                size_t idx = slotFor(key, table_, capacity_);
                if (table_[idx] == key) return false;
                table_[idx] = key;
                ++count_;
                return true;
            }

        private:
            static size_t hashOf(const ProtoObject* key) {
                // ProtoObjectCell pointers are 64-byte aligned; shift away
                // the always-zero low bits to spread the hash.
                return static_cast<size_t>(reinterpret_cast<uintptr_t>(key) >> 6);
            }
            static size_t slotFor(const ProtoObject* key, const ProtoObject* const* table, size_t capacity) {
                size_t idx = hashOf(key) & (capacity - 1);
                while (table[idx] != nullptr && table[idx] != key) {
                    idx = (idx + 1) & (capacity - 1);
                }
                return idx;
            }
            void grow() {
                size_t newCapacity = capacity_ * 2;
                auto* newTable = new const ProtoObject*[newCapacity];
                for (size_t i = 0; i < newCapacity; ++i) newTable[i] = nullptr;
                for (size_t i = 0; i < capacity_; ++i) {
                    if (table_[i] != nullptr) {
                        newTable[slotFor(table_[i], newTable, newCapacity)] = table_[i];
                    }
                }
                if (table_ != inlineTable_) delete[] table_;
                table_ = newTable;
                capacity_ = newCapacity;
            }

            static constexpr size_t kInlineCapacity = 512; // power of two
            const ProtoObject* inlineTable_[kInlineCapacity];
            const ProtoObject** table_;
            size_t capacity_;
            size_t count_ = 0;
        };

        // Inline capacity shared by the flattening accumulator and its
        // dedup set: sized to comfortably cover BOTH step 1 (the listed
        // parents) and step 2 (their ancestors) for realistic hierarchies
        // without touching the heap; SmallVector/ObjectPointerSet grow
        // into the heap automatically for anything larger.
        //
        // Stack cost of flattenParentsOrder's two locals at this
        // capacity: FlatParentList's inline_ is 256 * 8 bytes = 2 KiB;
        // ObjectPointerSet's inlineTable_ is 512 * 8 bytes = 4 KiB (sized
        // 2x for a <=50% load factor) — ~6 KiB total per call. Not
        // recursive, so this is a one-time cost per `setParents` call,
        // not a per-level one; still worth keeping in mind if this
        // constant is ever raised.
        constexpr size_t SET_PARENTS_INLINE_CAPACITY = 256;
        using FlatParentList = SmallVector<const ProtoObject*, SET_PARENTS_INLINE_CAPACITY>;

        /**
         * @brief Computes setParents's flattened, de-duplicated ancestor
         * ORDER — no Cell allocation at all, so this is safe (and, for
         * performance, deliberately called) OUTSIDE any GC critical
         * section and OUTSIDE a mutable receiver's CAS retry loop: it
         * depends only on `newParents` and each listed parent's own
         * already-published chain, never on the receiver's own current
         * state, so recomputing it on every CAS retry bought nothing but
         * cost.
         *
         * Fills `outFlat`, in order: (1) the entries of `newParents`, in
         * the given order, de-duplicated; (2) every ancestor of each of
         * those listed parents — walking each parent's own chain in that
         * parent's own order, in the SAME order the parents were listed —
         * that is not already present. A list that already contains every
         * ancestor of every listed parent (e.g. a full linearization) is
         * unaffected by step 2 and installs exactly as given, in the exact
         * same order: a no-op relative to step 1 alone.
         *
         * `skipIdentity`, when non-null, is OMITTED wherever it would
         * otherwise be added — as a listed parent, or as an ancestor of
         * one — instead of being inserted: a mutable receiver can never
         * become its own ancestor this way. This is a silent no-op for
         * that ONE entry, not an error: it matches `addParent`, which
         * already tolerates `obj->addParent(ctx, obj)` as a no-op (via
         * `hasParent`'s `target == this` short-circuit — see
         * `ProtoObject::addParent`), and it means the exception this used
         * to throw here can no longer reach an embedder through
         * `setParents` at all. See `ProtoObject::setParents`'s doc
         * comment for why the comparison is only meaningful for a mutable
         * receiver, and CHANGELOG.md for why a THROWING version of this
         * check was replaced (it only ever caught a DIRECT reference back
         * to the receiver — one hop through a listed parent's own chain —
         * never a longer cycle built up over several `setParents` calls
         * on different mutable objects; skipping is exactly as complete a
         * guard as throwing was, without an exception embedders must
         * catch). `skipIdentity` MUST be the receiver's STABLE PUBLIC
         * HANDLE (the `this` a caller holds), never a resolved mutable
         * snapshot pointer: a mutable object's snapshot pointer changes on
         * every mutation, so comparing against one would never match a
         * reference back to the SAME logical object (whose identity, from
         * every OTHER object's point of view, is the handle, not whatever
         * snapshot happened to be current when it was last read).
         */
        void flattenParentsOrder(ProtoContext* context, const ProtoList* newParents,
                                  const ProtoObject* skipIdentity, FlatParentList& outFlat)
        {
            if (!newParents) return;
            proto_ulong n = newParents->getSize(context);
            outFlat.reserve(n);
            ObjectPointerSet seen;

            // Step 1: listed parents, given order, de-duplicated.
            for (proto_ulong i = 0; i < n; ++i) {
                const ProtoObject* p = newParents->getAt(context, static_cast<int>(i));
                if (!p) continue;
                if (skipIdentity && p == skipIdentity) continue; // no-op: never its own parent
                if (seen.insert(p)) outFlat.push_back(p);
            }

            // Step 2: each listed parent's own ancestors, in the parent's
            // own chain order, appended once each, in listed-parent order.
            // Index over the step-1 count specifically (not outFlat.size(),
            // which grows during this loop) so parents are processed in
            // their original listed order even as their ancestors are
            // appended after them.
            //
            // Non-object policy (consistent with step 1, which accepts
            // any entry regardless of tag — addParent/setParents both
            // accept a non-object "parent", e.g. a heap ProtoString, and
            // getAttribute/hasAttribute/getAttributes already answer for
            // one through its own prototype): a listed parent that is not
            // a real object cell simply has no OWN chain to walk here, so
            // it contributes nothing beyond its own step-1 entry — it is
            // NOT removed from `outFlat`, only skipped as a source of
            // further ancestors, exactly as it has no attributes chain of
            // its own to search either.
            const size_t listedCount = outFlat.size();
            for (size_t idx = 0; idx < listedCount; ++idx) {
                const ProtoObject* p = outFlat[idx];
                if (!proto::isObjectFast(p)) continue; // no chain to walk

                const ProtoObjectCell* pOc = resolveOwnCell(context, p);
                const ParentLinkImplementation* link = pOc->parent;
                while (link && ((uintptr_t)link & 0x3F) == 0) {
                    auto pl = toImpl<const ParentLinkImplementation>(link);
                    if (pl->getType() != CellType::ParentLink) break;

                    const ProtoObject* anc = pl->getObject(context);
                    if (anc && !(skipIdentity && anc == skipIdentity) && seen.insert(anc)) {
                        outFlat.push_back(anc);
                    }

                    link = pl->getParent(context);
                }
            }
        }

        /**
         * @brief Builds the ParentLinkImplementation chain from an
         * already-flattened, ordered list (flattenParentsOrder above).
         * Allocates one Cell per entry — the caller MUST hold a GC
         * critical section for the duration: the intermediate links are
         * not reachable from any root until the final head this returns
         * is itself linked into a ProtoObjectCell.
         *
         * Called ONCE per `setParents` call (not once per CAS retry on a
         * mutable receiver, unlike the flattening step's old, folded-
         * together implementation): the chain is the same regardless of
         * how many times the CAS is retried, so the retry loop just
         * re-wraps this SAME precomputed chain in a fresh ProtoObjectCell
         * each attempt (see ProtoObject::setParents).
         */
        const ParentLinkImplementation* buildParentChainFromFlat(ProtoContext* context, const FlatParentList& flat)
        {
            // Tail-first: flat's LAST entry becomes the chain tail, flat's
            // FIRST entry the chain head, matching getParents()'s
            // head-first emit order.
            const ParentLinkImplementation* newChain = nullptr;
            for (size_t i = flat.size(); i > 0; --i) {
                newChain = new(context) ParentLinkImplementation(context, newChain, flat[i - 1]);
            }
            return newChain;
        }
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
     *
     * For the handle of a mutable object (mutable_ref > 0) it records the
     * mutable_ref, so that the collector releases the object's entry in
     * mutableRoot after sweep (releaseFinalizedMutableEntries in
     * ProtoSpace.cpp).  State cells are always built with mutable_ref 0 and
     * every handle gets its own ref, so only the handle records it.
     *
     * Following the finalizer contract (Cell::finalize) it neither allocates
     * nor touches mutableRoot.  The record is an append to
     * ProtoSpace::gcFinalizedMutableRefs, GC-thread bookkeeping of the same
     * kind as the mark work list: a plain std::vector of integers, used only
     * by the GC thread, which is the only thread that runs finalizers.
     */
    void ProtoMutableObjectCell::finalize(ProtoContext* context) const
    {
        if (this->mutable_ref > 0 && context && context->space) {
            if (std::vector<proto_ulong>* sink = sweep::finalizedRefSink())
                sink->push_back(this->mutable_ref);
            else
                context->space->gcFinalizedMutableRefs.push_back(this->mutable_ref);
        }
    }

    void ProtoMutableObjectCell::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const
    {
        if (this->parent) {
            method(context, self, this->parent);
        }
        if (this->initialState) {
            method(context, self, this->initialState);
        }
    }

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
        if (this->parent)
        {
            method(context, self, this->parent);
        }

        if (this->shape)
        {
            method(context, self, this->shape);
            const proto_ulong count = this->shape->count;
            if (count <= 4) {
                for (proto_ulong i = 0; i < count; ++i) {
                    if (const Cell* c = ProtoObject::asCellPointer(this->values[i])) {
                        method(context, self, c);
                    }
                }
            } else {
                for (unsigned i = 0; i < 4; ++i) {
                    if (this->blocks[i]) {
                        method(context, self, this->blocks[i]);
                    }
                }
            }
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
            // The objectPrototype is the root of the chain: returning it
            // here would loop back into itself.  All other parentless
            // cells fall through to objectPrototype as the conventional
            // root.
            if (this == context->space->objectPrototype) return nullptr;
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
        case POINTER_TAG_MAP: return context->space->mapPrototype;
        case POINTER_TAG_MPSC_QUEUE: return context->space->mpscQueuePrototype;
        case POINTER_TAG_LINKED_LIST: return context->space->getLinkedListPrototype();
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

    const ProtoObject* ProtoObject::newClone(ProtoContext* context, bool isMutable) const
    {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return PROTO_NONE;

        ProtoContext::CriticalSection cs(context);
        const ProtoObjectCell* oc = resolveOwnCell(context, this);

        if (isMutable) {
            proto_ulong ref = generate_mutable_ref(context);
            const ProtoObjectCell* init = nullptr;
            if (oc->shape != nullptr) {
                init = new(context) ProtoObjectCell(
                    context, oc->parent, oc->shape,
                    oc->rawSlots[0], oc->rawSlots[1], oc->rawSlots[2], oc->rawSlots[3]);
            }
            return (new(context) ProtoMutableObjectCell(context, oc->parent, ref, init))->asObject(context);
        }

        return (new(context) ProtoObjectCell(
            context, oc->parent, oc->shape,
            oc->rawSlots[0], oc->rawSlots[1], oc->rawSlots[2], oc->rawSlots[3]))->asObject(context);
    }

    const ProtoObject* ProtoObject::clone(ProtoContext* context, bool isMutable) const
    {
        return newClone(context, isMutable);
    }

    const ProtoObject* ProtoObject::newChild(ProtoContext* context, bool isMutable) const
    {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) {
             const ProtoObject* prototype = getPrototype(context);
             return prototype ? prototype->newChild(context, isMutable) : PROTO_NONE;
        }
        proto_ulong ref = isMutable ? generate_mutable_ref(context) : 0;

        ProtoContext::CriticalSection cs(context);
        const ProtoObjectCell* oc = resolveOwnCell(context, this);
        const auto* link = new(context) ParentLinkImplementation(context, oc->parent, this);
        if (isMutable) {
            return (new(context) ProtoMutableObjectCell(context, link, ref, nullptr))->asObject(context);
        }
        return (new(context) ProtoObjectCell(context, link, nullptr))->asObject(context);
    }

    // newChild(false) with a ProtoLazyObjectCell in place of the object cell
    // (core/ProtoLazyObject.cpp holds the slot's operations).
    const ProtoObject* ProtoObject::newLazyChild(ProtoContext* context, const ProtoObject* producer) const
    {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) {
            const ProtoObject* prototype = getPrototype(context);
            return prototype ? prototype->newLazyChild(context, producer) : PROTO_NONE;
        }
        ProtoContext::CriticalSection cs(context);
        const ProtoObjectCell* oc = resolveOwnCell(context, this);
        auto* newObject = new(context) ProtoLazyObjectCell(
            context, new(context) ParentLinkImplementation(context, oc->parent, this),
            producer);
        return newObject->asObject(context);
    }

    namespace {
        // Every call form ends here: one lookup, one method, the arguments
        // as descriptors (either may be null).
        const ProtoObject* dispatchCall(const ProtoObject* receiver, ProtoContext* context,
                                        const ParentLink* nextParent, const ProtoString* method,
                                        const ProtoObject* self, const ProtoArguments* positional,
                                        const ProtoKeywordArguments* keywords) {
            const auto* result = receiver->getAttribute(context, const_cast<ProtoString*>(method));
            if (result && result->isMethod(context)) {
                return toImpl<const ProtoMethodCell>(result)->method(context, self, nextParent, positional, keywords);
            }
            if (context->space->nonMethodCallback) {
                return (*context->space->nonMethodCallback)(context, nextParent, method, self, positional, keywords);
            }
            return PROTO_NONE;
        }
    }

    const ProtoObject* ProtoObject::call(ProtoContext* context, const ParentLink* nextParent, const ProtoString* method, const ProtoObject* self, const ProtoList* positionalParameters, const ProtoSparseList* keywordParametersDict) const
    {
        // The lists' values are read in place (a small list) or copied into
        // local arrays of this frame; only a call with more arguments than
        // the arrays hold goes through the heap-backed bridges.  The lists
        // keep the values reachable.
        constexpr unsigned kLocalPositional = 16;
        constexpr unsigned kLocalKeywords = 8;
        const ProtoObject* localValues[kLocalPositional];
        ProtoKeywordArgument localPairs[kLocalKeywords];

        const unsigned n = positionalParameters ? static_cast<unsigned>(positionalParameters->getSize(context)) : 0;
        const unsigned kn = keywordParametersDict ? static_cast<unsigned>(keywordParametersDict->getSize(context)) : 0;
        if (n > kLocalPositional || kn > kLocalKeywords) {
            const ProtoListArguments positional(context, positionalParameters);
            const ProtoSparseListKeywordArguments keywords(context, keywordParametersDict);
            return dispatchCall(this, context, nextParent, method, self, &positional,
                                keywordParametersDict ? &keywords : nullptr);
        }

        const ProtoObject* const* values = localValues;
        ProtoObjectPointer pa{};
        pa.oid = reinterpret_cast<const ProtoObject*>(positionalParameters);
        if (n && pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            values = toImpl<const ProtoListSmallImplementation>(positionalParameters)->slots;
        } else {
            for (unsigned i = 0; i < n; ++i) localValues[i] = positionalParameters->getAt(context, static_cast<int>(i));
        }
        const ProtoArguments positional(values, n, positionalParameters);
        if (!keywordParametersDict) {
            return dispatchCall(this, context, nextParent, method, self, &positional, nullptr);
        }
        struct Fill {
            ProtoKeywordArgument* pairs;
            unsigned at;
            static void add(void* self, proto_ulong key, const ProtoObject* value) {
                auto* f = static_cast<Fill*>(self);
                f->pairs[f->at++] = {reinterpret_cast<const ProtoString*>(key), value};
            }
        } fill{localPairs, 0};
        if (kn) walkSparseListNoPark(keywordParametersDict, &fill, &Fill::add);
        const ProtoKeywordArguments keywords(localPairs, fill.at, keywordParametersDict);
        return dispatchCall(this, context, nextParent, method, self, &positional, &keywords);
    }

    const ProtoObject* ProtoObject::call(ProtoContext* context, const ParentLink* nextParent, const ProtoString* method,
                                         const ProtoObject* self, const ProtoObject* const* positional,
                                         unsigned positionalCount, const ProtoKeywordArgument* keywords,
                                         unsigned keywordCount) const
    {
        const ProtoArguments positionalArgs(positional, positionalCount);
        const ProtoKeywordArguments keywordArgs(keywords, keywordCount);
        return dispatchCall(this, context, nextParent, method, self, &positionalArgs,
                            keywordCount ? &keywordArgs : nullptr);
    }

    const ProtoObject* ProtoObject::call(ProtoContext* context, const ParentLink* nextParent, const ProtoString* method,
                                         const ProtoObject* self, const ProtoArguments& positional,
                                         const ProtoKeywordArguments* keywords) const
    {
        return dispatchCall(this, context, nextParent, method, self, &positional, keywords);
    }

    namespace {
        /**
         * @brief Is `ancestor` a direct entry of the chain starting at `link`?
         *
         * A pure linear scan, no allocation, no recursion, no length limit —
         * exactly the chain-navigation loop `ProtoObject::getAttribute`
         * uses. `ProtoObject::newChild`, `ProtoObjectCell::addParent` (via
         * the public `ProtoObject::addParent` trampoline) AND the
         * `flattenParentsOrder`/`buildParentChainFromFlat` pair `setParents`
         * uses all guarantee that an object's own chain already contains
         * every one of its ancestors as a direct entry, so this single-level
         * scan is complete and exact for any object — there is no
         * construction path that leaves an ancestor reachable only through
         * a visited parent's own separate chain. (Function names, not line
         * numbers: this file changes shape often enough that line citations
         * go stale quickly — grep for the name instead.)
         */
        bool chainHasAncestor(ProtoContext* context, const ParentLinkImplementation* link,
                               const ProtoObject* ancestor)
        {
            while (link && ((uintptr_t)link & 0x3F) == 0) {
                auto pl = toImpl<const ParentLinkImplementation>(link);
                if (pl->getType() != CellType::ParentLink) break;
                if (pl->getObject(context) == ancestor) return true;
                link = pl->getParent(context);
            }
            return false;
        }
    }

    const ProtoObject* ProtoObject::isInstanceOf(ProtoContext* context, const ProtoObject* prototype) const
    {
        if (!this || !prototype || !context) return PROTO_NONE;

        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            // Object receiver: search this object's OWN chain directly,
            // mutable-resolved — rather than through getPrototype(), which
            // does not resolve a mutable receiver's current snapshot.
            const ProtoObjectCell* oc = resolveOwnCell(context, this);
            if (chainHasAncestor(context, oc->parent, prototype)) return PROTO_TRUE;

            // Universal-root fallback: an object with NO parent chain of
            // its own conventionally descends from objectPrototype — the
            // same fallback getPrototype() applies for a parentless object
            // cell (core/ProtoObject.cpp getPrototype, POINTER_TAG_OBJECT
            // case). This only applies here, at the top level, matching
            // getPrototype()'s own scope: an object WITH an explicit chain
            // is not implicitly rooted at objectPrototype unless its own
            // construction put it there (e.g. via addParent).
            if (!hasValidParentLink(oc) && this != context->space->objectPrototype
                    && prototype == context->space->objectPrototype) {
                return PROTO_TRUE;
            }
            return PROTO_NONE;
        }

        // Non-object receiver (SmallInteger, string, list, ...): answered
        // through its prototype, same as today.
        const ProtoObject* start = this->getPrototype(context);
        if (!start) return PROTO_NONE;
        if (start == prototype) return PROTO_TRUE;
        if (!proto::isObjectFast(start)) return PROTO_NONE;
        const ProtoObjectCell* startOc = resolveOwnCell(context, start);
        return chainHasAncestor(context, startOc->parent, prototype) ? PROTO_TRUE : PROTO_NONE;
    }

    const ProtoObject* ProtoObject::getAttribute(ProtoContext* context, const ProtoString* name, bool callbacks) const
    {
        if (!this || !name || !context) return nullptr;

        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                nonInterned::note(nonInterned::Get, PROTO_RETURN_ADDRESS());
                static const bool s_trust =
                    std::getenv("PROTOCORE_TRUST_SYMBOLS") != nullptr;
                if (s_trust) return PROTO_NONE;
                const ProtoObject* sym = context->space->symbolTable->lookupByContent(
                    context, reinterpret_cast<const ProtoObject*>(name));
                if (!sym) return PROTO_NONE;
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

        AttributeCacheEntry* cache = getShapeCache(context);

        const ProtoObject* currentPointer = this;
        const ParentLinkImplementation* currentLink = nullptr;
#ifdef PROTO_CACHE_STATS
        proto_ulong pcsSteps = 0, pcsAvlSteps = 0, pcsMutableSteps = 0;
#endif

        while (currentPointer) {
            const bool isObj = (reinterpret_cast<uintptr_t>(currentPointer) & PROTO_UL(0x3F)) == POINTER_TAG_OBJECT;
            if (!isObj) {
                const ProtoObject* nextProto = currentPointer->getPrototype(context);
                if (nextProto == currentPointer) break;
                currentPointer = nextProto;
                continue;
            }

            auto oc = toImpl<const ProtoObjectCell>(currentPointer);
            const ProtoObjectCell* ocValue = oc;
#ifdef PROTO_CACHE_STATS
            ++pcsSteps;
#endif
            if (oc->isMutableCell()) {
#ifdef PROTO_CACHE_STATS
                ++pcsMutableSteps;
#endif
                const auto* moc = static_cast<const ProtoMutableObjectCell*>(oc);
                const proto::ProtoObject* storedState =
                    resolveMutableSnapshot(context, moc->mutable_ref);
                if (storedState != nullptr) {
                    ocValue = toImpl<const ProtoObjectCell>(storedState);
                } else if (moc->initialState != nullptr) {
                    ocValue = moc->initialState;
                }
            }

            const ProtoShape* shape = ocValue->shape;
            if (shape != nullptr) {
                proto_ulong shapeCount = 0;
                const proto_long offset = findAttributeOffset(cache, shape, name, shapeCount);
                if (offset >= 0) {
                    const ProtoObject* result = getValueAt(
                        ocValue, shapeCount, static_cast<proto_ulong>(offset));
                    if (result != nullptr) {
#ifdef PROTO_CACHE_STATS
                        protoCacheStatsRecordCall(pcsSteps, pcsAvlSteps, pcsMutableSteps, true);
#endif
                        return result;
                    }
                }
            }

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
#ifdef PROTO_CACHE_STATS
        protoCacheStatsRecordCall(pcsSteps, pcsAvlSteps, pcsMutableSteps, false);
#endif
        return PROTO_NONE;
    }

    namespace {
        inline const ProtoObjectCell* applySingleAttributeWrite(
            ProtoContext* context,
            AttributeCacheEntry* cache,
            const ProtoObjectCell* currentOc,
            const ProtoString* name,
            const ProtoObject* value)
        {
            const ProtoShape* shape = currentOc->shape;
            const proto_ulong oldCount = shape ? shape->count : 0;
            const proto_long offset = findAttributeOffset(cache, shape, name);

            if (offset >= 0) {
                const proto_ulong uoff = static_cast<proto_ulong>(offset);
                if (getValueAt(currentOc, oldCount, uoff) == value) {
                    return currentOc;
                }
                return setSlotAt(context, currentOc, shape, oldCount, uoff, value);
            }

            // Attribute not present in shape: if value is nullptr (removal of
            // non-existent attribute), nothing changes.
            if (value == nullptr) {
                return currentOc;
            }

            const ProtoShape* newShape = extendShape(context, shape, name);
            if (cache) {
                const proto_ulong idx = shapeCacheSlot(newShape, name);
                cache[idx] = {newShape, name, static_cast<proto_long>(oldCount), newShape->count};
            }
            return appendValueAt(context, currentOc, newShape, oldCount, value);
        }

        const ProtoShape* extendShapeBulk(ProtoContext* context,
                                          const ProtoShape* oldShape,
                                          const ProtoString* const* addedNames,
                                          unsigned addedCount)
        {
            if (addedCount == 0) return oldShape;
            const ProtoShape* curShape = oldShape;
            proto_ulong curCount = oldShape ? oldShape->count : 0;
            unsigned idx = 0;

            if (curShape != nullptr) {
                const unsigned used = static_cast<unsigned>(((curCount - 1) & 3UL) + 1);
                if (used < 4) {
                    const unsigned take = std::min<unsigned>(4 - used, addedCount);
                    const ProtoString* n[4] = {
                        curShape->names[0], curShape->names[1],
                        curShape->names[2], curShape->names[3]
                    };
                    for (unsigned j = 0; j < take; ++j) {
                        n[3 - (used + j)] = addedNames[idx + j];
                    }
                    curCount += take;
                    idx += take;
                    curShape = new(context) ProtoShape(
                        context, curCount, curShape->next, n[0], n[1], n[2], n[3]);
                }
            }

            while (idx < addedCount) {
                const unsigned take = std::min<unsigned>(4, addedCount - idx);
                const ProtoString* n[4] = {nullptr, nullptr, nullptr, nullptr};
                for (unsigned j = 0; j < take; ++j) {
                    n[3 - j] = addedNames[idx + j];
                }
                curCount += take;
                idx += take;
                curShape = new(context) ProtoShape(
                    context, curCount, curShape, n[0], n[1], n[2], n[3]);
            }
            return curShape;
        }
    }

    const ProtoObject* ProtoObject::setAttribute(ProtoContext* context, const ProtoString* name, const ProtoObject* value) const {
        if (!this || !name) return this;

        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                nonInterned::note(nonInterned::Set, PROTO_RETURN_ADDRESS());
                const ProtoObject* sym = context->space->symbolTable->intern(
                    context, reinterpret_cast<const ProtoObject*>(name));
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

        if (!proto::isObjectFast(this)) return this;
        auto* oc = toImpl<ProtoObjectCell>(this);
        AttributeCacheEntry* cache = getShapeCache(context);

        if (oc->isMutableCell()) {
            const auto* moc = static_cast<const ProtoMutableObjectCell*>(oc);
            const proto_ulong ref = moc->mutable_ref;
            int shard = ref % context->space->MUTABLE_ROOT_SHARDS;
            ProtoContext::CriticalSection cs(context);
            int casIteration = 0;
            while (true) {
                ++casIteration;

                ProtoSparseList* oldRoot = nullptr;
                const ProtoObject* storedState = nullptr;
                resolveMutableState(context, ref, &oldRoot, &storedState);

                const ProtoObjectCell* currentOc = nullptr;
                if (storedState != nullptr) {
                    if (!proto::isObjectFast(storedState)) return this;
                    currentOc = toImpl<const ProtoObjectCell>(storedState);
                } else if (moc->initialState != nullptr) {
                    currentOc = moc->initialState;
                } else {
                    currentOc = moc; // parent = moc->parent, shape = nullptr
                }

                const ProtoObjectCell* nextOc =
                    applySingleAttributeWrite(context, cache, currentOc, name, value);
                if (nextOc == currentOc) {
                    // If the write was a no-op (e.g. removing an absent key),
                    // still publish an empty state if nothing was published yet
                    // and the caller expects the mutable object initialized, or
                    // return immediately if unchanged.
                    if (storedState != nullptr || value == nullptr) return this;
                    nextOc = new(context) ProtoObjectCell(
                        context, currentOc->parent, currentOc->shape,
                        currentOc->rawSlots[0], currentOc->rawSlots[1],
                        currentOc->rawSlots[2], currentOc->rawSlots[3]);
                }

                const ProtoObject* newState = nextOc->asObject(context);
                const ProtoSparseList* oldRootSL =
                    (oldRoot == nullptr) ? context->newSparseList() : oldRoot;
                ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(
                    oldRootSL->setAt(context, ref, newState));
                ProtoSparseList* expected = oldRoot;
                if (globalMutableShards[shard].root.compare_exchange_weak(expected, newRoot)) {
                    refreshMutableCache(context, ref, newRoot, newState);
                    break;
                }
                if ((casIteration & 31) == 0) {
                    std::this_thread::yield();
                }
            }
            return this;
        }

        // Immutable object (or ProtoLazyObjectCell, which produces a plain ProtoObjectCell).
        ProtoContext::CriticalSection cs(context);
        const ProtoObjectCell* nextOc = applySingleAttributeWrite(context, cache, oc, name, value);
        if (nextOc == oc) {
            return (new(context) ProtoObjectCell(
                context, oc->parent, oc->shape,
                oc->rawSlots[0], oc->rawSlots[1], oc->rawSlots[2], oc->rawSlots[3]))->asObject(context);
        }
        return nextOc->asObject(context);
    }

    const ProtoObject* ProtoObject::setAttributes(ProtoContext* context, unsigned count,
                                                  const ProtoString* const* names,
                                                  const ProtoObject* const* values) const {
        if (!this || count == 0 || !names || !values) return this;
        if (!proto::isObjectFast(this)) return this;

        struct Write { const ProtoString* name; const ProtoObject* value; };
        constexpr unsigned kInline = 16;
        Write inlineWrites[kInline];
        std::vector<Write> heapWrites;
        Write* writes = inlineWrites;
        if (count > kInline) {
            heapWrites.resize(count);
            writes = heapWrites.data();
        }

        unsigned n = 0;
        for (unsigned i = 0; i < count; ++i) {
            const ProtoString* name = names[i];
            if (!name) continue;
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                nonInterned::note(nonInterned::SetBulk, PROTO_RETURN_ADDRESS());
                name = reinterpret_cast<const ProtoString*>(context->space->symbolTable->intern(
                    context, reinterpret_cast<const ProtoObject*>(name)));
            }
            // Deduplicate in-place: if `name` already appeared earlier, update
            // its value (later write wins) while keeping its first-seen order.
            bool found = false;
            for (unsigned j = 0; j < n; ++j) {
                if (writes[j].name == name) {
                    writes[j].value = values[i];
                    found = true;
                    break;
                }
            }
            if (!found) {
                writes[n++] = {name, values[i]};
            }
        }
        if (n == 0) return this;

        AttributeCacheEntry* cache = getShapeCache(context);

        auto applyGroup = [&](const ProtoObjectCell* currentOc) -> const ProtoObjectCell* {
            const ProtoShape* oldShape = currentOc->shape;
            const proto_ulong oldCount = oldShape ? oldShape->count : 0;

            struct OffsetWrite { proto_ulong offset; const ProtoObject* value; };
            OffsetWrite inlineUpdates[kInline];
            std::vector<OffsetWrite> heapUpdates;
            OffsetWrite* updates = (n > kInline) ? (heapUpdates.resize(n), heapUpdates.data()) : inlineUpdates;
            unsigned updateCount = 0;

            const ProtoString* inlineAddedNames[kInline];
            const ProtoObject* inlineAddedVals[kInline];
            std::vector<const ProtoString*> heapAddedNames;
            std::vector<const ProtoObject*> heapAddedVals;
            const ProtoString** addedNames = (n > kInline) ? (heapAddedNames.resize(n), heapAddedNames.data()) : inlineAddedNames;
            const ProtoObject** addedVals = (n > kInline) ? (heapAddedVals.resize(n), heapAddedVals.data()) : inlineAddedVals;
            unsigned addedCount = 0;

            for (unsigned i = 0; i < n; ++i) {
                const proto_long off = findAttributeOffset(cache, oldShape, writes[i].name);
                if (off >= 0) {
                    const proto_ulong uoff = static_cast<proto_ulong>(off);
                    if (getValueAt(currentOc, oldCount, uoff) != writes[i].value) {
                        updates[updateCount++] = {uoff, writes[i].value};
                    }
                } else if (writes[i].value != nullptr) {
                    addedNames[addedCount] = writes[i].name;
                    addedVals[addedCount] = writes[i].value;
                    ++addedCount;
                }
            }

            if (updateCount == 0 && addedCount == 0) {
                return currentOc;
            }

            const ProtoShape* newShape = extendShapeBulk(context, oldShape, addedNames, addedCount);
            const proto_ulong newCount = oldCount + addedCount;

            if (newCount <= 4) {
                const ProtoObject* v[4] = {
                    oldCount > 0 ? currentOc->values[0] : nullptr,
                    oldCount > 1 ? currentOc->values[1] : nullptr,
                    oldCount > 2 ? currentOc->values[2] : nullptr,
                    oldCount > 3 ? currentOc->values[3] : nullptr
                };
                for (unsigned i = 0; i < updateCount; ++i) {
                    v[updates[i].offset] = updates[i].value;
                }
                for (unsigned j = 0; j < addedCount; ++j) {
                    v[oldCount + j] = addedVals[j];
                }
                return new(context) ProtoObjectCell(
                    context, currentOc->parent, newShape, v[0], v[1], v[2], v[3]);
            }

            if (newCount <= 24) {
                const ProtoAttrLeafCell* leaves[4] = {nullptr, nullptr, nullptr, nullptr};
                const ProtoObject* leafVals[4][6] = {};
                bool leafDirty[4] = {false, false, false, false};

                if (oldCount <= 4) {
                    for (proto_ulong i = 0; i < oldCount; ++i) {
                        leafVals[0][i] = currentOc->values[i];
                    }
                    if (oldCount > 0) leafDirty[0] = true;
                } else {
                    for (unsigned b = 0; b < 4; ++b) {
                        leaves[b] = static_cast<const ProtoAttrLeafCell*>(currentOc->blocks[b]);
                        if (leaves[b]) {
                            for (unsigned k = 0; k < 6; ++k) leafVals[b][k] = leaves[b]->values[k];
                        }
                    }
                }
                for (unsigned i = 0; i < updateCount; ++i) {
                    const proto_ulong off = updates[i].offset;
                    leafVals[off / 6][off % 6] = updates[i].value;
                    leafDirty[off / 6] = true;
                }
                for (unsigned j = 0; j < addedCount; ++j) {
                    const proto_ulong off = oldCount + j;
                    leafVals[off / 6][off % 6] = addedVals[j];
                    leafDirty[off / 6] = true;
                }
                for (unsigned b = 0; b < 4; ++b) {
                    if (leafDirty[b]) {
                        leaves[b] = new(context) ProtoAttrLeafCell(
                            context,
                            leafVals[b][0], leafVals[b][1], leafVals[b][2],
                            leafVals[b][3], leafVals[b][4], leafVals[b][5]);
                    }
                }
                return new(context) ProtoObjectCell(
                    context, currentOc->parent, newShape,
                    leaves[0], leaves[1], leaves[2], leaves[3]);
            }

            // General case for newCount > 24: apply appends to reach newCount's
            // tree geometry, then apply updates.
            const ProtoObjectCell* cur = currentOc;
            for (unsigned j = 0; j < addedCount; ++j) {
                cur = appendValueAt(context, cur, newShape, oldCount + j, addedVals[j]);
            }
            for (unsigned i = 0; i < updateCount; ++i) {
                cur = setSlotAt(context, cur, newShape, newCount, updates[i].offset, updates[i].value);
            }
            return cur;
        };

        auto* oc = toImpl<const ProtoObjectCell>(this);
        ProtoContext::CriticalSection cs(context);

        if (!oc->isMutableCell()) {
            const ProtoObjectCell* nextOc = applyGroup(oc);
            if (nextOc == oc) {
                return (new(context) ProtoObjectCell(
                    context, oc->parent, oc->shape,
                    oc->rawSlots[0], oc->rawSlots[1], oc->rawSlots[2], oc->rawSlots[3]))->asObject(context);
            }
            return nextOc->asObject(context);
        }

        const auto* moc = static_cast<const ProtoMutableObjectCell*>(oc);
        const proto_ulong ref = moc->mutable_ref;
        const int shard = ref % context->space->MUTABLE_ROOT_SHARDS;
        int casIteration = 0;
        while (true) {
            ++casIteration;
            ProtoSparseList* oldRoot = nullptr;
            const ProtoObject* storedState = nullptr;
            resolveMutableState(context, ref, &oldRoot, &storedState);

            const ProtoObjectCell* currentOc = nullptr;
            if (storedState != nullptr) {
                if (!proto::isObjectFast(storedState)) return this;
                currentOc = toImpl<const ProtoObjectCell>(storedState);
            } else if (moc->initialState != nullptr) {
                currentOc = moc->initialState;
            } else {
                currentOc = moc;
            }

            const ProtoObjectCell* nextOc = applyGroup(currentOc);
            if (nextOc == currentOc) {
                return this;
            }
            const ProtoObject* newState = nextOc->asObject(context);

            const ProtoSparseList* oldRootSL =
                (oldRoot == nullptr) ? context->newSparseList() : oldRoot;
            ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(
                oldRootSL->setAt(context, ref, newState));
            ProtoSparseList* expected = oldRoot;
            if (globalMutableShards[shard].root.compare_exchange_weak(expected, newRoot)) {
                refreshMutableCache(context, ref, newRoot, newState);
                return this;
            }
            if ((casIteration & 31) == 0) {
                std::this_thread::yield();
            }
        }
    }

    bool ProtoObject::setAttributeIfEqual(ProtoContext* context, const ProtoString* name,
                                          const ProtoObject* expected,
                                          const ProtoObject* newValue) const {
        if (!this || !name) return false;

        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                nonInterned::note(nonInterned::SetIfEqual, PROTO_RETURN_ADDRESS());
                const ProtoObject* sym = context->space->symbolTable->intern(
                    context, reinterpret_cast<const ProtoObject*>(name));
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

        if (!proto::isObjectFast(this)) return false;
        auto* oc = toImpl<ProtoObjectCell>(this);
        if (!oc->isMutableCell()) return false;

        const auto* moc = static_cast<const ProtoMutableObjectCell*>(oc);
        const proto_ulong ref = moc->mutable_ref;
        const int shard = ref % context->space->MUTABLE_ROOT_SHARDS;
        AttributeCacheEntry* cache = getShapeCache(context);

        ProtoContext::CriticalSection cs(context);
        int casIteration = 0;
        while (true) {
            ++casIteration;

            ProtoSparseList* oldRoot = nullptr;
            const ProtoObject* storedState = nullptr;
            resolveMutableState(context, ref, &oldRoot, &storedState);

            const ProtoObjectCell* currentOc = nullptr;
            if (storedState != nullptr) {
                if (!proto::isObjectFast(storedState)) return false;
                currentOc = toImpl<const ProtoObjectCell>(storedState);
            } else if (moc->initialState != nullptr) {
                currentOc = moc->initialState;
            } else {
                currentOc = moc;
            }

            const ProtoObject* currentValue = nullptr;
            if (currentOc->shape != nullptr) {
                const proto_long off = findAttributeOffset(cache, currentOc->shape, name);
                if (off >= 0) {
                    currentValue = getValueAt(
                        currentOc, currentOc->shape->count, static_cast<proto_ulong>(off));
                }
            }

            if (currentValue != expected) {
                return false;
            }

            const ProtoObjectCell* nextOc =
                applySingleAttributeWrite(context, cache, currentOc, name, newValue);
            if (nextOc == currentOc && storedState == nullptr) {
                nextOc = new(context) ProtoObjectCell(
                    context, currentOc->parent, currentOc->shape,
                    currentOc->rawSlots[0], currentOc->rawSlots[1],
                    currentOc->rawSlots[2], currentOc->rawSlots[3]);
            }
            const ProtoObject* newState = nextOc->asObject(context);

            const ProtoSparseList* oldRootSL =
                (oldRoot == nullptr) ? context->newSparseList() : oldRoot;
            ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(
                oldRootSL->setAt(context, ref, newState));
            ProtoSparseList* expectedRoot = oldRoot;
            if (globalMutableShards[shard].root.compare_exchange_weak(expectedRoot, newRoot)) {
                refreshMutableCache(context, ref, newRoot, newState);
                return true;
            }
            if ((casIteration & 31) == 0) {
                std::this_thread::yield();
            }
        }
    }

    const ProtoObject* ProtoObject::removeAttribute(ProtoContext* context, const ProtoString* name) const {
        if (!this || !name) return this;

        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                nonInterned::note(nonInterned::Remove, PROTO_RETURN_ADDRESS());
                const ProtoObject* sym = context->space->symbolTable->intern(
                    context, reinterpret_cast<const ProtoObject*>(name));
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

        if (!proto::isObjectFast(this)) return this;
        auto* oc = toImpl<ProtoObjectCell>(this);
        AttributeCacheEntry* cache = getShapeCache(context);

        if (oc->isMutableCell()) {
            const auto* moc = static_cast<const ProtoMutableObjectCell*>(oc);
            const proto_ulong ref = moc->mutable_ref;
            int shard = ref % context->space->MUTABLE_ROOT_SHARDS;
            ProtoContext::CriticalSection cs(context);
            int casIteration = 0;
            while (true) {
                ++casIteration;

                ProtoSparseList* oldRoot = nullptr;
                const ProtoObject* storedState = nullptr;
                resolveMutableState(context, ref, &oldRoot, &storedState);

                const ProtoObjectCell* currentOc = nullptr;
                if (storedState != nullptr) {
                    if (!proto::isObjectFast(storedState)) return this;
                    currentOc = toImpl<const ProtoObjectCell>(storedState);
                } else if (moc->initialState != nullptr) {
                    currentOc = moc->initialState;
                } else {
                    currentOc = moc;
                }

                if (!currentOc->shape) return this;
                const proto_long off = findAttributeOffset(cache, currentOc->shape, name);
                if (off < 0) return this;
                const proto_ulong uoff = static_cast<proto_ulong>(off);
                if (getValueAt(currentOc, currentOc->shape->count, uoff) == nullptr) {
                    return this;
                }

                const ProtoObjectCell* nextOc = setSlotAt(
                    context, currentOc, currentOc->shape, currentOc->shape->count, uoff, nullptr);
                const ProtoObject* newState = nextOc->asObject(context);

                const ProtoSparseList* oldRootSL =
                    (oldRoot == nullptr) ? context->newSparseList() : oldRoot;
                ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(
                    oldRootSL->setAt(context, ref, newState));
                ProtoSparseList* expected = oldRoot;
                if (globalMutableShards[shard].root.compare_exchange_weak(expected, newRoot)) {
                    refreshMutableCache(context, ref, newRoot, newState);
                    break;
                }
                if ((casIteration & 31) == 0) {
                    std::this_thread::yield();
                }
            }
            return this;
        }

        if (!oc->shape) return this;
        const proto_long off = findAttributeOffset(cache, oc->shape, name);
        if (off < 0) return this;
        const proto_ulong uoff = static_cast<proto_ulong>(off);
        if (getValueAt(oc, oc->shape->count, uoff) == nullptr) {
            return this;
        }
        ProtoContext::CriticalSection cs(context);
        return setSlotAt(context, oc, oc->shape, oc->shape->count, uoff, nullptr)->asObject(context);
    }

    const ProtoObject* ProtoObject::getFirstParent(ProtoContext* context) const {
        if (!proto::isObjectFast(this)) return PROTO_NONE;
        const ProtoObjectCell* oc = resolveOwnCell(context, this);

        if (oc->parent && ((uintptr_t)oc->parent & 0x3F) == 0) {
            auto pl = toImpl<const ParentLinkImplementation>(oc->parent);
            if (pl->getType() == CellType::ParentLink) return pl->getObject(context);
        }
        return PROTO_NONE;
    }

    const ProtoList* ProtoObject::getParents(ProtoContext* context) const {
        if (!proto::isObjectFast(this)) return context->newList();

        ProtoContext::CriticalSection cs(context);
        const ProtoObjectCell* oc = resolveOwnCell(context, this);

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
        if (!this || !target || !context) return 0;
        if (target == this) return 1;

        if (!proto::isObjectFast(this)) return 0;

        const ProtoObjectCell* oc = resolveOwnCell(context, this);
        const ParentLinkImplementation* link = oc->parent;
        while (link && ((uintptr_t)link & 0x3F) == 0) {
            auto pl = toImpl<const ParentLinkImplementation>(link);
            if (pl->getType() != CellType::ParentLink) break;
            if (pl->getObject(context) == target) return 1;
            link = pl->getParent(context);
        }
        return 0;
    }

    const ProtoObject* ProtoObject::addParentInternal(ProtoContext* context, const ProtoObject* newParent) const {
        if (!isCell(context)) return this;

        const auto* oc = toImpl<const ProtoObjectCell>(this);

        if (oc->isMutableCell()) {
             const auto* moc = static_cast<const ProtoMutableObjectCell*>(oc);
             const proto_ulong ref = moc->mutable_ref;
             int shard = ref % context->space->MUTABLE_ROOT_SHARDS;
             ProtoContext::CriticalSection cs(context);
             int casIteration = 0;
             while (true) {
                 ++casIteration;

                 ProtoSparseList* oldRoot = nullptr;
                 const ProtoObject* storedState = nullptr;
                 resolveMutableState(context, ref, &oldRoot, &storedState);

                 const ProtoObjectCell* currentOc = nullptr;
                 if (storedState != nullptr) {
                     currentOc = toImpl<const ProtoObjectCell>(storedState);
                 } else if (moc->initialState != nullptr) {
                     currentOc = moc->initialState;
                 } else {
                     currentOc = moc;
                 }

                 auto* newState = currentOc->addParent(context, newParent)->asObject(context);

                 const ProtoSparseList* oldRootSL =
                     (oldRoot == nullptr) ? context->newSparseList() : oldRoot;
                 ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(
                     oldRootSL->setAt(context, ref, newState));
                 ProtoSparseList* expected = oldRoot;
                 if (globalMutableShards[shard].root.compare_exchange_weak(expected, newRoot)) {
                     refreshMutableCache(context, ref, newRoot, newState);
                     break;
                 }
                 if ((casIteration & 31) == 0) {
                     std::this_thread::yield();
                 }
             }

             return this;
        }

        ProtoContext::CriticalSection cs(context);
        return oc->addParent(context, newParent)->asObject(context);
    }

    const ProtoObject* ProtoObject::addParent(ProtoContext* context, const ProtoObject* newParent) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT || !ProtoObject::isCellPointer(newParent)) return this;

        ProtoContext::CriticalSection cs(context);

        const ProtoObject* result = this;
        const ProtoList* mroOfNew = newParent->getParents(context);

        if (mroOfNew) {
            for (int i = static_cast<int>(mroOfNew->getSize(context)) - 1; i >= 0; --i) {
                const ProtoObject* ancestor = mroOfNew->getAt(context, i);
                if (ancestor != newParent && !result->hasParent(context, ancestor)) {
                    result = result->addParentInternal(context, ancestor);
                }
            }
        }

        if (!result->hasParent(context, newParent)) {
            result = result->addParentInternal(context, newParent);
        }

        return result;
    }

    const ProtoObject* ProtoObject::setParents(ProtoContext* context, const ProtoList* newParents) const {
        if (!this || !context) return this;
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return this;

        const auto* oc = toImpl<const ProtoObjectCell>(this);
        const bool isMut = oc->isMutableCell();

        FlatParentList flat;
        flattenParentsOrder(context, newParents, isMut ? this : nullptr, flat);

        ProtoContext::CriticalSection cs(context);
        const ParentLinkImplementation* newChain = buildParentChainFromFlat(context, flat);

        if (isMut) {
            const auto* moc = static_cast<const ProtoMutableObjectCell*>(oc);
            const proto_ulong ref = moc->mutable_ref;
            int shard = ref % context->space->MUTABLE_ROOT_SHARDS;
            int casIteration = 0;
            while (true) {
                ++casIteration;

                ProtoSparseList* oldRoot = nullptr;
                const ProtoObject* storedState = nullptr;
                resolveMutableState(context, ref, &oldRoot, &storedState);

                const ProtoObjectCell* currentOc = nullptr;
                if (storedState != nullptr) {
                    currentOc = toImpl<const ProtoObjectCell>(storedState);
                } else if (moc->initialState != nullptr) {
                    currentOc = moc->initialState;
                } else {
                    currentOc = moc;
                }

                auto* newState = (new(context) ProtoObjectCell(
                    context, newChain, currentOc->shape,
                    currentOc->rawSlots[0], currentOc->rawSlots[1],
                    currentOc->rawSlots[2], currentOc->rawSlots[3]))->asObject(context);

                const ProtoSparseList* oldRootSL =
                    (oldRoot == nullptr) ? context->newSparseList() : oldRoot;
                ProtoSparseList* newRoot = const_cast<ProtoSparseList*>(
                    oldRootSL->setAt(context, ref, newState));
                ProtoSparseList* expected = oldRoot;
                if (globalMutableShards[shard].root.compare_exchange_weak(expected, newRoot)) {
                    refreshMutableCache(context, ref, newRoot, newState);
                    break;
                }
                if ((casIteration & 31) == 0) {
                    std::this_thread::yield();
                }
            }

            return this;
        }

        return (new(context) ProtoObjectCell(
            context, newChain, oc->shape,
            oc->rawSlots[0], oc->rawSlots[1], oc->rawSlots[2], oc->rawSlots[3]))->asObject(context);
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
        uintptr_t raw = reinterpret_cast<uintptr_t>(pa.voidPointer) & ~PROTO_UL(0x3F);
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
    // SmallInteger first, in this function: the common case costs one tag test
    // and no further call.
    bool ProtoObject::isInteger(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.oid = this;
        if (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && p.op.embedded_type == EMBEDDED_TYPE_SMALLINT)
            return true;
        return proto::isInteger(this);
    }
    bool ProtoObject::isByte(ProtoContext* context) const {
        // protoCore has no distinct byte type: fromByte(char) is
        // fromInteger(char), and asByte reads byteValue.byteData, an 8-bit
        // field sitting after the same 6-bit tag and 4-bit embedded type as
        // si.smallInteger — i.e. the low byte of a SmallInteger.  So this
        // answers "is this a SmallInteger whose value fits in one byte",
        // over the range that survives the byte round trip: every char
        // fromByte can encode (-128..127) plus the unsigned 0..255 reading.
        if (!this) return false;
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_EMBEDDED_VALUE) return false;
        if (pa.op.embedded_type != EMBEDDED_TYPE_SMALLINT) return false;
        const proto_long value = pa.si.smallInteger;
        return value >= -128 && value <= 255;
    }
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
        const ProtoObjectCell* oc = resolveOwnCell(context, this);
        if (!oc->shape) return nullptr;

        AttributeCacheEntry* cache = getShapeCache(context);
        proto_ulong shapeCount = 0;
        const proto_long off = findAttributeOffset(cache, oc->shape, name, shapeCount);
        if (off < 0) return nullptr;
        return getValueAt(oc, shapeCount, static_cast<proto_ulong>(off));
    }

    proto_ulong ProtoObject::getHash(ProtoContext* context) const {
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

        // Strings hash by content, whatever their representation: an inline
        // string, or a heap string whose cell is its rope root (a node whose
        // own Cell::getHash would be its address).
        if (isStringTagFast(this)) return getProtoStringHash(context, this);

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
    bool ProtoObject::isMap(ProtoContext* context) const {
        if (!this) return false;
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_MAP) return true;
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->isMap(context);
        }
        return false;
    }

    const ProtoMap* ProtoObject::asMap(ProtoContext* context) const {
        if (!this) return nullptr;
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_MAP)
            return reinterpret_cast<const ProtoMap*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->asMap(context);
        }
        return nullptr;
    }
    bool ProtoObject::isMPSCQueue(ProtoContext* context) const {
        if (!this) return false;
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_MPSC_QUEUE) return true;
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->isMPSCQueue(context);
        }
        return false;
    }

    const ProtoMPSCQueue* ProtoObject::asMPSCQueue(ProtoContext* context) const {
        if (!this) return nullptr;
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_MPSC_QUEUE)
            return reinterpret_cast<const ProtoMPSCQueue*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->asMPSCQueue(context);
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
        if (pa.op.pointer_tag == POINTER_TAG_STRING) return reinterpret_cast<const ProtoString*>(this)->asList(context);
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_INLINE_STRING) return reinterpret_cast<const ProtoString*>(this)->asList(context);
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->asList(context);
        }
        return nullptr;
    }

    long long ProtoObject::asLong(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.oid = this;
        if (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && p.op.embedded_type == EMBEDDED_TYPE_SMALLINT)
            return p.si.smallInteger;
        return Integer::asLong(context, this);
    }
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
            if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE) return static_cast<double>(asLong(context));
            // A LargeInteger may not fit a long long (asLong throws), so
            // convert through its decimal digits; out of range becomes ±inf.
            std::string digits;
            Integer::toString(context, this, 10)->toUTF8String(context, digits);
            return std::strtod(digits.c_str(), nullptr);
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
    namespace {
        // Integer vs double, compared exactly: converting the integer to
        // double rounds above 2^53 (2**70 + 1 compared equal to 2.0**70) and
        // threw for integers beyond long long, so the integer is compared
        // with floor(d) as integers instead.  NaN is unordered; +/-infinity
        // lies beyond every integer.  Returns the order of `integer` relative
        // to `d`.
        std::partial_ordering compareIntegerWithDouble(ProtoContext* context,
                                                       const ProtoObject* integer, double d) {
            if (std::isnan(d)) return std::partial_ordering::unordered;
            if (std::isinf(d)) return d > 0 ? std::partial_ordering::less : std::partial_ordering::greater;
            const double floored = std::floor(d);
            const ProtoObject* flooredInt;
            if (floored >= -9.2e18 && floored <= 9.2e18) {
                flooredInt = Integer::fromLong(context, static_cast<long long>(floored));
            } else {
                char digits[400];
                std::snprintf(digits, sizeof(digits), "%.0f", floored);
                flooredInt = Integer::fromString(context, digits, 10);
            }
            const int c = Integer::compare(context, integer, flooredInt);
            if (c == 0 && d > floored) return std::partial_ordering::less;
            return c <=> 0;
        }
    }

    std::partial_ordering ProtoObject::partialCompare(ProtoContext* context, const ProtoObject* other) const {
        {
            // Two SmallIntegers: compared by value, before any type probe.
            ProtoObjectPointer a{}, b{};
            a.oid = this;
            b.oid = other;
            if (a.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && a.op.embedded_type == EMBEDDED_TYPE_SMALLINT &&
                b.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && b.op.embedded_type == EMBEDDED_TYPE_SMALLINT) {
                return static_cast<long long>(a.si.smallInteger) <=> static_cast<long long>(b.si.smallInteger);
            }
        }
        if (this->isString(context) && other->isString(context)) {
            return this->asString(context)->cmp_to_string(context, other->asString(context)) <=> 0;
        }
        const bool thisIsDouble = this->isDouble(context);
        const bool otherIsDouble = other->isDouble(context);
        if ((thisIsDouble || this->isInteger(context)) && (otherIsDouble || other->isInteger(context))) {
            if (thisIsDouble && otherIsDouble) {
                return this->asDouble(context) <=> other->asDouble(context);
            }
            if (thisIsDouble) {
                return 0 <=> compareIntegerWithDouble(context, other, this->asDouble(context));
            }
            if (otherIsDouble) {
                return compareIntegerWithDouble(context, this, other->asDouble(context));
            }
            return Integer::compare(context, this, other) <=> 0;
        }
        return this == other ? std::partial_ordering::equivalent : std::partial_ordering::unordered;
    }

    int ProtoObject::compare(ProtoContext* context, const ProtoObject* other) const {
        {
            // Two SmallIntegers: compared by value, before any type probe.
            ProtoObjectPointer a{}, b{};
            a.oid = this;
            b.oid = other;
            if (a.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && a.op.embedded_type == EMBEDDED_TYPE_SMALLINT &&
                b.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && b.op.embedded_type == EMBEDDED_TYPE_SMALLINT) {
                const long long x = a.si.smallInteger;
                const long long y = b.si.smallInteger;
                return (x > y) - (x < y);
            }
        }
        if (this->isString(context) && other->isString(context)) {
            return this->asString(context)->cmp_to_string(context, other->asString(context));
        }
        const bool thisIsNum = this->isDouble(context) || this->isInteger(context);
        const bool otherIsNum = other->isDouble(context) || other->isInteger(context);
        if (thisIsNum && otherIsNum) {
            const std::partial_ordering o = this->partialCompare(context, other);
            if (o < 0) return -1;
            if (o > 0) return 1;
            // Equivalent, or unordered because a NaN is involved: a NaN
            // compares as 0 with every number, as it always has.
            return 0;
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
        if (!this || !name || !context) return PROTO_FALSE;

        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                nonInterned::note(nonInterned::Has, PROTO_RETURN_ADDRESS());
                static const bool s_trust =
                    std::getenv("PROTOCORE_TRUST_SYMBOLS") != nullptr;
                if (s_trust) return PROTO_FALSE;
                const ProtoObject* sym = context->space->symbolTable->lookupByContent(
                    context, reinterpret_cast<const ProtoObject*>(name));
                if (!sym) return PROTO_FALSE;
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

        AttributeCacheEntry* cache = getShapeCache(context);
        const ProtoObject* currentPointer = this;
        const ParentLinkImplementation* currentLink = nullptr;

        while (currentPointer) {
            const bool isObj = (reinterpret_cast<uintptr_t>(currentPointer) & PROTO_UL(0x3F)) == POINTER_TAG_OBJECT;
            if (!isObj) {
                const ProtoObject* nextProto = currentPointer->getPrototype(context);
                if (nextProto == currentPointer) break;
                currentPointer = nextProto;
                continue;
            }

            const ProtoObjectCell* ocValue = resolveOwnCell(context, currentPointer);
            if (ocValue->shape != nullptr) {
                const proto_long off = findAttributeOffset(cache, ocValue->shape, name);
                if (off >= 0 && getValueAt(ocValue, ocValue->shape->count, static_cast<proto_ulong>(off)) != nullptr) {
                    return PROTO_TRUE;
                }
            }

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
        return PROTO_FALSE;
    }

    namespace {
        const ProtoSparseListImplementation* mergeObjectIntoSparseList(
            ProtoContext* context,
            const ProtoSparseListImplementation* attrs,
            const ProtoObjectCell* oc,
            bool overwriteExisting)
        {
            if (!oc || !oc->shape) return attrs;
            const proto_ulong total = oc->shape->count;
            for (const ProtoShape* b = oc->shape; b != nullptr; b = b->next) {
                const proto_ulong baseOff = (b->count - 1) & ~PROTO_UL(3);
                for (int i = 3; i >= 0; --i) {
                    const ProtoString* nm = b->names[i];
                    if (!nm) continue;
                    const proto_ulong off = baseOff + static_cast<proto_ulong>(3 - i);
                    const ProtoObject* val = getValueAt(oc, total, off);
                    if (val == nullptr) continue;
                    const proto_ulong key = reinterpret_cast<uintptr_t>(nm);
                    if (overwriteExisting || !attrs || attrs->implGetAt(context, key) == nullptr) {
                        attrs = attrs
                            ? attrs->implSetAt(context, key, val)
                            : new(context) ProtoSparseListImplementation(context, key, val, nullptr, nullptr, false);
                    }
                }
            }
            return attrs;
        }
    }

    const ProtoSparseList* ProtoObject::getAttributes(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) {
            const ProtoObject* prototype = getPrototype(context);
            return prototype ? prototype->getAttributes(context) : context->newSparseList();
        }

        ProtoContext::CriticalSection cs(context);

        const ProtoObjectCell* oc = resolveOwnCell(context, this);
        const ProtoSparseListImplementation* attrs = context->newSparseListImpl();
        attrs = mergeObjectIntoSparseList(context, attrs, oc, true);

        const ParentLinkImplementation* link = oc->parent;
        while (link && ((uintptr_t)link & 0x3F) == 0) {
            auto pl = toImpl<const ParentLinkImplementation>(link);
            if (pl->getType() != CellType::ParentLink) break;

            const ProtoObject* ancestor = pl->getObject(context);
            const ProtoObject* attrSource = ancestor;
            if (!proto::isObjectFast(attrSource)) {
                attrSource = attrSource ? attrSource->getPrototype(context) : nullptr;
            }

            if (attrSource && proto::isObjectFast(attrSource)) {
                const ProtoObjectCell* ancOc = resolveOwnCell(context, attrSource);
                attrs = mergeObjectIntoSparseList(context, attrs, ancOc, false);
            }

            link = pl->getParent(context);
        }

        return attrs ? attrs->asSparseList(context) : context->newSparseList();
    }

    const ProtoSparseList* ProtoObject::getOwnAttributes(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return context->newSparseList();

        ProtoContext::CriticalSection cs(context);
        const ProtoObjectCell* oc = resolveOwnCell(context, this);
        const ProtoSparseListImplementation* attrs = context->newSparseListImpl();
        attrs = mergeObjectIntoSparseList(context, attrs, oc, true);
        return attrs ? attrs->asSparseList(context) : context->newSparseList();
    }

    void ProtoObject::processOwnAttributes(
        ProtoContext* context, void* self,
        void (*method)(ProtoContext*, void*, const ProtoString*, const ProtoObject*)) const
    {
        if (!this || !context || !method) return;

        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return;

        auto oc = toImpl<const ProtoObjectCell>(this);
        const ProtoObject* snapshot = nullptr;
        const ProtoObjectCell* stateOc = oc;
        if (oc->isMutableCell()) {
            const auto* moc = static_cast<const ProtoMutableObjectCell*>(oc);
            const ProtoObject* storedState = resolveMutableSnapshot(context, moc->mutable_ref);
            if (storedState != nullptr) {
                snapshot = storedState;
                stateOc = toImpl<const ProtoObjectCell>(storedState);
            } else if (moc->initialState != nullptr) {
                snapshot = moc->initialState->asObject(context);
                stateOc = moc->initialState;
            }
        }
        if (!stateOc || !stateOc->shape) return;

        WalkPin pin(context, snapshot);

        const proto_ulong total = stateOc->shape->count;
        const ProtoShape* b = stateOc->shape;
        while (b != nullptr) {
            const proto_ulong baseOff = (b->count - 1) & ~PROTO_UL(3);
            const ProtoString* n0 = b->names[0];
            const ProtoString* n1 = b->names[1];
            const ProtoString* n2 = b->names[2];
            const ProtoString* n3 = b->names[3];
            const ProtoShape* nextBlock = b->next;

            if (n3) {
                const ProtoObject* v = getValueAt(stateOc, total, baseOff + 0);
                if (v != nullptr) method(context, self, n3, v);
            }
            if (n2) {
                const ProtoObject* v = getValueAt(stateOc, total, baseOff + 1);
                if (v != nullptr) method(context, self, n2, v);
            }
            if (n1) {
                const ProtoObject* v = getValueAt(stateOc, total, baseOff + 2);
                if (v != nullptr) method(context, self, n1, v);
            }
            if (n0) {
                const ProtoObject* v = getValueAt(stateOc, total, baseOff + 3);
                if (v != nullptr) method(context, self, n0, v);
            }

            b = nextBlock;
        }
    }

    const ProtoObject* ProtoObject::hasOwnAttribute(ProtoContext* context, const ProtoString* name) const {
        {
            ProtoObjectPointer pa{};
            pa.oid = reinterpret_cast<const ProtoObject*>(name);
            if (pa.op.pointer_tag == POINTER_TAG_STRING && context->space->symbolTable) {
                nonInterned::note(nonInterned::HasOwn, PROTO_RETURN_ADDRESS());
                static const bool s_trust =
                    std::getenv("PROTOCORE_TRUST_SYMBOLS") != nullptr;
                if (s_trust) return PROTO_FALSE;
                const ProtoObject* sym = context->space->symbolTable->lookupByContent(
                    context, reinterpret_cast<const ProtoObject*>(name));
                if (!sym) return PROTO_FALSE;
                name = reinterpret_cast<const ProtoString*>(sym);
            }
        }

        if (!proto::isObjectFast(this)) return PROTO_FALSE;
        const ProtoObjectCell* oc = resolveOwnCell(context, this);
        if (!oc->shape) return PROTO_FALSE;

        AttributeCacheEntry* cache = getShapeCache(context);
        const proto_long off = findAttributeOffset(cache, oc->shape, name);
        if (off < 0) return PROTO_FALSE;
        return context->fromBoolean(
            getValueAt(oc, oc->shape->count, static_cast<proto_ulong>(off)) != nullptr);
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
    
    proto_ulong ProtoObject::asTimestamp(ProtoContext* context) const {
        ProtoObjectPointer pa{};
        pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE) {
            return pa.timestampValue.timestamp;
        }
        return 0;
    }
    
    proto_long ProtoObject::asTimeDelta(ProtoContext* context) const {
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
