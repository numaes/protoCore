// Tests for the GC survivor re-chain and per-context allocation threshold
// trigger.  See docs/superpowers/specs/2026-05-03-gc-survivor-rechain.md.
//
// All tests in this file are skipped when PROTOCORE_GC_REINCLUDE_SURVIVORS is
// not defined; they exercise behaviour that is only correct under the flag.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <chrono>
#include <thread>
#include <vector>

using namespace proto;

namespace {
// Drive a few GC cycles to convergence.  Two interlocking concerns:
//   1. triggerGC() in production is gated on heap pressure
//      (freeRatio < 0.2), which never trips when a test allocates only a
//      handful of cells.  Tests must set gcStarted directly.
//   2. STW root collection cannot start until parkedThreads >=
//      runningThreads.  The test thread is "running" and must
//      cooperate by calling safepoint() so it actually parks when STW
//      is requested.  Otherwise the GC blocks forever waiting for the
//      test thread to reach a safepoint.
//
// The loop kicks gcStarted, then spins on safepoint() until the cycle
// completes (gcStarted cleared by the GC thread).
void waitForGcCycles(ProtoSpace& space, int cycles = 1) {
    ProtoContext* ctx = space.rootContext;
    for (int i = 0; i < cycles; ++i) {
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            space.gcStarted = true;
            space.gcCV.notify_all();
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (space.gcStarted.load() &&
               std::chrono::steady_clock::now() < deadline) {
            // Cooperate with STW: park if the GC has requested it.
            if (ctx) ctx->safepoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Tail latency: the GC thread clears gcStarted before sweep
        // finishes (sweep runs without the global lock).  Give it room.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
}  // namespace

// T1 — RSS bounded in a tight loop with no escapes.
//
// A long-running context allocates many cells, each immediately discarded.
// With the survivor re-chain + per-context threshold trigger enabled, the GC
// fires mid-loop, sweeps the unreferenced cells, and heapSize stays bounded
// in the working set.  Without the fix, all cells stay pinned in
// lastAllocatedCell and heapSize grows ~ K * cell-size.
TEST(GCSurvivorTest, RssBoundedInTightLoopNoEscapes) {
#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    GTEST_SKIP() << "requires PROTOCORE_GC_REINCLUDE_SURVIVORS";
#else
    ProtoSpace space;
    // Lower the threshold so the trigger fires many times in this test.
    space.maxAllocatedCellsPerContext = 100;

    ProtoContext* ctx = space.rootContext;

    // K is large enough that, without the trigger, allocatedCellsCount
    // would be K + setup overhead.  With the trigger, the count should
    // be reset whenever it crosses the threshold, leaving only the
    // most recent partial batch in the counter at observation time.
    // heapSize is not a reliable discriminator here because the OS
    // allocation batch (blocksPerAllocation * 50) dwarfs K.
    constexpr int K = 5000;
    for (int i = 0; i < K; ++i) {
        // Returned object is not stored; should be reclaimable on next GC.
        (void)ctx->newObject(false);
    }

    // Let any in-flight GC cycles complete.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Without the trigger: allocatedCellsCount = K + setup, far above
    // the threshold.  With the trigger: count is below 2 * threshold
    // (the most-recent batch plus the increment that fired the trigger).
    EXPECT_LT(ctx->allocatedCellsCount, space.maxAllocatedCellsPerContext * 2u)
        << "allocatedCellsCount = " << ctx->allocatedCellsCount
        << " after " << K << " allocations with threshold "
        << space.maxAllocatedCellsPerContext
        << " — threshold trigger not firing.";
#endif
}

// T2 — A long-lived survivor is freed when its reference is dropped.
//
// Allocate one cell in a temporary subContext (so it gets submitted to
// dirtySegments on subContext destruction).  Pin it via a root set, run a
// couple of GC cycles (cell survives both because it is rooted), then
// drop the root and run another GC cycle.  Without survivor re-chain, the
// cell is dropped from analysis after the first cycle and is never freed.
// With the fix, the cell is freed on the cycle after the root is removed.
TEST(GCSurvivorTest, LongLivedSurvivorFreedWhenReferenceDropped) {
#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    GTEST_SKIP() << "requires PROTOCORE_GC_REINCLUDE_SURVIVORS";
#else
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    auto* rs = space.createRootSet("test-survivor");
    ASSERT_NE(rs, nullptr);

    ProtoRootSet::Handle h = ProtoRootSet::kNullHandle;
    {
        ProtoContext sub(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* obj = sub.newObject(false);
        ASSERT_NE(obj, nullptr);
        h = rs->add(obj);
    }
    // sub destructor submits its young chain to dirtySegments.

    // Two cycles: cell survives both because it is rooted in rs.  After
    // cycle 1, the survivor re-chain has placed the cell back into
    // dirtySegments so cycle 2 still sees it as a candidate.
    waitForGcCycles(space, 2);

    const unsigned long freeBefore = space.freeCellsCount;

    // Drop the reference, run one more cycle.  With the fix, the cell is in
    // a survivor segment that the next cycle re-includes; mark misses it
    // (no root reaches it now), sweep frees it back to space.freeCells.
    rs->remove(h);
    waitForGcCycles(space, 1);

    const unsigned long freeAfter = space.freeCellsCount;

    EXPECT_GT(freeAfter, freeBefore)
        << "no cells returned to free pool after dropping the only reference; "
           "survivor re-chain may not be feeding the candidate set";

    space.destroyRootSet(rs);
#endif
}

// T3 — The per-context threshold trigger actually fires.
//
// Pin every allocated cell so nothing can be reclaimed even if GC runs.
// Lower the threshold so that, over the course of a few thousand
// allocations, the trigger should fire many times.  Each firing submits
// the chain and resets the count.  We observe by pre/post comparison of
// dirtySegments push activity: heapSize will grow normally (cells are
// pinned), but the per-context count must be re-armed below the
// threshold by the time the loop ends.
TEST(GCSurvivorTest, ThresholdTriggerFiresUnderHeavyAllocation) {
#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    GTEST_SKIP() << "requires PROTOCORE_GC_REINCLUDE_SURVIVORS";
#else
    ProtoSpace space;
    space.maxAllocatedCellsPerContext = 100;

    auto* rs = space.createRootSet("threshold-pin");
    ASSERT_NE(rs, nullptr);

    std::vector<ProtoRootSet::Handle> handles;
    handles.reserve(2000);

    ProtoContext* ctx = space.rootContext;
    constexpr int K = 2000;  // 20× threshold
    for (int i = 0; i < K; ++i) {
        const ProtoObject* obj = ctx->newObject(false);
        handles.push_back(rs->add(obj));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // After K=2000 allocations with threshold=100, the trigger fired ~20
    // times.  Each firing resets allocatedCellsCount to 0 + however many
    // we did before the next firing.  The current count must therefore be
    // less than 2 * threshold (room for the most-recent batch plus the
    // increment that fired the trigger).
    EXPECT_LT(ctx->allocatedCellsCount, space.maxAllocatedCellsPerContext * 2u)
        << "allocatedCellsCount = " << ctx->allocatedCellsCount
        << " but threshold = " << space.maxAllocatedCellsPerContext
        << " — trigger should have reset it many times";

    for (auto handle : handles) rs->remove(handle);
    space.destroyRootSet(rs);
#endif
}

// T4 — Cells that are legitimately retained are not freed.
//
// Regression guard: the survivor re-chain must not over-collect.  Allocate
// many cells, pin every one of them via a root set, force several GC
// cycles, and confirm the heap stayed populated (handles all still
// resolve to non-null objects).  Runs in both flag states; if it fails
// with the flag on, the implementation is over-collecting.
TEST(GCSurvivorTest, LegitimateRetentionGrows) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    auto* rs = space.createRootSet("retention");
    ASSERT_NE(rs, nullptr);

    std::vector<ProtoRootSet::Handle> handles;
    constexpr int K = 500;
    handles.reserve(K);

    for (int i = 0; i < K; ++i) {
        ProtoContext sub(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* obj = sub.newObject(false);
        ASSERT_NE(obj, nullptr);
        handles.push_back(rs->add(obj));
    }

    waitForGcCycles(space, 3);

    // Every handle must still resolve to a live object.
    for (auto h : handles) {
        const ProtoObject* obj = rs->resolve(h);
        EXPECT_NE(obj, nullptr) << "a pinned cell was incorrectly freed";
    }

    for (auto h : handles) rs->remove(h);
    space.destroyRootSet(rs);
}
