// ThreadCacheClearTests.cpp — per-thread caches cleared after a stop-the-world.
//
// The per-thread attributeCache and mutableValueCache are not GC roots.  Each
// thread clears both of its caches when it resumes after a stop-the-world
// (ProtoThreadExtension::clearCachesAfterStopTheWorld), before it can look
// anything up again: every entry a lookup can see was written after the last
// stop-the-world, so the cells it names were marked or young then and cannot
// be freed or reused before the next stop-the-world clears the entry.
//
// These tests check that:
//   * a mutableValueCache entry and an attributeCache entry written before a
//     cycle are gone once their owner has resumed from that cycle;
//   * new objects allocated at the addresses of collected, cached objects never
//     get the old cached results;
//   * a thread that spends a cycle in an unmanaged region has its caches
//     cleared when it returns;
//   * a thread whose heap-headroom wait spans a cycle has its caches cleared
//     when the wait returns.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace proto;

namespace {

void requestGcCycle(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    space.gcStarted = true;
    space.gcCV.notify_all();
}

// Runs one collection cycle that starts after the call, cooperating with its
// stop-the-world through safepoint(), and returns when it has finished.
bool runGcCycle(ProtoSpace& space, ProtoContext* ctx) {
    const uint64_t before = space.getGCCycleCount();
    requestGcCycle(space);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (space.gcStarted.load() || space.getGCCycleCount() == before) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        if (!space.gcStarted.load() && space.getGCCycleCount() == before) requestGcCycle(space);
        ctx->safepoint();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

ProtoThreadExtension* extensionOf(ProtoContext* ctx) {
    return toImpl<ProtoThreadImplementation>(ctx->thread)->extension;
}

unsigned long attributeSlot(const ProtoObject* object, const ProtoString* name) {
    return ((reinterpret_cast<uintptr_t>(object) >> 6) ^
            (reinterpret_cast<uintptr_t>(name) >> 4)) % THREAD_CACHE_DEPTH;
}

}  // namespace

TEST(ThreadCacheClear, MutableValueEntryIsClearedWhenTheOwnerResumesAfterACycle) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    ASSERT_NE(ctx->thread, nullptr);
    const ProtoString* key = ProtoString::createSymbol(ctx, "value");

    const ProtoObject* object = ctx->newObject(true);
    object->setAttribute(ctx, key, ctx->fromInteger(1));
    const unsigned long ref = toImpl<const ProtoObjectCell>(object)->mutable_ref;
    ProtoThreadExtension* ext = extensionOf(ctx);
    MutableValueCacheEntry& entry = ext->mutableValueCache[ref % MUTABLE_VALUE_CACHE_DEPTH];

    ASSERT_EQ(object->getAttribute(ctx, key)->asLong(ctx), 1);
    ASSERT_EQ(entry.mutable_ref, ref) << "the lookup must have filled the entry";

    ASSERT_TRUE(runGcCycle(space, ctx));
    EXPECT_EQ(entry.mutable_ref, 0u) << "the owner must clear its cache when it resumes after a cycle";
    EXPECT_EQ(entry.shard_root, nullptr);
    EXPECT_EQ(entry.current_value, nullptr);
    EXPECT_EQ(ext->lastClearedEpoch, space.getGCCycleCount());

    EXPECT_EQ(object->getAttribute(ctx, key)->asLong(ctx), 1);
    EXPECT_EQ(entry.mutable_ref, ref) << "a lookup after the cycle refills the entry";
}

TEST(ThreadCacheClear, AttributeEntryIsClearedWhenTheOwnerResumesAfterACycle) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    ASSERT_NE(ctx->thread, nullptr);
    const ProtoString* key = ProtoString::createSymbol(ctx, "value");

    const ProtoObject* object = ctx->newObject(false)->setAttribute(ctx, key, ctx->fromInteger(1));
    AttributeCacheEntry& entry = extensionOf(ctx)->attributeCache[attributeSlot(object, key)];
    ASSERT_EQ(object->getAttribute(ctx, key)->asLong(ctx), 1);
    ASSERT_EQ(entry.object, object) << "the lookup must have filled the entry";

    ASSERT_TRUE(runGcCycle(space, ctx));
    EXPECT_EQ(entry.object, nullptr) << "the owner must clear its cache when it resumes after a cycle";
    EXPECT_EQ(entry.result, nullptr);
    EXPECT_EQ(entry.name, nullptr);

    EXPECT_EQ(object->getAttribute(ctx, key)->asLong(ctx), 1);
    EXPECT_EQ(entry.object, object) << "a lookup after the cycle refills the entry";
}

// Objects whose lookups are cached are dropped and collected; the cells are
// then reused.  Whenever a new object lands at the address of a cached one,
// its lookup must return its own value.  With untraced caches that were never
// cleared, the old entry (same address, same name, same slot) would hit.
TEST(ThreadCacheClear, ReusedAddressNeverHitsAStaleAttributeEntry) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const ProtoString* key = ProtoString::createSymbol(ctx, "value");

    constexpr int kCached = 1000;
    std::unordered_map<uintptr_t, int> cachedAddresses;
    {
        ProtoContext sub(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kCached; ++i) {
            const ProtoObject* object = sub.newObject(false)->setAttribute(&sub, key, sub.fromInteger(1));
            ASSERT_EQ(object->getAttribute(&sub, key)->asLong(&sub), 1);  // fills the cache
            cachedAddresses[reinterpret_cast<uintptr_t>(object)] = i;
        }
    }

    for (int cycle = 0; cycle < 3; ++cycle) ASSERT_TRUE(runGcCycle(space, ctx));

    int reused = 0;
    int falseHits = 0;
    constexpr int kBatch = 5000;
    for (int batch = 0; batch < 200 && reused < 50; ++batch) {
        ProtoContext sub(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kBatch; ++i) {
            // Sweep returns a segment's dead cells in allocation order, so an
            // identical allocation pattern replays the old cell sequence with a
            // fixed offset; varying padding moves new object cells across every
            // offset.
            for (int pad = 0; pad < (i + batch) % 7; ++pad) (void) sub.newList();
            const ProtoObject* object = sub.newObject(false)->setAttribute(&sub, key, sub.fromInteger(2));
            if (cachedAddresses.count(reinterpret_cast<uintptr_t>(object))) {
                ++reused;
                const ProtoObject* value = object->getAttribute(&sub, key);
                if (!value || value->asLong(&sub) != 2) ++falseHits;
            }
        }
    }
    ASSERT_GT(reused, 0) << "no cached address was reused; the test did not exercise address reuse";
    EXPECT_EQ(falseHits, 0) << "a new object at a reused address got a stale cached attribute";
}

namespace {

struct UnmanagedShared {
    const ProtoString* key = nullptr;
    std::atomic<bool> inRegion{false};
    std::atomic<bool> leave{false};
    std::atomic<bool> done{false};
    std::atomic<bool> filledBefore{false};
    std::atomic<bool> stillFilledWhileUnmanaged{false};
    std::atomic<bool> clearedAfterReturn{false};
    std::atomic<bool> epochCurrentAfterReturn{false};
};
UnmanagedShared* gUnmanaged = nullptr;

const ProtoObject* unmanagedThreadMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                       const ProtoList*, const ProtoSparseList*) {
    UnmanagedShared& shared = *gUnmanaged;
    ProtoThreadExtension* ext = extensionOf(ctx);
    const ProtoObject* object = ctx->newObject(true);
    object->setAttribute(ctx, shared.key, ctx->fromInteger(7));
    const unsigned long ref = toImpl<const ProtoObjectCell>(object)->mutable_ref;
    const MutableValueCacheEntry& entry = ext->mutableValueCache[ref % MUTABLE_VALUE_CACHE_DEPTH];
    (void) object->getAttribute(ctx, shared.key);
    shared.filledBefore = (entry.mutable_ref == ref);
    {
        ProtoContext::UnmanagedScope parked(ctx);
        shared.inRegion = true;
        while (!shared.leave.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // Reading this thread's own cache memory is not ProtoObject access.
        shared.stillFilledWhileUnmanaged = (entry.mutable_ref == ref);
    }  // returnFromUnmanaged: the caches are cleared here
    shared.clearedAfterReturn = (entry.mutable_ref == 0);
    shared.epochCurrentAfterReturn = (ext->lastClearedEpoch == ctx->space->getGCCycleCount());
    shared.done = true;
    ProtoContext::UnmanagedScope parked(ctx);
    return PROTO_NONE;
}

}  // namespace

TEST(ThreadCacheClear, CachesAreClearedOnReturnFromAnUnmanagedRegion) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    UnmanagedShared shared;
    shared.key = ProtoString::createSymbol(root, "value");
    gUnmanaged = &shared;

    const ProtoThread* worker = space.newThread(root, ProtoString::createSymbol(root, "unmanaged-worker"),
                                                unmanagedThreadMain, nullptr, nullptr);
    {
        ProtoContext::UnmanagedScope parked(root);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!shared.inRegion.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ASSERT_TRUE(shared.inRegion.load());
    // The worker counts as parked while unmanaged, so cycles run without it.
    for (int cycle = 0; cycle < 2; ++cycle) ASSERT_TRUE(runGcCycle(space, root));
    shared.leave = true;
    {
        ProtoContext::UnmanagedScope parked(root);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!shared.done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const_cast<ProtoThread*>(worker)->join(root);
    }
    gUnmanaged = nullptr;

    EXPECT_TRUE(shared.filledBefore.load());
    EXPECT_TRUE(shared.stillFilledWhileUnmanaged.load())
        << "caches are cleared when the thread returns, not while it is unmanaged";
    EXPECT_TRUE(shared.clearedAfterReturn.load())
        << "a thread returning from an unmanaged region across a cycle must clear its caches";
    EXPECT_TRUE(shared.epochCurrentAfterReturn.load());
}

// The heap-headroom wait takes the thread out of the running set; a whole
// cycle can complete while it waits, and the thread may then find no pending
// stop-the-world to park for.  Its caches must still be cleared when the wait
// returns.
TEST(ThreadCacheClear, CachesAreClearedWhenAHeadroomWaitSpansACycle) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    ASSERT_NE(root->thread, nullptr);
    const ProtoString* key = ProtoString::createSymbol(root, "value");
    ProtoThreadExtension* ext = extensionOf(root);

    ProtoContext work(&space, root, nullptr, nullptr, nullptr, nullptr);
    const ProtoObject* object = work.newObject(true);
    object->setAttribute(&work, key, work.fromInteger(3));
    const unsigned long ref = toImpl<const ProtoObjectCell>(object)->mutable_ref;
    const MutableValueCacheEntry& entry = ext->mutableValueCache[ref % MUTABLE_VALUE_CACHE_DEPTH];

    const uint64_t cyclesStart = space.getGCCycleCount();
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize);
    bool sawClear = false;
    for (int round = 0; round < 8 && !sawClear; ++round) {
        (void) object->getAttribute(&work, key);  // refill the entry
        ASSERT_EQ(entry.mutable_ref, ref);
        {
            // Drain the space-level freelist with allocations that open no
            // critical section, so no heap checkpoint runs here.
            ProtoContext garbage(&space, &work, nullptr, nullptr, nullptr, nullptr);
            for (int batch = 0; batch < 100000 && (space.freeChunks || space.freeCells); ++batch) {
                for (int k = 0; k < 256; ++k) (void) garbage.newList();
            }
        }
        const uint64_t beforeWait = space.getGCCycleCount();
        // The first outermost critical section waits for headroom in `work`.
        (void) work.newObject(false);
        if (space.getGCCycleCount() != beforeWait) {
            sawClear = true;
            EXPECT_EQ(entry.mutable_ref, 0u)
                << "the caches must be cleared when a headroom wait spanned a cycle";
            EXPECT_EQ(ext->lastClearedEpoch, space.getGCCycleCount());
        }
    }
    space.setHeapLimits(0, 0);
    EXPECT_TRUE(sawClear) << "no headroom wait spanned a cycle; the test did not exercise the wait";
    EXPECT_GE(space.getGCCycleCount() - cyclesStart, 1u);
    EXPECT_EQ(object->getAttribute(&work, key)->asLong(&work), 3);
}
