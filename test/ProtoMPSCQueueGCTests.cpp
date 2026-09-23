// ProtoMPSCQueueGCTests.cpp - PMQ-SPEC section 5 GC safety.
//
// Cycles are forced with a small hard heap limit (the pattern of
// ProtoMapGCTests.cpp / GCRootScopeTests.cpp).  Three properties:
//   1. an item whose only reference is the queue survives;
//   2. an item pushed WHILE the collector is marking survives, and so does
//      one that a takeAll detaches while the collector is marking;
//   3. a producer or consumer parked inside UnmanagedScope never delays a
//      pause.
//
// Worker threads are protoCore threads (ProtoSpace::newThread).  A raw
// std::thread is not counted in runningThreads, so it never parks at a
// stop-the-world; property 3 in particular would then be vacuous.
//
// Every worker uses one ProtoContext per turn.  A context owns its young
// generation until it is destroyed, and a cell in a young chain is not a
// candidate of the running cycle - so a long-lived context would keep every
// node and every item out of the collector's reach and these tests would
// prove nothing about tracing.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace proto;

namespace {

constexpr int kHeadroomCells = 40000;
constexpr int kGarbagePerBatch = 5000;

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

// Builds an item whose identity is verifiable after any number of cycles.
const ProtoObject* probe(ProtoContext* c, long i) {
    return c->newList()->appendLast(c, c->fromInteger(i))
                       ->appendLast(c, c->fromInteger(~i))->asObject(c);
}

bool probeIntact(ProtoContext* c, const ProtoObject* o, long i) {
    const ProtoList* l = o ? o->asList(c) : nullptr;
    return l && l->getSize(c) == 2 &&
           l->getAt(c, 0)->asLong(c) == i &&
           l->getAt(c, 1)->asLong(c) == ~i;
}

//--------------------------------------------------------------------------
// Shared state for the mark-race test.
//--------------------------------------------------------------------------
// Backpressure for the mark race, for the same reason as in
// ProtoMPSCQueueConcurrencyTests.cpp: a producer running flat out against one
// consumer fills any bounded heap by construction, and this test's heap is
// deliberately tiny (kHeadroomCells).  Without a bound the run ended on
// protoCore's OOM guard, which proved nothing about marking.
//
// The bound is smaller here than in the concurrency stress because the items
// are heavier.  Each `probe` is a two-element ProtoList, so an in-flight item
// costs its queue node plus the probe's own cells, and one takeAll of N items
// additionally allocates about N*log2(N) cells in the consumer's young
// generation while ProtoContext::newList builds the batch (measured: with a
// 412,144-cell ceiling and plain integer items, N = 20,000 completes and
// N = 30,000 does not; a no-queue control calling only newList hits the same
// wall at the same N).  Against kHeadroomCells = 40,000 that leaves room for
// a few hundred heavy items in flight.
constexpr long kMarkRaceMaxInFlight = 250;

struct MarkRaceJob {
    const ProtoMPSCQueue* queue;
    long total;
    long turn;                       // pushes (or drains) per ProtoContext
    std::atomic<long> produced;
    std::atomic<long> consumed;
    std::atomic<long> corrupt;
    // Set when the consumer stops, so a producer parked on backpressure can
    // never outlive it and hang the join.
    std::atomic<bool> abort;
};

const ProtoObject* markRaceProducer(ProtoContext* ctx,
                                    const ProtoObject*,
                                    const ParentLink*,
                                    const ProtoList* args,
                                    const ProtoSparseList*) {
    if (!args || args->getSize(ctx) < 1) return PROTO_NONE;
    auto* job = reinterpret_cast<MarkRaceJob*>(args->getAt(ctx, 0)->asLong(ctx));
    for (long i = 0; i < job->total; ) {
        ProtoContext t(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
        const long end = (i + job->turn < job->total) ? i + job->turn : job->total;
        for (; i < end; ++i) {
            if (job->produced.load(std::memory_order_relaxed) -
                    job->consumed.load(std::memory_order_relaxed) >= kMarkRaceMaxInFlight) {
                // Inside an UnmanagedScope: a producer waiting here in the
                // running set would delay every pause, and the pauses are
                // what this test is about.
                ProtoContext::UnmanagedScope parked(&t);
                while (job->produced.load(std::memory_order_relaxed) -
                           job->consumed.load(std::memory_order_relaxed) >=
                               kMarkRaceMaxInFlight &&
                       !job->abort.load(std::memory_order_relaxed))
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            if (job->abort.load(std::memory_order_relaxed)) return PROTO_NONE;
            job->queue->push(&t, probe(&t, i));
            job->produced.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return PROTO_NONE;
}

const ProtoObject* markRaceConsumer(ProtoContext* ctx,
                                    const ProtoObject*,
                                    const ParentLink*,
                                    const ProtoList* args,
                                    const ProtoSparseList*) {
    if (!args || args->getSize(ctx) < 1) return PROTO_NONE;
    auto* job = reinterpret_cast<MarkRaceJob*>(args->getAt(ctx, 0)->asLong(ctx));
    long next = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    while (next < job->total) {
        ProtoContext t(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* batch = job->queue->takeAll(&t);
        const unsigned long n = batch->getSize(&t);
        for (unsigned long i = 0; i < n; ++i) {
            const ProtoObject* o = batch->getAt(&t, static_cast<int>(i));
            if (!probeIntact(&t, o, next)) job->corrupt.fetch_add(1, std::memory_order_relaxed);
            ++next;
        }
        job->consumed.store(next, std::memory_order_relaxed);
        if (n == 0) {
            std::this_thread::yield();
            if (std::chrono::steady_clock::now() > deadline) {
                // Release the producer before leaving, or it waits on
                // backpressure that will never arrive and the join hangs.
                job->abort.store(true, std::memory_order_relaxed);
                break;
            }
        }
    }
    return PROTO_NONE;
}

//--------------------------------------------------------------------------
// Shared state for the UnmanagedScope test.
//--------------------------------------------------------------------------
struct ParkJob {
    std::atomic<int> parked;
    std::atomic<bool> release;
};

const ProtoObject* parkedWorker(ProtoContext* ctx,
                                const ProtoObject*,
                                const ParentLink*,
                                const ProtoList* args,
                                const ProtoSparseList*) {
    if (!args || args->getSize(ctx) < 1) return PROTO_NONE;
    auto* job = reinterpret_cast<ParkJob*>(args->getAt(ctx, 0)->asLong(ctx));
    ProtoContext::UnmanagedScope u(ctx);   // no ProtoObject* access in here
    job->parked.fetch_add(1, std::memory_order_relaxed);
    while (!job->release.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return PROTO_NONE;
}

}  // namespace

// 1. The queue is the ONLY reference to its items, and the context that
//    created them is gone.  Several cycles later every item is intact.
TEST(MPSCQueueGC, ItemsReferencedOnlyByTheQueueSurvive) {
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoRootSet* rs = space.createRootSet("mpsc-gc");
    ASSERT_NE(rs, nullptr);

    const ProtoMPSCQueue* q = live.newMPSCQueue();
    const ProtoRootSet::Handle pinned = rs->add(q->asObject(&live));
    ASSERT_NE(pinned, ProtoRootSet::kNullHandle);

    constexpr long kN = 2000;
    {
        ProtoContext producer(&space, &live, nullptr, nullptr, nullptr, nullptr);
        for (long i = 0; i < kN; ++i) q->push(&producer, probe(&producer, i));
    }   // the producer context is gone; only the queue holds the items

    const uint64_t cycles = forceCycles(space, &live, 3);
    EXPECT_GE(cycles, 3u) << "no collection ran; the test proves nothing";

    const ProtoList* got = q->takeAll(&live);
    ASSERT_EQ(got->getSize(&live), static_cast<unsigned long>(kN));
    for (long i = 0; i < kN; ++i)
        EXPECT_TRUE(probeIntact(&live, got->getAt(&live, static_cast<int>(i)), i)) << "item " << i;

    std::cout << "MPSC gc-only-reference: items=" << kN
              << " gc cycles=" << cycles << std::endl;
    rs->remove(pinned);
}

// 1b. The same, but the batch is taken BEFORE the cycles run and the
//     returned list is the only reference: this covers the other half of
//     the hand-off, where the items live in a young ProtoList while the
//     retain chain still holds their nodes.
TEST(MPSCQueueGC, ATakenBatchSurvivesLaterCycles) {
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoRootSet* rs = space.createRootSet("mpsc-gc-batch");
    ASSERT_NE(rs, nullptr);

    const ProtoMPSCQueue* q = live.newMPSCQueue();
    const ProtoRootSet::Handle pinnedQueue = rs->add(q->asObject(&live));

    constexpr long kN = 2000;
    {
        ProtoContext producer(&space, &live, nullptr, nullptr, nullptr, nullptr);
        for (long i = 0; i < kN; ++i) q->push(&producer, probe(&producer, i));
    }
    const ProtoList* got = q->takeAll(&live);
    ASSERT_EQ(got->getSize(&live), static_cast<unsigned long>(kN));
    const ProtoRootSet::Handle pinnedBatch = rs->add(got->asObject(&live));

    const uint64_t cycles = forceCycles(space, &live, 3);
    EXPECT_GE(cycles, 3u);

    for (long i = 0; i < kN; ++i)
        EXPECT_TRUE(probeIntact(&live, got->getAt(&live, static_cast<int>(i)), i)) << "item " << i;

    rs->remove(pinnedBatch);
    rs->remove(pinnedQueue);
}

// 2. Pushes and takeAlls that run WHILE the collector marks.  A producer
//    thread pushes continuously and a consumer thread drains continuously
//    while the main thread forces cycles; every item must come out intact
//    and exactly once, in order.
TEST(MPSCQueueGC, PushAndTakeAllDuringConcurrentMarking) {
    ProtoSpace space;
    ProtoContext main(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoRootSet* rs = space.createRootSet("mpsc-gc-mark");
    ASSERT_NE(rs, nullptr);

    const ProtoMPSCQueue* q = main.newMPSCQueue();
    const ProtoRootSet::Handle pinned = rs->add(q->asObject(&main));
    ASSERT_NE(pinned, ProtoRootSet::kNullHandle);

    // 400,000 messages rather than 200,000: the EXPECT_GE(cycles, 5) guard
    // below is what stops this test from passing vacuously, and at 200,000
    // the traffic was short enough that a slower process (the
    // PROTOCORE_GC_INSTRUMENT build, or this filter run after the 8 x 1M
    // stress in the same process) sometimes ended with only four cycles.
    // Longer traffic gives the guard margin; it does not weaken it.
    MarkRaceJob job{q, 400000, 200, {0}, {0}, {0}, {false}};

    const ProtoString* pname = ProtoString::createSymbol(&main, "mpsc-mark-producer");
    const ProtoString* cname = ProtoString::createSymbol(&main, "mpsc-mark-consumer");
    const ProtoObject* handle = main.fromLong(reinterpret_cast<long long>(&job));
    const ProtoThread* producer = space.newThread(
        &main, pname, &markRaceProducer, main.newList()->appendLast(&main, handle), nullptr);
    const ProtoThread* consumer = space.newThread(
        &main, cname, &markRaceConsumer, main.newList()->appendLast(&main, handle), nullptr);
    ASSERT_NE(producer, nullptr);
    ASSERT_NE(consumer, nullptr);

    // Force collections while both run.
    space.setHeapLimits(0, space.heapSize + kHeadroomCells);
    const uint64_t start = space.getGCCycleCount();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    while (job.consumed.load(std::memory_order_relaxed) < job.total &&
           std::chrono::steady_clock::now() < deadline) {
        ProtoContext garbage(&space, &main, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 2000; ++i) (void) garbage.newObject(false);
    }
    // Whether the loop above finished or hit its deadline, release anyone
    // parked on backpressure before joining.
    job.abort.store(true, std::memory_order_relaxed);
    const_cast<ProtoThread*>(producer)->join(&main);
    const_cast<ProtoThread*>(consumer)->join(&main);
    space.setHeapLimits(0, 0);
    rs->remove(pinned);

    const uint64_t cycles = space.getGCCycleCount() - start;
    std::cout << "MPSC mark-race: produced=" << job.produced.load()
              << " consumed=" << job.consumed.load()
              << " corrupt=" << job.corrupt.load()
              << " gc cycles=" << cycles << std::endl;

    EXPECT_GE(cycles, 5u) << "no collection overlapped the traffic";
    EXPECT_EQ(job.corrupt.load(), 0) << "an item was collected, recycled or reordered";
    EXPECT_EQ(job.consumed.load(), job.total);
}

// 3. A producer and a consumer parked inside UnmanagedScope must not delay
//    a pause: the collector completes cycles while they sit there.
TEST(MPSCQueueGC, ParkedProducerAndConsumerDoNotDelayAPause) {
    ProtoSpace space;
    ProtoContext main(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoMPSCQueue* q = main.newMPSCQueue();
    ProtoRootSet* rs = space.createRootSet("mpsc-unmanaged");
    const ProtoRootSet::Handle pinned = rs->add(q->asObject(&main));

    for (long i = 0; i < 100; ++i) q->push(&main, probe(&main, i));

    ParkJob job{{0}, {false}};
    const ProtoString* name = ProtoString::createSymbol(&main, "mpsc-parked");
    const ProtoObject* handle = main.fromLong(reinterpret_cast<long long>(&job));
    std::vector<const ProtoThread*> sleepers;
    for (int t = 0; t < 2; ++t) {
        const ProtoThread* th = space.newThread(
            &main, name, &parkedWorker, main.newList()->appendLast(&main, handle), nullptr);
        ASSERT_NE(th, nullptr);
        sleepers.push_back(th);
    }
    const auto parkDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (job.parked.load() < 2) {
        ASSERT_LT(std::chrono::steady_clock::now(), parkDeadline) << "workers never parked";
        std::this_thread::yield();
    }

    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t cycles = forceCycles(space, &main, 3);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    job.release.store(true);
    for (const ProtoThread* t : sleepers) const_cast<ProtoThread*>(t)->join(&main);

    EXPECT_GE(cycles, 3u) << "the parked threads blocked the collector";
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 30)
        << "collections took far longer with threads parked in UnmanagedScope";

    const ProtoList* got = q->takeAll(&main);
    EXPECT_EQ(got->getSize(&main), 100u);
    for (long i = 0; i < 100; ++i)
        EXPECT_TRUE(probeIntact(&main, got->getAt(&main, static_cast<int>(i)), i));

    std::cout << "MPSC unmanaged-park: gc cycles=" << cycles
              << " elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << std::endl;
    rs->remove(pinned);
}

// ---------------------------------------------------------------------------
// Pause — PMQ-SPEC section 3 constraint 1
// ---------------------------------------------------------------------------

// A consumer draining a large mailbox must not hold the world stopped.
//
// This is the measurement that condemned `takeAll` when the branch was first
// reviewed: `takeAll` builds its result with `ProtoContext::newList(n, items)`,
// and that builder used to wrap its whole O(n) construction in a
// `ProtoContext::CriticalSection`.  A thread inside a critical section never
// parks at the stop-the-world poll, so phase P1 could not complete until the
// batch was fully built — stop-the-world work proportional to queue length,
// which PMQ-SPEC section 3 constraint 1 forbids.
//
// What is measured is the pause itself: `stwFlag` is raised at the start of
// P1 and cleared at the end of P2, so the interval over which this thread
// observes the flag raised is P1 + P2.  The method, the bound relative to a
// measured reference drain, and the one-drain-per-sample discipline are those
// of `BulkListBuild.LargeBuildDoesNotBlockStopTheWorld`, so the two numbers
// are directly comparable.
//
// The bound is relative to the measured drain time, so the test is
// independent of machine speed and of the load on it.
//
// One probe measures one batch size; the test below runs two sizes a factor
// of four apart and compares them, because what constraint 1 forbids is not a
// large pause but a pause that is a function of the batch.
struct DrainPauseProbe {
    long batch{0};
    long long medianUs{0};
    long long minUs{0};
    long long maxUs{0};
    long long drainMs{0};
};

static void runDrainPauseProbe(long kBatch, DrainPauseProbe* out) {
    constexpr int kSamples = 9;

    using Clock = std::chrono::steady_clock;

    struct Shared {
        const ProtoMPSCQueue* queue{nullptr};
        long batch{0};
        std::atomic<bool> stop{false};
        std::atomic<bool> ready{false};
        std::atomic<int> requested{0};
        std::atomic<int> completed{0};
        std::atomic<bool> inDrain{false};
        std::atomic<unsigned> bad{0};
    };
    static Shared* shared = nullptr;
    Shared state;
    shared = &state;

    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    ProtoRootSet* rs = space.createRootSet("mpsc-pause");
    ASSERT_NE(rs, nullptr);
    const ProtoMPSCQueue* q = root->newMPSCQueue();
    const ProtoRootSet::Handle pinned = rs->add(q->asObject(root));
    ASSERT_NE(pinned, ProtoRootSet::kNullHandle);
    state.queue = q;

    // Reference: fill the mailbox and drain it once on this thread, with no
    // collector activity, to get the cost of one drain on this machine.
    long long drainMs = 0;
    {
        ProtoContext fill(&space, root, nullptr, nullptr, nullptr, nullptr);
        for (long i = 0; i < kBatch; ++i) q->push(&fill, fill.fromInteger(i));
        const auto t0 = Clock::now();
        const ProtoList* batch = q->takeAll(&fill);
        drainMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        ASSERT_EQ(batch->getSize(&fill), static_cast<unsigned long>(kBatch));
    }
    ASSERT_GT(drainMs, 20) << "the reference drain is too short to measure a pause against";

    // One fill-and-drain per request, then back to an unmanaged wait so the
    // collector can reclaim the batch before the next sample starts.
    state.batch = kBatch;
    auto consumerMain = [](ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                           const ProtoList*, const ProtoSparseList*) -> const ProtoObject* {
        const long kBatch = shared->batch;
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
                ProtoContext turn(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
                for (long i = 0; i < kBatch; ++i)
                    shared->queue->push(&turn, turn.fromInteger(i));
                shared->ready.store(true, std::memory_order_relaxed);
                shared->inDrain.store(true, std::memory_order_relaxed);
                const ProtoList* batch = shared->queue->takeAll(&turn);
                shared->inDrain.store(false, std::memory_order_relaxed);
                if (!batch || batch->getSize(&turn) != static_cast<unsigned long>(kBatch))
                    shared->bad.fetch_add(1, std::memory_order_relaxed);
            }
            shared->completed.fetch_add(1, std::memory_order_relaxed);
        }
        return PROTO_NONE;
    };

    const ProtoThread* worker = space.newThread(
        root, ProtoString::createSymbol(root, "mpsc-drainer"), consumerMain, nullptr, nullptr);
    ASSERT_NE(worker, nullptr);

    std::vector<long long> pausesUs;
    int sampledInDrain = 0;
    {
        ProtoContext::UnmanagedScope parked(root);
        for (int s = 0; s < kSamples; ++s) {
            const auto idleDeadline = Clock::now() + std::chrono::seconds(60);
            while (space.gcStarted.load() && Clock::now() < idleDeadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));

            state.requested.fetch_add(1, std::memory_order_relaxed);
            // Wait until the worker is inside takeAll, so the cycle is
            // requested against a drain that is really in flight.
            const auto armDeadline = Clock::now() + std::chrono::seconds(60);
            while (!state.inDrain.load(std::memory_order_relaxed) &&
                   state.completed.load(std::memory_order_relaxed) < s + 1 &&
                   Clock::now() < armDeadline)
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            if (state.inDrain.load(std::memory_order_relaxed)) ++sampledInDrain;

            {
                std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
                space.gcStarted = true;
                space.gcCV.notify_all();
            }
            const auto cap = std::chrono::milliseconds(std::max<long long>(2000, drainMs * 20));
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

            const auto finishDeadline =
                Clock::now() + std::chrono::milliseconds(std::max<long long>(5000, drainMs * 50));
            while (state.completed.load(std::memory_order_relaxed) < s + 1 &&
                   Clock::now() < finishDeadline) {
                {
                    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
                    space.gcStarted = true;
                    space.gcCV.notify_all();
                }
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
    rs->remove(pinned);

    ASSERT_EQ(pausesUs.size(), static_cast<size_t>(kSamples));
    std::vector<long long> sorted = pausesUs;
    std::sort(sorted.begin(), sorted.end());
    const long long medianUs = sorted[sorted.size() / 2];

    // Reported on every run, pass or fail: this is the number the merge of
    // this branch is justified by.
    std::printf("[ PAUSE    ] stop-the-world (P1+P2) while a consumer drains %ld items: "
                "median %lld us, min %lld us, max %lld us over %d samples; "
                "one drain = %lld ms\n",
                kBatch, medianUs, sorted.front(), sorted.back(), kSamples, drainMs);
    std::fflush(stdout);

    EXPECT_EQ(state.bad.load(), 0u) << "the consumer produced a malformed batch";
    EXPECT_EQ(state.completed.load(), kSamples) << "the consumer did not complete every drain";
    EXPECT_GE(sampledInDrain, kSamples - 1)
        << "the cycles were not observed against a drain in flight";

    // Sanity bound, kept from the first version of this test: whatever else
    // is true, the world must not stay stopped for a quarter of a drain.
    EXPECT_LT(medianUs, drainMs * 1000 / 4)
        << "the world stayed stopped for " << medianUs << " us (median of " << kSamples
        << ", max " << sorted.back() << ") while a consumer was draining " << kBatch
        << " items, against " << drainMs << " ms for one drain";

    out->batch = kBatch;
    out->medianUs = medianUs;
    out->minUs = sorted.front();
    out->maxUs = sorted.back();
    out->drainMs = drainMs;
}

// PMQ-SPEC section 3 constraint 1: no stop-the-world work proportional to
// queue length.  A single measurement cannot show that; two, a factor of four
// apart, can.  Before the poll was added to `takeAll`'s reversal loop this
// probe measured 338 us at 50,000 and 3,731 us at 400,000 on this machine -
// an 11x rise for an 8x batch, i.e. linear.  After it: 32 us and 27 us.
//
// The bound is deliberately generous (a full factor of four of growth plus a
// fixed 200 us of scheduler noise) because the quantity being falsified is
// the SHAPE of the curve, not its height.  Linear growth blows through it;
// noise on a loaded machine does not.
TEST(MPSCQueueGC, LargeDrainDoesNotBlockStopTheWorld) {
    // Overridable so the same probe can be swept over other sizes by hand.
    long kBatch = 200000;
    if (const char* e = std::getenv("PMQ_PAUSE_BATCH")) { long v = std::atol(e); if (v > 0) kBatch = v; }

    DrainPauseProbe small{};
    runDrainPauseProbe(kBatch / 4, &small);
    if (::testing::Test::HasFatalFailure()) return;

    DrainPauseProbe large{};
    runDrainPauseProbe(kBatch, &large);
    if (::testing::Test::HasFatalFailure()) return;

    const long long bound = small.medianUs * 4 + 200;
    std::printf("[ PAUSE    ] proportionality: %ld items -> %lld us, %ld items -> %lld us "
                "(bound %lld us)\n",
                small.batch, small.medianUs, large.batch, large.medianUs, bound);
    std::fflush(stdout);

    EXPECT_LT(large.medianUs, bound)
        << "the pause grows with the batch: " << small.medianUs << " us at " << small.batch
        << " items against " << large.medianUs << " us at " << large.batch
        << " items.  PMQ-SPEC section 3 constraint 1 forbids stop-the-world work "
           "proportional to queue length";
}
