// SpaceTeardownTests.cpp -- what a cycle that is still running when its
// space is destroyed does with the space's entries in the global mutable
// table.
//
// ~ProtoSpace records the space as destroyed and then joins its collector,
// whose cycle in progress stops sweeping at its next claim of segments
// (DestroyedDuringItsSweepStopsTheSweep, below).  That cycle's Phase 5b used to find
// its own space in the destroyed list and remove every live entry of the
// space from the table -- a walk of the whole table plus a removal of all of
// the space's entries, at exit, while the destructor waited.  A runtime
// measured about 30 % of a 90 s shutdown there.  The entries are now left to
// the next cycle of a live space, exactly as when the space is destroyed with
// its collector idle.
//
// To make "a cycle in progress at destruction" deterministic, the dying
// space's sweep is held inside a finalizer until the destructor has begun.
// (A finalizer must never block; this one does so only to pin the
// interleaving, and only for a bounded time.)

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

using namespace proto;

namespace {

std::atomic<bool> g_inFinalizer{false};
std::atomic<int> g_probesFinalized{0};
void countingFinalizer(void*) { g_probesFinalized.fetch_add(1); }
std::atomic<bool> g_releaseFinalizer{false};

void holdingFinalizer(void*) {
    g_inFinalizer.store(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!g_releaseFinalizer.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

uint64_t forceCycles(ProtoSpace& space, ProtoContext* parent, uint64_t minCycles) {
    const uint64_t start = space.getGCCycleCount();
    space.setHeapLimits(0, space.heapSize + 40000);
    for (int batch = 0; batch < 400 && space.getGCCycleCount() - start < minCycles; ++batch) {
        ProtoContext garbage(&space, parent, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 5000; ++i) {
            (void) garbage.newObject(false);
            if ((i & 1023) == 0) garbage.safepoint();
        }
        garbage.safepoint();
    }
    space.setHeapLimits(0, 0);
    return space.getGCCycleCount() - start;
}

}  // namespace

TEST(SpaceTeardown, CycleInProgressLeavesTheSpacesLiveEntriesToALiveSpace) {
    constexpr proto_ulong kLive = 200;
    ProtoSpace a;
    ProtoContext actx(&a, a.rootContext, nullptr, nullptr, nullptr, nullptr);

    auto* b = new ProtoSpace();
    const proto_ulong bId = spaceIdOf(b);
    {
        auto* bctx = new ProtoContext(b, b->rootContext, nullptr, nullptr, nullptr, nullptr, 1);
        const ProtoString* key = ProtoString::createSymbol(bctx, "value");
        const ProtoList* keep = bctx->newList();
        for (proto_ulong i = 0; i < kLive; ++i) {
            const ProtoObject* o = bctx->newObject(true);
            o->setAttribute(bctx, key, bctx->fromInteger(static_cast<long long>(i)));
            keep = keep->appendLast(bctx, o);
        }
        // Rooted for good: the root context outlives every cycle of b.
        b->createRootSet("teardown-test")->add(keep->asObject(bctx));
        // Garbage whose finalizer holds b's sweep.
        {
            ProtoContext garbage(b, bctx, nullptr, nullptr, nullptr, nullptr);
            (void) garbage.fromExternalPointer(nullptr, &holdingFinalizer);
        }
        delete bctx;
    }
    ASSERT_EQ(multispace::countMutableEntriesOfSpace(&actx, bId), kLive);

    g_inFinalizer = false;
    g_releaseFinalizer = false;
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        b->gcStarted = true;
        b->gcCV.notify_all();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!g_inFinalizer.load() && std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            b->gcStarted = true;
            b->gcCV.notify_all();
        }
        ProtoContext::UnmanagedScope parked(&actx);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(g_inFinalizer.load()) << "b's collector never swept the held cell";

    // Release the finalizer only once the destructor has begun, so the rest
    // of the cycle -- Phase 5b included -- runs with the space ENDING.
    std::thread releaser([b]() {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        for (;;) {
            bool ending;
            {
                std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
                ending = b->state == SPACE_STATE_ENDING;
            }
            if (ending || std::chrono::steady_clock::now() > until) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        g_releaseFinalizer.store(true);
    });
    {
        ProtoContext::UnmanagedScope parked(&actx);
        delete b;
    }
    releaser.join();

    // b's last cycle must not have purged b's live entries itself ...
    const proto_ulong afterDestroy = multispace::countMutableEntriesOfSpace(&actx, bId);
    // ... and a cycle of the live space a purges them.
    ASSERT_GE(forceCycles(a, &actx, 2), 2u);
    const proto_ulong afterLiveCycle = multispace::countMutableEntriesOfSpace(&actx, bId);
    std::printf("[ Teardown ] live entries of b: after destroy %llu, after a's cycles %llu\n",
                (unsigned long long) afterDestroy, (unsigned long long) afterLiveCycle);
    EXPECT_EQ(afterDestroy, kLive)
        << "the cycle in progress at destruction walked the table and removed the space's live entries";
    EXPECT_EQ(afterLiveCycle, 0u) << "a live space's cycle did not purge the destroyed space's entries";
}

// The space being destroyed is the only one, so its sweep runs finalizers in
// place, and a finalizer can hold the sweep at its first segment until the
// destructor has begun.  Two thousand further segments, each with one counted
// probe, follow it.  Once the destructor raises `ending`, the sweep claims no
// further segment: most probes are never finalized (none runs at exit), the
// destructor returns without sweeping them, and a later space collects
// normally -- the cycle handed on the token -- and purges the destroyed
// space's table entries.
TEST(SpaceTeardown, DestroyedDuringItsSweepStopsTheSweep) {
    constexpr proto_ulong kLive = 200;
    constexpr int kProbeSegments = 2000;
    auto* b = new ProtoSpace();
    const proto_ulong bId = spaceIdOf(b);
    g_probesFinalized.store(0);
    {
        auto* bctx = new ProtoContext(b, b->rootContext, nullptr, nullptr, nullptr, nullptr, 1);
        const ProtoString* key = ProtoString::createSymbol(bctx, "value");
        const ProtoList* keep = bctx->newList();
        for (proto_ulong i = 0; i < kLive; ++i) {
            const ProtoObject* o = bctx->newObject(true);
            o->setAttribute(bctx, key, bctx->fromInteger(static_cast<long long>(i)));
            keep = keep->appendLast(bctx, o);
        }
        b->createRootSet("teardown-test")->add(keep->asObject(bctx));
        // One segment per destroyed context.  The list of segments is a
        // stack, so the holder -- pushed last -- is swept first.
        for (int i = 0; i < kProbeSegments; ++i) {
            ProtoContext garbage(b, bctx, nullptr, nullptr, nullptr, nullptr);
            (void) garbage.fromExternalPointer(nullptr, &countingFinalizer);
        }
        {
            ProtoContext garbage(b, bctx, nullptr, nullptr, nullptr, nullptr);
            (void) garbage.fromExternalPointer(nullptr, &holdingFinalizer);
        }
        delete bctx;
    }

    g_inFinalizer = false;
    g_releaseFinalizer = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!g_inFinalizer.load() && std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            b->gcStarted = true;
            b->gcCV.notify_all();
        }
        // This thread constructed b, so b's stop-the-world waits for it.
        ProtoContext::UnmanagedScope parked(b->rootContext);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(g_inFinalizer.load()) << "b's collector never swept the held cell";
    const int finalizedBeforeDestroy = g_probesFinalized.load();

    std::thread releaser([b]() {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        for (;;) {
            bool ending;
            {
                std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
                ending = b->state == SPACE_STATE_ENDING;
            }
            if (ending || std::chrono::steady_clock::now() > until) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        g_releaseFinalizer.store(true);
    });
    delete b;
    releaser.join();
    const int finalizedByB = g_probesFinalized.load();

    ProtoSpace c;
    ProtoContext cctx(&c, c.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    const uint64_t cCycles = forceCycles(c, &cctx, 2);
    const proto_ulong bEntries = multispace::countMutableEntriesOfSpace(&cctx, bId);
    std::printf("[ Teardown ] stopped sweep: probes finalized before destroy %d, by b in all %d of %d; "
                "c cycles %llu; b entries after c's cycles %llu\n",
                finalizedBeforeDestroy, finalizedByB, kProbeSegments,
                (unsigned long long) cCycles, (unsigned long long) bEntries);
    EXPECT_EQ(finalizedBeforeDestroy, 0) << "the holder was not the first segment swept";
    // At most one claim's worth of segments (sweep::kClaimRun) plus those
    // already loaded in the cursors are swept after the flag is raised.
    EXPECT_LT(finalizedByB, kProbeSegments / 4) << "the sweep went on after its space began to be destroyed";
    EXPECT_GE(cCycles, 2u) << "a later space does not collect (cycle token not handed on?)";
    EXPECT_EQ(bEntries, 0u) << "the destroyed space's table entries were not purged";
}
