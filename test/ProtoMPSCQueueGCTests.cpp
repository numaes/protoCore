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
struct MarkRaceJob {
    const ProtoMPSCQueue* queue;
    long total;
    long turn;                       // pushes (or drains) per ProtoContext
    std::atomic<long> produced;
    std::atomic<long> consumed;
    std::atomic<long> corrupt;
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
            if (std::chrono::steady_clock::now() > deadline) break;
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
    MarkRaceJob job{q, 400000, 200, {0}, {0}, {0}};

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
