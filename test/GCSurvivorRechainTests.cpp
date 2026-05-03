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
// A long-running context allocates many cells, each immediately
// discarded, and calls ctx->safepoint() periodically.  Every safepoint
// at which the per-context counter has crossed the threshold submits
// the young chain to dirtySegments and resets the counter, so the
// counter stays bounded by ~2 × threshold instead of growing with
// the loop iteration count (which is what a non-fixed runtime
// previously did — `lastAllocatedCell` accumulated everything until
// the context was destroyed).
//
// Submission happens at safepoint() rather than allocCell(): native
// helpers that hold ProtoObject* in C++ locals across allocations
// (mutable setAttribute, SparseList rebuild, …) would lose those
// values to a sweep if the chain were submitted from inside their
// allocation calls.  See ProtoContext::safepoint() for the rationale.
TEST(GCSurvivorTest, RssBoundedInTightLoopNoEscapes) {
#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    GTEST_SKIP() << "requires PROTOCORE_GC_REINCLUDE_SURVIVORS";
#else
    ProtoSpace space;
    // Lower the threshold so the trigger fires many times in this test.
    space.maxAllocatedCellsPerContext = 100;

    ProtoContext* ctx = space.rootContext;

    // K is large enough that, without periodic safepoints, the counter
    // would be K + setup; with safepoints, the threshold submission
    // resets it on every crossing so it stays bounded by ~threshold.
    constexpr int K = 5000;
    for (int i = 0; i < K; ++i) {
        // Returned object is not stored; should be reclaimable on next GC.
        (void)ctx->newObject(false);
        // Mimic an embedder safepoint every 64 allocations — the same
        // cadence protoPython's bytecode loop uses.
        if ((i & 0x3F) == 0) ctx->safepoint();
    }
    ctx->safepoint();  // final flush

    // Without the threshold submission: count = K + setup, far above
    // threshold.  With it: count is below 2 * threshold (the most-recent
    // batch plus the increment that crossed the threshold but had not
    // yet hit a safepoint).
    EXPECT_LT(ctx->allocatedCellsCount, space.maxAllocatedCellsPerContext * 2u)
        << "allocatedCellsCount = " << ctx->allocatedCellsCount
        << " after " << K << " allocations with threshold "
        << space.maxAllocatedCellsPerContext
        << " — threshold submission not firing at safepoint().";
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

// T3 — Threshold submission fires even when every allocated cell is
// pinned.
//
// Companion to T1, isolating the threshold-submission mechanism from
// the survivor re-chain that frees unreached cells.  Every cell is
// pinned in a root set, so a sweep cannot reclaim anything, but the
// counter must still reset on every safepoint at which the threshold
// has been crossed — otherwise a long-running pinned-allocation
// workload would burn a triggerGC cycle on every allocCell instead of
// once per threshold-many.
TEST(GCSurvivorTest, ThresholdSubmissionFiresUnderHeavyAllocation) {
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
        if ((i & 0x3F) == 0) ctx->safepoint();
    }
    ctx->safepoint();

    EXPECT_LT(ctx->allocatedCellsCount, space.maxAllocatedCellsPerContext * 2u)
        << "allocatedCellsCount = " << ctx->allocatedCellsCount
        << " but threshold = " << space.maxAllocatedCellsPerContext
        << " — safepoint() should reset the counter once per threshold-many "
           "allocations.";

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
