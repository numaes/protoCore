// GCRootScopeTests.cpp — the stop-the-world phase collects roots only.
//
// protoCore stops user threads only to collect roots: each thread's stack
// roots, the root of the mutables tree and the roots of the global
// structures.  Everything reachable from those roots is immutable, so the
// traversal of references, the references of young cells included, runs in
// the concurrent mark.  These tests check both halves:
//
//   * no Cell::processReferences call happens while stwFlag is raised;
//   * objects reachable only through a young cell, through C++ locals of a
//     running context, or through the mutables tree survive collection
//     cycles, also while several threads allocate during the concurrent
//     mark.
//
// Cycles are forced with a small hard heap limit (ProtoSpace::setHeapLimits):
// once the garbage allocated by the test fills it, refills wait for cycles
// that reclaim it.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace proto;

namespace {

constexpr int kHeadroomCells = 40000;
constexpr int kGarbagePerBatch = 5000;

// Allocates garbage in short-lived children of `parent` under a hard heap
// limit just above the current heap until at least `minCycles` cycles have
// started (or a batch cap is reached), then removes the limit.  Returns the
// number of cycles started meanwhile.
uint64_t forceCycles(ProtoSpace& space, ProtoContext* parent, uint64_t minCycles) {
    const uint64_t start = space.getGCCycleCount();
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + kHeadroomCells);
    for (int batch = 0; batch < 400 && space.getGCCycleCount() - start < minCycles; ++batch) {
        ProtoContext garbage(&space, parent, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kGarbagePerBatch; ++i) (void) garbage.newObject(false);
    }
    space.setHeapLimits(0, 0);
    return space.getGCCycleCount() - start;
}

// Waits, parked in an unmanaged region, until no cycle is pending or
// running, so a cycle requested by the last refill can complete.
bool waitForIdleCollector(ProtoSpace& space, ProtoContext* ctx) {
    ProtoContext::UnmanagedScope parked(ctx);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (space.gcStarted.load()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// A test cell without references that records every traversal by the
// collector, and each traversal made while the world was stopped.
class StwProbeCell final : public Cell {
public:
    static std::atomic<unsigned long> traversals;
    static std::atomic<unsigned long> traversalsWhileStopped;

    explicit StwProbeCell(ProtoContext* context) : Cell(context) {}

    void processReferences(ProtoContext* context, void*,
                           void (*)(ProtoContext*, void*, const Cell*)) const override {
        traversals.fetch_add(1, std::memory_order_relaxed);
        if (context->space->stwFlag.load()) {
            traversalsWhileStopped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const ProtoObject* implAsObject(ProtoContext*) const override { return PROTO_NONE; }
};

std::atomic<unsigned long> StwProbeCell::traversals{0};
std::atomic<unsigned long> StwProbeCell::traversalsWhileStopped{0};

}  // namespace

// Stop-the-world collects roots only.  A probe cell held only by a live
// context's young chain must be traversed by the collector, and never while
// the world is stopped.
TEST(GCRootScope, YoungChainIsNotTraversedWhileTheWorldIsStopped) {
    StwProbeCell::traversals = 0;
    StwProbeCell::traversalsWhileStopped = 0;

    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    (void) new (&live) StwProbeCell(&live);

    const uint64_t cycles = forceCycles(space, &live, 8);
    ASSERT_TRUE(waitForIdleCollector(space, &live));

    EXPECT_GE(cycles, 8u);
    EXPECT_GT(StwProbeCell::traversals.load(), 0u)
        << "the collector never traversed the young probe cell";
    EXPECT_EQ(StwProbeCell::traversalsWhileStopped.load(), 0u)
        << "the young chain was traversed while the world was stopped";
}

// Stop-the-world collects roots only, for the survivor pen as well.  A probe
// cell kept alive by a root set survives its first cycle into the pen; with a
// stagger of 2, every other cycle leaves the pen unfolded and scans it.  No
// traversal of the probe may happen while the world is stopped.
TEST(GCRootScope, SurvivorPenIsNotTraversedWhileTheWorldIsStopped) {
#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    GTEST_SKIP() << "the survivor pen exists only with PROTOCORE_GC_REINCLUDE_SURVIVORS";
#else
    StwProbeCell::traversals = 0;
    StwProbeCell::traversalsWhileStopped = 0;

    ProtoSpace space;
    space.survivorStagger = 2;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    ProtoRootSet* rs = space.createRootSet("gc-root-scope-pen");
    ASSERT_NE(rs, nullptr);
    ProtoRootSet::Handle pinned = ProtoRootSet::kNullHandle;
    {
        ProtoContext sub(&space, &live, nullptr, nullptr, nullptr, nullptr);
        const Cell* probe = new (&sub) StwProbeCell(&sub);
        pinned = rs->add(reinterpret_cast<const ProtoObject*>(probe));
    }  // the probe is now a candidate that the root set keeps alive

    const uint64_t cycles = forceCycles(space, &live, 8);
    ASSERT_TRUE(waitForIdleCollector(space, &live));

    EXPECT_GE(cycles, 8u);
    EXPECT_GT(StwProbeCell::traversals.load(), 0u)
        << "the collector never traversed the pinned probe cell";
    EXPECT_EQ(StwProbeCell::traversalsWhileStopped.load(), 0u)
        << "the survivor pen was traversed while the world was stopped";

    rs->remove(pinned);
    space.destroyRootSet(rs);
#endif
}

// An old object whose only reference is held by a young cell of a live
// context survives, and so does the young cell, which is held only by a C++
// local and the context's young chain.
TEST(GCRootScope, ObjectReachableOnlyThroughAYoungCellSurvives) {
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    const ProtoList* old = nullptr;
    {
        ProtoContext sub(&space, &live, nullptr, nullptr, nullptr, nullptr);
        old = sub.newList()->appendLast(&sub, sub.fromInteger(4242));
    }  // `old` is now a sweep candidate
    const ProtoList* young = live.newList()->appendLast(&live, old->asObject(&live));

    const uint64_t cycles = forceCycles(space, &live, 5);
    ASSERT_TRUE(waitForIdleCollector(space, &live));
    EXPECT_GE(cycles, 5u);

    ASSERT_EQ(young->getSize(&live), 1u);
    const ProtoObject* item = young->getAt(&live, 0);
    ASSERT_EQ(item, old->asObject(&live));
    const ProtoList* again = item->asList(&live);
    ASSERT_NE(again, nullptr);
    ASSERT_EQ(again->getSize(&live), 1u);
    EXPECT_EQ(again->getAt(&live, 0)->asLong(&live), 4242);
}

// An object referenced only by a mutable object's attribute lives only in
// the mutables tree; the mutable object itself is pinned by a root set.
TEST(GCRootScope, ObjectReachableOnlyThroughTheMutablesTreeSurvives) {
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoString* key = ProtoString::createSymbol(&live, "gcRootScopeValue");

    ProtoRootSet* rs = space.createRootSet("gc-root-scope-mutable");
    ASSERT_NE(rs, nullptr);
    ProtoRootSet::Handle holderHandle = ProtoRootSet::kNullHandle;
    {
        ProtoContext sub(&space, &live, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* holder = sub.newObject(true);
        const ProtoList* value = sub.newList()->appendLast(&sub, sub.fromInteger(777));
        holder->setAttribute(&sub, key, value->asObject(&sub));
        holderHandle = rs->add(holder);
    }

    const uint64_t cycles = forceCycles(space, &live, 5);
    ASSERT_TRUE(waitForIdleCollector(space, &live));
    EXPECT_GE(cycles, 5u);

    const ProtoObject* holder = rs->resolve(holderHandle);
    ASSERT_NE(holder, nullptr);
    const ProtoObject* value = holder->getAttribute(&live, key);
    ASSERT_NE(value, nullptr);
    const ProtoList* list = value->asList(&live);
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->getSize(&live), 1u);
    EXPECT_EQ(list->getAt(&live, 0)->asLong(&live), 777);

    rs->remove(holderHandle);
    space.destroyRootSet(rs);
}

namespace {

constexpr int kThreadIterations = 1500;
std::atomic<unsigned long> gThreadChecks{0};
std::atomic<unsigned long> gThreadErrors{0};
std::atomic<int> gThreadsDone{0};
std::atomic<bool> gThreadsRelease{false};

// Each iteration makes an old list (a candidate once its context is gone),
// references it from a young list held only in C++ locals and the thread's
// young chain, allocates garbage while cycles run, and checks both lists.
// The thread parks through synchToGC(), which never submits the young
// generation.  When its work is done it waits, parked, for the release, so
// no thread exits while a cycle may be requested.
const ProtoObject* allocatingThreadMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                        const ProtoList*, const ProtoSparseList*) {
    ProtoThread* self = const_cast<ProtoThread*>(ctx->thread);
    for (long iteration = 0; iteration < kThreadIterations; ++iteration) {
        const ProtoList* old = nullptr;
        {
            ProtoContext sub(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
            old = sub.newList()->appendLast(&sub, sub.fromInteger(iteration));
        }
        const ProtoList* young = ctx->newList()->appendLast(ctx, old->asObject(ctx));
        {
            ProtoContext garbage(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
            for (int i = 0; i < 256; ++i) {
                (void) garbage.newObject(false);
                if ((i & 63) == 0) self->synchToGC();
            }
        }
        const ProtoObject* item = young->getSize(ctx) == 1 ? young->getAt(ctx, 0) : nullptr;
        const ProtoList* list = item ? item->asList(ctx) : nullptr;
        if (item != old->asObject(ctx) || !list || list->getSize(ctx) != 1 ||
            list->getAt(ctx, 0)->asLong(ctx) != iteration) {
            gThreadErrors.fetch_add(1, std::memory_order_relaxed);
        }
        gThreadChecks.fetch_add(1, std::memory_order_relaxed);
    }
    gThreadsDone.fetch_add(1);
    ProtoContext::UnmanagedScope parked(ctx);
    while (!gThreadsRelease.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return PROTO_NONE;
}

}  // namespace

// Several ProtoThreads allocate, build young cells over old candidates and
// verify them while cycles run and the concurrent mark traverses their young
// chains.
TEST(GCRootScope, AllocationDuringConcurrentMarkIsSafe) {
    constexpr int kThreads = 4;
    gThreadChecks = 0;
    gThreadErrors = 0;
    gThreadsDone = 0;
    gThreadsRelease = false;

    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    const uint64_t cyclesStart = space.getGCCycleCount();
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + 5 * kHeadroomCells);

    std::vector<const ProtoThread*> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.push_back(space.newThread(root, ProtoString::createSymbol(root, "gc-root-scope-worker"),
                                          allocatingThreadMain, nullptr, nullptr));
    }
    {
        ProtoContext::UnmanagedScope parked(root);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (gThreadsDone.load() < kThreads && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const uint64_t cycles = space.getGCCycleCount() - cyclesStart;
    space.setHeapLimits(0, 0);
    ASSERT_TRUE(waitForIdleCollector(space, root));
    const int done = gThreadsDone.load();

    gThreadsRelease = true;
    {
        ProtoContext::UnmanagedScope parked(root);
        for (const ProtoThread* thread : threads) const_cast<ProtoThread*>(thread)->join(root);
    }

    ASSERT_EQ(done, kThreads) << "worker threads did not finish";
    EXPECT_EQ(gThreadChecks.load(), static_cast<unsigned long>(kThreads) * kThreadIterations);
    EXPECT_EQ(gThreadErrors.load(), 0u);
    EXPECT_GE(cycles, 3u) << "too few collection cycles ran for the test to exercise the mark";
}
