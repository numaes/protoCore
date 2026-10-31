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
#include "../core/Sweep.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

namespace {

// Holds the sweep of `g_heldSpace` until released (bounded).
std::atomic<ProtoSpace*> g_heldSpace{nullptr};
std::atomic<bool> g_sweepHeld{false};
std::atomic<bool> g_releaseSweep{false};
void holdBeforeSweep(ProtoSpace* space) {
    if (space != g_heldSpace.load()) return;
    g_sweepHeld.store(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!g_releaseSweep.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// A ProtoThread that stays in managed code without passing a safepoint until
// released (bounded): while it runs, no grace period of the process can end.
std::atomic<bool> g_blockerRunning{false};
std::atomic<bool> g_releaseBlocker{false};
const ProtoObject* graceBlockerMain(ProtoContext*, const ProtoObject*, const ParentLink*,
                                    const ProtoArguments*, const ProtoKeywordArguments*) {
    g_blockerRunning.store(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!g_releaseBlocker.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return PROTO_NONE;
}

}  // namespace

// With another space live the sweep only collects dead cells; they are
// finalized after a grace period that every registered thread of the process
// must pass.  A space destroyed before its sweep runs abandons the sweep, and
// then neither waits for that grace period nor finalizes anything: protoCore
// runs no finalizer at exit, and the destructor must not wait on threads of
// other spaces.  It still settles mutableReleasesPending, so later cycles of
// the live space run, and those purge the destroyed space's table entries.
//
// Deterministic: the collector of b is held just before its sweep (a test
// hook) until ~ProtoSpace has raised `ending`, so the sweep stops at its first
// claim; and a ProtoThread of the live space stays in managed code without a
// safepoint across the destructor, so a grace period could not end.  Waiting
// for one would hold the destructor until that thread's 15 s bound.
TEST(SpaceTeardown, DeferredFreeSpaceDestroyedBeforeItsSweepWaitsForNoGracePeriod) {
    constexpr proto_ulong kLive = 200;
    constexpr int kProbes = 200;
    ProtoSpace a;
    ProtoContext actx(&a, a.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    auto* b = new ProtoSpace();
    const proto_ulong bId = spaceIdOf(b);
    ASSERT_GE(ProtoSpace::liveSpaceCount(), 2u);
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
        for (int i = 0; i < kProbes; ++i) {
            ProtoContext garbage(b, bctx, nullptr, nullptr, nullptr, nullptr);
            (void) garbage.fromExternalPointer(nullptr, &countingFinalizer);
        }
        delete bctx;
    }

    g_heldSpace.store(b);
    g_sweepHeld.store(false);
    g_releaseSweep.store(false);
    sweep::setBeforeSweepForTest(&holdBeforeSweep);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!g_sweepHeld.load() && std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            b->gcStarted = true;
            b->gcCV.notify_all();
        }
        ProtoContext::UnmanagedScope parked(&actx);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(g_sweepHeld.load()) << "b's collector never reached its sweep";

    g_blockerRunning.store(false);
    g_releaseBlocker.store(false);
    const ProtoThread* blocker = a.newThread(&actx, ProtoString::createSymbol(&actx, "blocker"),
                                             graceBlockerMain, nullptr, nullptr);
    while (!g_blockerRunning.load()) std::this_thread::yield();

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
        g_releaseSweep.store(true);
    });
    const auto t0 = std::chrono::steady_clock::now();
    {
        ProtoContext::UnmanagedScope parked(&actx);
        delete b;
    }
    const double destroySeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const bool blockerStillRunning = !g_releaseBlocker.load() && g_blockerRunning.load();
    g_releaseBlocker.store(true);
    releaser.join();
    sweep::setBeforeSweepForTest(nullptr);
    g_heldSpace.store(nullptr);
    {
        ProtoContext::UnmanagedScope parked(&actx);
        const_cast<ProtoThread*>(blocker)->join(&actx);
    }
    const int finalized = g_probesFinalized.load();
    const int pending = multispace::mutableReleasesPending.load();

    const uint64_t aCycles = forceCycles(a, &actx, 2);
    const proto_ulong bEntries = multispace::countMutableEntriesOfSpace(&actx, bId);
    std::printf("[ Teardown ] deferred free: destroy %.3f s, probes finalized %d of %d, "
                "releases pending %d, a cycles %llu, b entries after %llu\n",
                destroySeconds, finalized, kProbes, pending,
                (unsigned long long) aCycles, (unsigned long long) bEntries);
    EXPECT_TRUE(blockerStillRunning) << "the blocker ended before the destructor returned";
    EXPECT_LT(destroySeconds, 5.0) << "the destructor waited for a grace period";
    EXPECT_EQ(finalized, 0) << "a finalizer ran for a space being destroyed";
    EXPECT_EQ(pending, 0) << "the abandoned cycle did not settle mutableReleasesPending";
    EXPECT_GE(aCycles, 2u) << "the live space does not collect after the destruction";
    EXPECT_EQ(bEntries, 0u) << "the destroyed space's table entries were not purged";
}

namespace {

std::atomic<long> g_spinIterations{0};
std::atomic<bool> g_sleeperStarted{false};
std::atomic<bool> g_sleeperReturned{false};

// Allocates and reads and writes attributes without end, passing safepoints.
const ProtoObject* spinnerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                               const ProtoArguments*, const ProtoKeywordArguments*) {
    const ProtoString* key = ProtoString::createSymbol(ctx, "spinner-key");
    for (long i = 0;; ++i) {
        const ProtoObject* o = ctx->newObject(true);
        o->setAttribute(ctx, key, ctx->fromInteger(i));
        (void) o->getAttribute(ctx, key);
        const ProtoObject* frozen = ctx->newObject(false)->setAttribute(ctx, key, o);
        (void) frozen->hasAttribute(ctx, key);
        g_spinIterations.fetch_add(1);
        if ((i & 255) == 0) ctx->safepoint();
    }
}

// Sleeps (out of the quorum, in an unmanaged region) past the destruction.
const ProtoObject* sleeperMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                               const ProtoArguments*, const ProtoKeywordArguments*) {
    g_sleeperStarted.store(true);
    ProtoThread::sleepFor(ctx, 1500 * 1000);
    g_sleeperReturned.store(true);
    (void) ctx->newObject(false);
    return PROTO_NONE;
}

}  // namespace

// A space destroyed while two of its ProtoThreads still run -- one spinning
// in allocation and attribute access, one asleep in an unmanaged region --
// stops both for good (the terminal stop in ~ProtoSpace) instead of freeing
// the space under them.  The destructor returns promptly; the spinner makes no
// further iteration; the sleeper, waking after the destruction, never returns
// from sleepFor.  Run under ASan and TSan, a thread that touched the destroyed
// space would be reported.  (The two threads stay blocked until the process
// exits; they are never joined.)
TEST(SpaceTeardown, ThreadsStillRunningAreStoppedForGood) {
    auto* b = new ProtoSpace();
    g_spinIterations.store(0);
    g_sleeperStarted.store(false);
    g_sleeperReturned.store(false);
    (void) b->newThread(b->rootContext, ProtoString::createSymbol(b->rootContext, "spinner"),
                        spinnerMain, nullptr, nullptr);
    (void) b->newThread(b->rootContext, ProtoString::createSymbol(b->rootContext, "sleeper"),
                        sleeperMain, nullptr, nullptr);
    {
        // This thread constructed b: out of its quorum while it waits, so the
        // spinner's collections do not wait for it.
        ProtoContext::UnmanagedScope parked(b->rootContext);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while ((g_spinIterations.load() < 20000 || !g_sleeperStarted.load())
               && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_GE(g_spinIterations.load(), 20000);
    ASSERT_TRUE(g_sleeperStarted.load());

    const auto t0 = std::chrono::steady_clock::now();
    delete b;
    const double destroySeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const long atDestroy = g_spinIterations.load();
    // Past the sleeper's 1.5 s: it has woken up by now.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const long after = g_spinIterations.load();
    std::printf("[ Teardown ] running threads: destroy %.3f s, spinner iterations %ld then %ld, "
                "sleeper returned %d\n", destroySeconds, atDestroy, after,
                (int) g_sleeperReturned.load());
    EXPECT_LT(destroySeconds, 5.0);
    EXPECT_EQ(after, atDestroy) << "the spinner ran on after its space was destroyed";
    EXPECT_FALSE(g_sleeperReturned.load()) << "the sleeper returned into a destroyed space";
}

// A space built on a helper thread that exits while the space lives on, then
// destroyed from another thread -- what a runtime's worker does when its
// GC thread frees the worker's state.  The space counted its constructing
// thread as running from construction; that count must leave with the thread,
// or every later stop-the-world, ~ProtoSpace's terminal stop among them, waits
// for a thread that no longer exists.
TEST(SpaceTeardown, ASpaceOutlivingItsConstructingThreadIsStillDestroyed) {
    ProtoSpace* b = nullptr;
    g_spinIterations.store(0);
    std::thread builder([&b] {
        b = new ProtoSpace();
        (void) b->newThread(b->rootContext, ProtoString::createSymbol(b->rootContext, "spinner"),
                            spinnerMain, nullptr, nullptr);
    });
    builder.join();
    ASSERT_NE(b, nullptr);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (g_spinIterations.load() < 20000 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_GE(g_spinIterations.load(), 20000);

    // Destroyed from this thread, as protoJS does: a helper destroying it
    // could be handed the builder's exited id by the OS and pass for the main
    // thread.  A watchdog turns a hang into a failure of the suite.
    std::atomic<bool> destroyed{false};
    std::thread watchdog([&destroyed] {
        const auto t0 = std::chrono::steady_clock::now();
        while (!destroyed.load()) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10)) {
                std::fprintf(stderr, "~ProtoSpace waited for a constructing thread that had exited\n");
                std::abort();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    delete b;
    destroyed.store(true);
    watchdog.join();
    const double destroySeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[ Teardown ] constructing thread gone: destroy %.3f s\n", destroySeconds);
    EXPECT_LT(destroySeconds, 5.0);
    const long atDestroy = g_spinIterations.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(g_spinIterations.load(), atDestroy) << "the spinner ran on after its space was destroyed";
}
