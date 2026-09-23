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

// Shared state handed to every producer thread through args[0].  The queue
// handle is an ordinary ProtoObject word and is passed through args[1] so it
// stays a GC root of the producer's own context while the thread runs.
struct ProducerJob {
    const ProtoMPSCQueue* queue;
    long pushes;
    std::atomic<long> nextProducer;
    std::atomic<long> pushed;
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
        const long start = i;
        for (; i < end; ++i)
            job->queue->push(&turn, turn.fromInteger(p * kStride + i));
        job->pushed.fetch_add(end - start, std::memory_order_relaxed);
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

    ProducerJob job{q, kPushes, {0}, {0}};

    // Bound the heap so the collector runs during the stress.
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + 150000);

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
            ASSERT_GE(p, 0);
            ASSERT_LT(p, kProducers);
            if (s != nextExpected[p]) ++outOfOrder;   // per-producer FIFO
            nextExpected[p] = s + 1;
            ++consumed;
        }
        if (n == 0) std::this_thread::yield();
        ASSERT_LT(std::chrono::steady_clock::now(), deadline)
            << "consumed " << consumed << " of " << total;
    }

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
            if (s != nextExpected[p]) ++outOfOrder;
            nextExpected[p] = s + 1;
            ++consumed;
        }
    }

    space.setHeapLimits(0, 0);
    rs->remove(pinned);

    EXPECT_EQ(consumed, total) << "items lost or duplicated";
    EXPECT_EQ(job.pushed.load(), total) << "a producer did not finish";
    EXPECT_EQ(outOfOrder, 0) << "per-producer FIFO order broken";
    for (long p = 0; p < kProducers; ++p)
        EXPECT_EQ(nextExpected[p], kPushes) << "producer " << p;

    // Self-report, so a silent failure cannot read as a pass.
    std::cout << "MPSC stress: producers=" << kProducers
              << " pushes/producer=" << kPushes
              << " pushed=" << job.pushed.load()
              << " consumed=" << consumed
              << " gc cycles=" << space.getGCCycleCount() << std::endl;
}
