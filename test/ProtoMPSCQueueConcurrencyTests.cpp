// ProtoMPSCQueueConcurrencyTests.cpp - PMQ-SPEC section 5: no loss, no
// duplication, per-producer order, with 8 producers against one consumer.
//
// The heap is bounded with setHeapLimits so collections actually run during
// the stress (8 x 1M nodes would otherwise be ~512 MB of live cells, and
// this machine is OOM-sensitive).  That makes this test double as the
// "pushes during concurrent marking" stress of section 5.
//
// The producers are protoCore threads (ProtoSpace::newThread), not raw
// std::threads: an unregistered thread is not counted in runningThreads, so
// it never parks at a stop-the-world and the pause completes while it runs.
// That would make this test weaker, not stronger (decision D10).

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using namespace proto;

namespace {

constexpr long kProducers = 8;
constexpr long kStride    = 10000000;   // room for any per-producer count

long pushesPerProducer() {
    if (const char* env = std::getenv("PMQ_STRESS_PUSHES")) {
        const long v = std::atol(env);
        if (v > 0) return v;
    }
    return 1000000;
}

// Backpressure.  PMQ-SPEC section 2 puts no bound on the queue itself, so
// the bound belongs to the workload - as it does in every real actor system.
// Without one this stops being a queue test: eight producers running flat out
// against one consumer fill any bounded heap by construction, and the run ends
// on protoCore's OOM guard with a backlog larger than the whole heap.  The
// first version of this test had no bound and did exactly that (the consumer
// froze at 5,732 items while the producers reached 54,600; at the abort every
// producer was parked in ProtoSpace::waitForHeapHeadroom inside push, and the
// consumer in the same wait inside takeAll, unable to allocate the list whose
// completion was the only thing that could have released the backlog).
//
// SIZING, and why it is not a magic number.  One takeAll of N items
// transiently allocates about N*log2(N) cells: ProtoContext::newList(n, items)
// builds the AVL list by n repeated appendLast path copies, and the whole
// trail stays in the consumer's young generation until the build ends, so none
// of it can be reclaimed while the drain is in flight.  Measured against this
// test's ceiling of 412,144 cells: N = 20,000 completes, N = 30,000 runs out
// of memory - and a control that only calls newList, with no queue anywhere,
// hits the same wall at the same N.  The wall is therefore the bulk builder's,
// not the queue's.  10,000 in flight leaves about a 3x margin, and the full
// 8 x 1,000,000 run completes under the same ceiling with the collector
// running throughout (measured: 15 s, 399 GC cycles, live set oscillating
// between 39,000 and 135,000 cells).
constexpr long kMaxInFlight = 10000;

// Shared state handed to every producer thread through args[0].  The queue
// handle is an ordinary ProtoObject word and is passed through args[1] so it
// stays a GC root of the producer's own context while the thread runs.
struct ProducerJob {
    const ProtoMPSCQueue* queue;
    long pushes;
    std::atomic<long> nextProducer;
    std::atomic<long> pushed;
    // Published by the consumer after every batch; read by the producers to
    // hold the in-flight set at or below kMaxInFlight.
    std::atomic<long> consumed;
    // Set by the consumer when it gives up, so a producer waiting on
    // backpressure can never outlive it and hang the join.
    std::atomic<bool> abort;
    // How often a producer actually had to wait.  Asserted non-zero: if the
    // bound is ever raised until it stops binding, this test silently becomes
    // the unbounded one again, and that is the failure it exists to prevent.
    std::atomic<long> throttled;
};

// A protoCore context owns its young generation until it is destroyed
// (ProtoContext::~ProtoContext submits the chain to the collector).  A
// producer that pushed a million nodes through ONE context would therefore
// keep every node out of the collector's reach and exhaust the heap - which
// is a property of protoCore's context model, not of the queue.  Real
// producers are actor turns: one context per invocation.  The loop below
// models that with one context per kPushBatch pushes, which is also what
// makes the nodes candidates and so what actually exercises the retain
// chain (a node is only at risk once it is a candidate).
constexpr long kPushBatch = 2000;

const ProtoObject* producerEntry(ProtoContext* ctx,
                                 const ProtoObject*,
                                 const ParentLink*,
                                 const ProtoList* args,
                                 const ProtoSparseList*) {
    if (!args || args->getSize(ctx) < 1) return PROTO_NONE;
    auto* job = reinterpret_cast<ProducerJob*>(args->getAt(ctx, 0)->asLong(ctx));
    const long p = job->nextProducer.fetch_add(1, std::memory_order_relaxed);
    for (long i = 0; i < job->pushes; ) {
        ProtoContext turn(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
        const long end = std::min(i + kPushBatch, job->pushes);
        for (; i < end; ++i) {
            if (job->pushed.load(std::memory_order_relaxed) -
                    job->consumed.load(std::memory_order_relaxed) >= kMaxInFlight) {
                // Wait inside an UnmanagedScope: a producer that sat here in
                // the running set would delay every stop-the-world for as long
                // as it waited, and this test's whole point is that it does
                // not.  No ProtoObject is touched inside the scope.
                job->throttled.fetch_add(1, std::memory_order_relaxed);
                ProtoContext::UnmanagedScope parked(&turn);
                while (job->pushed.load(std::memory_order_relaxed) -
                           job->consumed.load(std::memory_order_relaxed) >= kMaxInFlight &&
                       !job->abort.load(std::memory_order_relaxed))
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            if (job->abort.load(std::memory_order_relaxed)) return PROTO_NONE;
            job->queue->push(&turn, turn.fromInteger(p * kStride + i));
            // Per push, not per turn: the bound is only a bound if the
            // producers see the consumer's progress at the same granularity
            // at which they add to it.
            job->pushed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return PROTO_NONE;
}

}  // namespace

TEST(MPSCQueueConcurrency, EightProducersOneConsumerLoseNothingAndDuplicateNothing) {
    const long kPushes = pushesPerProducer();

    ProtoSpace space;
    ProtoContext consumerCtx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    // Keep the queue reachable for the whole run through a root set: the
    // consumer's list batches reference the items, but the queue itself must
    // stay rooted for the retain chain to be traced.
    ProtoRootSet* rs = space.createRootSet("mpsc-stress");
    ASSERT_NE(rs, nullptr);
    const ProtoMPSCQueue* q = consumerCtx.newMPSCQueue();
    const ProtoRootSet::Handle pinned = rs->add(q->asObject(&consumerCtx));
    ASSERT_NE(pinned, ProtoRootSet::kNullHandle);

    ProducerJob job{q, kPushes, {0}, {0}, {0}, {false}, {0}};

    // Bound the heap so the collector runs during the stress.  150,000 cells
    // of headroom against kMaxInFlight = 10,000 (see the sizing note above);
    // the two numbers are a pair and neither may be changed alone.
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + 150000);
    const uint64_t cyclesAtStart = space.getGCCycleCount();

    const ProtoString* name = ProtoString::createSymbol(&consumerCtx, "mpsc-producer");
    std::vector<const ProtoThread*> producers;
    for (long p = 0; p < kProducers; ++p) {
        const ProtoList* targs = consumerCtx.newList()->appendLast(
            &consumerCtx, consumerCtx.fromLong(reinterpret_cast<long long>(&job)));
        const ProtoThread* t = space.newThread(&consumerCtx, name, &producerEntry, targs, nullptr);
        ASSERT_NE(t, nullptr);
        producers.push_back(t);
    }

    std::vector<long> nextExpected(kProducers, 0);
    long consumed = 0;
    long outOfOrder = 0;
    long badProducerId = 0;
    bool timedOut = false;
    const long total = kProducers * kPushes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(15);

    while (consumed < total) {
        // One context per drained batch, for the same reason the producers
        // use one per turn: the retain cell and the returned list must
        // become candidates, or the test never exercises the collector.
        ProtoContext turn(&space, &consumerCtx, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* batch = q->takeAll(&turn);
        const unsigned long n = batch->getSize(&turn);
        for (unsigned long i = 0; i < n; ++i) {
            const long v = static_cast<long>(
                batch->getAt(&turn, static_cast<int>(i))->asLong(&turn));
            const long p = v / kStride;
            const long s = v % kStride;
            // Counted, not asserted: an ASSERT here would return from the
            // test body with eight producers still waiting on backpressure,
            // and nothing would ever release them.  Every check below is made
            // after the producers have been joined.
            if (p < 0 || p >= kProducers) { ++badProducerId; continue; }
            if (s != nextExpected[p]) ++outOfOrder;   // per-producer FIFO
            nextExpected[p] = s + 1;
            ++consumed;
        }
        // Publishing the count is what lets the producers run: this is the
        // other half of the backpressure loop, not bookkeeping.
        job.consumed.store(consumed, std::memory_order_relaxed);
        if (n == 0) std::this_thread::yield();
        if (std::chrono::steady_clock::now() >= deadline) { timedOut = true; break; }
    }

    // Release any producer parked on backpressure before joining, whether the
    // loop above finished or gave up.
    job.abort.store(true, std::memory_order_relaxed);
    for (const ProtoThread* t : producers)
        const_cast<ProtoThread*>(t)->join(&consumerCtx);

    // Drain anything pushed after the loop's last takeAll.
    for (;;) {
        ProtoContext turn(&space, &consumerCtx, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* batch = q->takeAll(&turn);
        const unsigned long n = batch->getSize(&turn);
        if (n == 0) break;
        for (unsigned long i = 0; i < n; ++i) {
            const long v = static_cast<long>(
                batch->getAt(&turn, static_cast<int>(i))->asLong(&turn));
            const long p = v / kStride;
            const long s = v % kStride;
            if (p < 0 || p >= kProducers) { ++badProducerId; continue; }
            if (s != nextExpected[p]) ++outOfOrder;
            nextExpected[p] = s + 1;
            ++consumed;
        }
    }

    space.setHeapLimits(0, 0);
    rs->remove(pinned);

    EXPECT_FALSE(timedOut) << "consumed " << consumed << " of " << total
                           << " before the deadline";
    EXPECT_EQ(badProducerId, 0) << "an item carried an impossible producer id";
    EXPECT_EQ(consumed, total) << "items lost or duplicated";
    EXPECT_EQ(job.pushed.load(), total) << "a producer did not finish";
    EXPECT_EQ(outOfOrder, 0) << "per-producer FIFO order broken";
    // Two guards against a vacuous pass.  Before the bulk-list-builder fix
    // this test ran with the collector completing ZERO cycles, which made
    // "no loss under concurrent marking" a claim about a mark that never
    // happened; and a backpressure bound that never binds is no bound at all.
    EXPECT_GE(space.getGCCycleCount() - cyclesAtStart, 10u)
        << "the collector never ran: nothing here was tested against marking";
    EXPECT_GT(job.throttled.load(), 0)
        << "the in-flight bound never bound; this is the unbounded test again";
    for (long p = 0; p < kProducers; ++p)
        EXPECT_EQ(nextExpected[p], kPushes) << "producer " << p;

    // Self-report, so a silent failure cannot read as a pass.
    std::cout << "MPSC stress: producers=" << kProducers
              << " pushes/producer=" << kPushes
              << " max in flight=" << kMaxInFlight
              << " pushed=" << job.pushed.load()
              << " consumed=" << consumed
              << " gc cycles=" << space.getGCCycleCount() << std::endl;
}
