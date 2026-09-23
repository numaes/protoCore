// BulkListBuildTests.cpp — ProtoContext::newList(n, items), the bulk list
// builder, against the collector.
//
// `newList(n, items)` produces the AVL form by repeated appendLast over an
// empty AVL list.  The running intermediate is a fresh tree that no caller
// holds yet, so the builder must answer two independent questions:
//
//   * CORRECTNESS — while the build is in flight, is every intermediate
//     reachable from a real GC root, so that a collection landing in the
//     middle of the build cannot sweep the spine the loop is standing on?
//     `LargeBuildSurvivesForcedCollections` and
//     `ConcurrentBuildersSurviveForcedCollections` answer this: they force
//     cycles continuously while large lists are built and verify every
//     element of every result.
//
//   * PAUSE — does the build make the building thread unparkable?  The
//     collector stops the world cooperatively: it raises `stwFlag` and waits
//     until every running thread parks.  A thread that holds a
//     ProtoContext::CriticalSection never parks, so wrapping an O(n) build in
//     one makes the stop-the-world phase (P1) wait for the whole build.
//     `LargeBuildDoesNotBlockStopTheWorld` measures that directly: it times
//     how long a requested cycle takes to complete while a worker thread
//     builds large lists, and compares it with the duration of one build.
//     The bound is expressed as a fraction of the measured build time, so it
//     does not depend on the speed of the machine or on how loaded it is.
//
// The pause test FAILS when the builder holds a critical section across the
// build, and passes when the running intermediate is anchored in a GC root
// the collector scans (ProtoContext::pendingRoot) instead.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using namespace proto;

namespace {

using Clock = std::chrono::steady_clock;

long long millisSince(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// Requests a collection cycle.  triggerGC() is gated on heap pressure, so a
// test that wants a cycle on demand sets `gcStarted` directly — the same way
// GCMarkTests and GCSurvivorRechainTests do.
void requestCycle(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    space.gcStarted = true;
    space.gcCV.notify_all();
}

// A plain OS thread (not a ProtoThread, so it is not part of the
// stop-the-world quorum) that keeps asking for cycles until it is stopped.
class CycleKicker {
public:
    explicit CycleKicker(ProtoSpace& space, std::chrono::microseconds period)
        : thread_([this, &space, period]() {
              while (!stop_.load(std::memory_order_relaxed)) {
                  requestCycle(space);
                  std::this_thread::sleep_for(period);
              }
          }) {}
    ~CycleKicker() {
        stop_.store(true, std::memory_order_relaxed);
        thread_.join();
    }

private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// Elements of the lists under test.  Every second element is a heap object
// carrying its index as an attribute, so the test also detects an element
// that was swept out from under the spine, not only a broken spine.
constexpr unsigned kElementsPerBuild = 10000;

const ProtoString* indexKey(ProtoContext* ctx) {
    return ProtoString::createSymbol(ctx, "idx");
}

void fillItems(ProtoContext* ctx, std::vector<const ProtoObject*>& items, unsigned n) {
    const ProtoString* key = indexKey(ctx);
    items.clear();
    items.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        if ((i & 1u) == 0u) {
            items.push_back(ctx->fromInteger(i));
        } else {
            const ProtoObject* obj = ctx->newObject(false);
            items.push_back(obj->setAttribute(ctx, key, ctx->fromInteger(i)));
        }
    }
}

// Returns the number of elements that did not read back as they were stored.
unsigned verifyList(ProtoContext* ctx, const ProtoList* list, unsigned n) {
    if (!list || list->getSize(ctx) != n) return n;
    const ProtoString* key = indexKey(ctx);
    unsigned bad = 0;
    for (unsigned i = 0; i < n; ++i) {
        const ProtoObject* element = list->getAt(ctx, static_cast<int>(i));
        if (!element) { ++bad; continue; }
        if ((i & 1u) == 0u) {
            if (!element->isInteger(ctx) || element->asLong(ctx) != static_cast<long long>(i)) ++bad;
        } else {
            const ProtoObject* stored = element->getAttribute(ctx, key);
            if (!stored || !stored->isInteger(ctx) ||
                stored->asLong(ctx) != static_cast<long long>(i)) ++bad;
        }
    }
    return bad;
}

}  // namespace

// ---------------------------------------------------------------------------
// Correctness
// ---------------------------------------------------------------------------

// One thread builds large lists back to back while cycles are forced
// continuously under a hard heap limit, so mark and sweep really run over the
// heap the builder is allocating from.  Every element of every result is read
// back.
//
// The heap limit leaves room for one whole build: a context's young
// generation is submitted to the collector only when the context is destroyed
// or at a safepoint the embedder calls, so the cells of a build in flight can
// never be reclaimed and the ceiling must stand above them.
TEST(BulkListBuild, LargeBuildSurvivesForcedCollections) {
    constexpr int kRepeats = 40;
    constexpr int kHeadroomCells = 400000;  // one build allocates ~154,000

    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + kHeadroomCells);

    const uint64_t cyclesStart = space.getGCCycleCount();
    unsigned badElements = 0;
    uint64_t cyclesDuringBuild = 0;
    {
        CycleKicker kicker(space, std::chrono::microseconds(200));
        std::vector<const ProtoObject*> items;
        for (int rep = 0; rep < kRepeats; ++rep) {
            ProtoContext build(&space, root, nullptr, nullptr, nullptr, nullptr);
            fillItems(&build, items, kElementsPerBuild);
            // Cycles that complete strictly between these two reads completed
            // while the build was in flight.
            const uint64_t before = space.getGCCycleCount();
            const ProtoList* list = build.newList(kElementsPerBuild, items.data());
            cyclesDuringBuild += space.getGCCycleCount() - before;
            badElements += verifyList(&build, list, kElementsPerBuild);
        }
    }
    space.setHeapLimits(0, 0);

    EXPECT_EQ(badElements, 0u)
        << "elements of a list built by newList(n, items) did not survive the "
           "collections that ran during the build";
    EXPECT_GE(space.getGCCycleCount() - cyclesStart, 1u)
        << "no collection cycle completed — the test did not exercise the GC";
    // Without this the element check above proves nothing: a builder that the
    // collector cannot interrupt is trivially safe from it.
    EXPECT_GE(cyclesDuringBuild, 1u)
        << "no cycle completed while a build was in flight over " << kRepeats
        << " builds — the builder is not interruptible, so the survival check "
           "above never exercised a collection mid-build";
}

// The same build, run by several ProtoThreads at once, so a cycle stops the
// world while more than one builder is mid-build.
TEST(BulkListBuild, ConcurrentBuildersSurviveForcedCollections) {
    constexpr int kThreads = 3;
    constexpr int kRepeatsPerThread = 15;

    struct Shared {
        std::atomic<int> done{0};
        std::atomic<unsigned> bad{0};
        std::atomic<bool> release{false};
    };
    static Shared* shared = nullptr;
    Shared state;
    shared = &state;

    ProtoSpace space;
    ProtoContext* root = space.rootContext;

    auto builderMain = [](ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                          const ProtoList*, const ProtoSparseList*) -> const ProtoObject* {
        std::vector<const ProtoObject*> items;
        for (int rep = 0; rep < kRepeatsPerThread; ++rep) {
            ProtoContext build(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
            fillItems(&build, items, kElementsPerBuild);
            const ProtoList* list = build.newList(kElementsPerBuild, items.data());
            shared->bad.fetch_add(verifyList(&build, list, kElementsPerBuild),
                                  std::memory_order_relaxed);
        }
        shared->done.fetch_add(1, std::memory_order_relaxed);
        ProtoContext::UnmanagedScope parked(ctx);
        while (!shared->release.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return PROTO_NONE;
    };

    std::vector<const ProtoThread*> handles;
    {
        CycleKicker kicker(space, std::chrono::microseconds(200));
        for (int t = 0; t < kThreads; ++t) {
            handles.push_back(space.newThread(
                root, ProtoString::createSymbol(root, "list-builder"),
                builderMain, nullptr, nullptr));
        }
        {
            ProtoContext::UnmanagedScope parked(root);
            const auto deadline = Clock::now() + std::chrono::seconds(300);
            while (state.done.load() < kThreads && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        state.release.store(true, std::memory_order_relaxed);
        ProtoContext::UnmanagedScope parked(root);
        for (const ProtoThread* h : handles) const_cast<ProtoThread*>(h)->join(root);
    }
    shared = nullptr;

    EXPECT_EQ(state.done.load(), kThreads) << "a builder thread did not finish";
    EXPECT_EQ(state.bad.load(), 0u)
        << "elements of a concurrently built list did not survive collection";
}

// The mechanism the builder's anchor relies on: a value named by
// `ProtoContext::pendingRoot` stays reachable across collections even once the
// context's young generation has been handed to the collector, so it no longer
// covers the cells the value is built from.
//
// This is what `newList` buys by parking every intermediate in that slot.  It
// is asserted separately because the builder's own tests cannot discriminate:
// while a build is in flight the context's young chain has not been submitted,
// and it keeps the same cells reachable on its own.  The anchor is what makes
// the guarantee independent of that.
TEST(BulkListBuild, PendingRootKeepsASubmittedValueReachable) {
    constexpr int kRepeats = 30;

    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    // A low threshold makes every safepoint() hand this context's young chain
    // over, so the anchored value's cells become sweep candidates.
    space.maxAllocatedCellsPerContext = 500;

    ProtoContext ctx(&space, root, nullptr, nullptr, nullptr, nullptr);
    std::vector<const ProtoObject*> items;
    fillItems(&ctx, items, 200);
    const ProtoList* anchored = ctx.newList(200, items.data());
    ASSERT_EQ(anchored->getSize(&ctx), 200u);

    Cell* const saved = ctx.pendingRoot;
    ctx.pendingRoot = const_cast<Cell*>(
        ProtoObject::asCellPointer(reinterpret_cast<const ProtoObject*>(anchored)));
    {
        CycleKicker kicker(space, std::chrono::microseconds(200));
        for (int rep = 0; rep < kRepeats; ++rep) {
            // Churn in a child context and submit this context's young chain,
            // so the anchored list is held by nothing but pendingRoot.
            {
                ProtoContext garbage(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
                for (int i = 0; i < 2000; ++i) (void) garbage.newObject(false);
            }
            ctx.safepoint();
        }
    }
    const unsigned bad = verifyList(&ctx, anchored, 200);
    ctx.pendingRoot = saved;

    EXPECT_EQ(bad, 0u)
        << "a value named by ProtoContext::pendingRoot did not survive the "
           "collections that ran while it was anchored there";
}

// ---------------------------------------------------------------------------
// Pause
// ---------------------------------------------------------------------------

// A thread building a large list must not hold the world stopped.
//
// What is measured is the stop-the-world pause itself, not the duration of a
// cycle: `stwFlag` is raised at the start of phase P1 and cleared at the end
// of phase P2, where the world resumes (see the comment at that store in
// core/ProtoSpace.cpp), so the interval over which a test observes the flag
// raised is P1 + P2 — the pause.  Cycle completion would be the wrong metric,
// because it also carries the concurrent mark and the sweep, which are
// proportional to the heap and run with the world running.
//
// The test first times one build on the main thread (`buildMs`).  A worker
// ProtoThread then performs one build per sample, on request; the main
// thread, parked in an unmanaged region so it is not itself part of the
// quorum, waits until the build is under way, asks for a cycle and times the
// flag.
//
// With the build inside a critical section the worker cannot park, so the
// collector sits in P1 until the current build finishes and the pause lands
// at a large fraction of `buildMs`.  With the running intermediate anchored
// in a GC root instead, the worker parks between elements and the pause is a
// small fraction of `buildMs`.
//
// The bound is relative to the measured build time, so the test is
// independent of machine speed and of the load on it.  One build at a time,
// with the worker parked between samples, keeps the live heap bounded: a
// builder the collector cannot interrupt also stops it from reclaiming, and
// a free-running loop of 100,000-element builds exhausts the machine.
TEST(BulkListBuild, LargeBuildDoesNotBlockStopTheWorld) {
    constexpr unsigned kBuildSize = 100000;
    constexpr int kSamples = 9;

    struct Shared {
        std::atomic<bool> stop{false};
        std::atomic<bool> ready{false};
        std::atomic<int> requested{0};
        std::atomic<int> completed{0};
        std::atomic<bool> inBuild{false};
        std::atomic<unsigned> bad{0};
    };
    static Shared* shared = nullptr;
    Shared state;
    shared = &state;

    ProtoSpace space;
    ProtoContext* root = space.rootContext;

    // Reference: one build, on this thread, with no collector activity.
    long long buildMs = 0;
    {
        ProtoContext build(&space, root, nullptr, nullptr, nullptr, nullptr);
        std::vector<const ProtoObject*> items;
        fillItems(&build, items, kBuildSize);
        const auto t0 = Clock::now();
        const ProtoList* list = build.newList(kBuildSize, items.data());
        buildMs = millisSince(t0);
        ASSERT_EQ(list->getSize(&build), kBuildSize);
    }
    ASSERT_GT(buildMs, 20) << "the reference build is too short to measure a pause against";

    // One build per request, then back to an unmanaged wait so the collector
    // can reclaim what the build produced before the next one starts.
    auto builderMain = [](ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                          const ProtoList*, const ProtoSparseList*) -> const ProtoObject* {
        std::vector<const ProtoObject*> items;
        shared->ready.store(true, std::memory_order_relaxed);
        for (;;) {
            {
                ProtoContext::UnmanagedScope parked(ctx);
                while (!shared->stop.load(std::memory_order_relaxed) &&
                       shared->requested.load(std::memory_order_relaxed) ==
                           shared->completed.load(std::memory_order_relaxed))
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            if (shared->stop.load(std::memory_order_relaxed)) break;
            {
                ProtoContext build(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
                fillItems(&build, items, kBuildSize);
                shared->inBuild.store(true, std::memory_order_relaxed);
                const ProtoList* list = build.newList(kBuildSize, items.data());
                shared->inBuild.store(false, std::memory_order_relaxed);
                if (!list || list->getSize(&build) != kBuildSize)
                    shared->bad.fetch_add(1, std::memory_order_relaxed);
            }
            shared->completed.fetch_add(1, std::memory_order_relaxed);
        }
        return PROTO_NONE;
    };

    const ProtoThread* worker = space.newThread(
        root, ProtoString::createSymbol(root, "list-builder"), builderMain, nullptr, nullptr);

    std::vector<long long> pausesUs;
    int sampledInBuild = 0;
    {
        ProtoContext::UnmanagedScope parked(root);
        const auto startDeadline = Clock::now() + std::chrono::seconds(60);
        while (!state.ready.load(std::memory_order_relaxed) && Clock::now() < startDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        for (int s = 0; s < kSamples; ++s) {
            // Start from an idle collector, so the sample times this cycle's
            // pause and not the tail of the previous cycle's mark and sweep.
            const auto idleDeadline = Clock::now() + std::chrono::seconds(60);
            while (space.gcStarted.load() && Clock::now() < idleDeadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));

            state.requested.fetch_add(1, std::memory_order_relaxed);
            // Wait until the worker is inside newList, so the cycle is
            // requested against a build that is really in flight.
            const auto armDeadline = Clock::now() + std::chrono::seconds(60);
            while (!state.inBuild.load(std::memory_order_relaxed) &&
                   state.completed.load(std::memory_order_relaxed) < s + 1 &&
                   Clock::now() < armDeadline)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (state.inBuild.load(std::memory_order_relaxed)) ++sampledInBuild;

            requestCycle(space);
            // Cap a sample at twenty builds.  Far above the regime under
            // test either way, and it keeps a lost wake-up from turning a
            // failing assertion into a hung suite.
            const auto cap = std::chrono::milliseconds(std::max<long long>(2000, buildMs * 20));
            // Spin, not sleep: after the fix the pause is well under a
            // millisecond and a sleeping poll would miss the whole window.
            const auto flagUpDeadline = Clock::now() + cap;
            while (!space.stwFlag.load() && Clock::now() < flagUpDeadline)
                std::this_thread::yield();
            const auto t0 = Clock::now();
            const auto flagDownDeadline = t0 + cap;
            while (space.stwFlag.load() && Clock::now() < flagDownDeadline)
                std::this_thread::yield();
            pausesUs.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());

            // Let the build finish before the next sample.
            const auto finishDeadline =
                Clock::now() + std::chrono::milliseconds(std::max<long long>(5000, buildMs * 50));
            while (state.completed.load(std::memory_order_relaxed) < s + 1 &&
                   Clock::now() < finishDeadline) {
                requestCycle(space);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        state.stop.store(true, std::memory_order_relaxed);
    }
    {
        ProtoContext::UnmanagedScope parked(root);
        const_cast<ProtoThread*>(worker)->join(root);
    }
    shared = nullptr;

    ASSERT_EQ(pausesUs.size(), static_cast<size_t>(kSamples));
    std::vector<long long> sorted = pausesUs;
    std::sort(sorted.begin(), sorted.end());
    const long long medianUs = sorted[sorted.size() / 2];
    const long long maxUs = sorted.back();

    // Reported on every run, pass or fail: this is the number the change is
    // justified by, and it is worth having in the log of a passing suite too.
    std::printf("[ PAUSE    ] stop-the-world (P1+P2) while a thread builds %u elements: "
                "median %lld us, min %lld us, max %lld us over %d samples; "
                "one build = %lld ms\n",
                kBuildSize, medianUs, sorted.front(), maxUs, kSamples, buildMs);
    std::fflush(stdout);

    EXPECT_EQ(state.bad.load(), 0u) << "the worker produced a malformed list";
    EXPECT_EQ(state.completed.load(), kSamples) << "the worker did not complete every build";
    EXPECT_GE(sampledInBuild, kSamples - 1)
        << "the cycles were not observed against a build in flight";

    // The pause must not be a function of the length of the list being built.
    // A quarter of one build is far above the cost of collecting the roots of
    // this heap and far below the "waits for the build" regime, which sits
    // near buildMs.
    EXPECT_LT(medianUs, buildMs * 1000 / 4)
        << "the world stayed stopped for " << medianUs << " us (median of "
        << kSamples << ", max " << maxUs
        << ") while a thread was building a " << kBuildSize
        << "-element list, against " << buildMs
        << " ms for one build: the builder is holding the stop-the-world phase open";
}
