/*
 * mpsc_queue_benchmark.cpp
 *
 * ProtoMPSCQueue against the two mailboxes it replaces, in one process, on
 * one machine:
 *
 *   protoCore ProtoMPSCQueue  - lock-free, O(1) send, GC-traced.
 *   protoClojure-shaped stack - std::atomic<Msg*> CAS stack on the C++
 *                               heap: lock-free and O(1), but the payload
 *                               is NOT a GC root (the defect PMQ-SPEC
 *                               section 1 records).  Its number is a floor,
 *                               not a target: it buys its speed by being
 *                               unsafe, and closing that gap is the whole
 *                               point of the new type.
 *   protoST-shaped mailbox    - a ProtoList under a mutable attribute,
 *                               updated with setAttributeIfEqual in a
 *                               CAS-retry loop: GC-safe, O(log n) cells per
 *                               send plus a retry under contention.
 *
 * Two measurements, because they answer different questions:
 *
 *   Table 1 - SEND cost.  What a producer pays per message.  This is the
 *             path an actor runtime executes N times per turn and the one
 *             PMQ-SPEC section 1 exists to make cheap.  Each producer times
 *             only its own loop, so the consumer's speed cannot flatter or
 *             penalise the number.
 *
 *   Table 2 - DRAIN cost per batch size.  What a consumer pays to turn a
 *             batch into something it can iterate.  Reported separately
 *             because protoCore's bulk list builder
 *             (ProtoContext::newList(n, items)) is O(n log n) cells for
 *             n > 5 - it repeats appendLast - so takeAll's per-item cost
 *             grows with the batch, while protoST's drain is a pointer
 *             swap that pays the same cost on the send side instead.
 *
 * Every mode verifies the consumed count against the pushed one and prints
 * what it did.  The program exits non-zero on any mismatch, so a silent
 * failure can never be read as throughput.
 *
 * Producers in the protoCore modes are protoCore threads
 * (ProtoSpace::newThread), and every producer turn gets its own
 * ProtoContext: a context owns its young generation until it is destroyed,
 * so a single long-lived context would never let the collector reclaim a
 * node and the run would hit the heap ceiling instead of measuring
 * anything.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#include "../headers/protoCore.h"

using namespace proto;

namespace {

double seconds(std::chrono::steady_clock::time_point a,
               std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

constexpr long kStride = 10000000;
constexpr long kTurn   = 2000;      // sends per producer ProtoContext

struct SendResult {
    const char* name;
    int producers;
    long pushed;
    long consumed;
    double sendWall;      // longest producer's own loop
    double sendCpuSum;    // sum of the producers' loops
    long batches;
    bool gcSafe;
};

void reportSend(const SendResult& r, bool& ok) {
    const bool good = r.consumed == r.pushed;
    ok = ok && good;
    std::printf("  %s | producers=%d | msgs=%8ld | consumed=%8ld | "
                "send wall=%7.3f s | sends/s=%11.0f | mean ns/send=%8.0f | "
                "drains=%7ld | gc-safe=%s%s\n",
                r.name, r.producers, r.pushed, r.consumed, r.sendWall,
                r.sendWall > 0 ? r.pushed / r.sendWall : 0.0,
                r.pushed > 0 ? r.sendCpuSum * 1e9 / r.pushed : 0.0,
                r.batches, r.gcSafe ? "yes" : "NO ",
                good ? "" : "  <-- MISMATCH");
    std::fflush(stdout);
}

//--------------------------------------------------------------- protoCore
struct PmqJob {
    const ProtoMPSCQueue* queue;
    long perProducer;
    std::atomic<long> nextProducer;
    std::atomic<long> doneCount;
    std::atomic<long> sendNanosSum;
    std::atomic<long> sendNanosMax;
};

void recordProducer(PmqJob* job, long nanos) {
    job->sendNanosSum.fetch_add(nanos, std::memory_order_relaxed);
    long prev = job->sendNanosMax.load(std::memory_order_relaxed);
    while (nanos > prev &&
           !job->sendNanosMax.compare_exchange_weak(prev, nanos,
                                                    std::memory_order_relaxed)) {}
    job->doneCount.fetch_add(1, std::memory_order_release);
}

const ProtoObject* pmqProducer(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                               const ProtoList* args, const ProtoSparseList*) {
    auto* job = reinterpret_cast<PmqJob*>(args->getAt(ctx, 0)->asLong(ctx));
    const long p = job->nextProducer.fetch_add(1, std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < job->perProducer; ) {
        ProtoContext turn(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
        const long end = (i + kTurn < job->perProducer) ? i + kTurn : job->perProducer;
        for (; i < end; ++i) job->queue->push(&turn, turn.fromInteger(p * kStride + i));
    }
    recordProducer(job, std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - t0).count());
    return PROTO_NONE;
}

SendResult runProtoCore(ProtoSpace& space, ProtoContext* main, int producers, long perProducer) {
    const ProtoMPSCQueue* q = main->newMPSCQueue();
    ProtoRootSet* rs = space.createRootSet("bench-pmq");
    const ProtoRootSet::Handle pinned = rs->add(q->asObject(main));

    PmqJob job{q, perProducer, {0}, {0}, {0}, {0}};
    const long total = static_cast<long>(producers) * perProducer;

    const ProtoString* name = ProtoString::createSymbol(main, "bench-pmq-producer");
    const ProtoObject* handle = main->fromLong(reinterpret_cast<long long>(&job));

    std::vector<const ProtoThread*> ps;
    for (int p = 0; p < producers; ++p)
        ps.push_back(space.newThread(main, name, &pmqProducer,
                                     main->newList()->appendLast(main, handle), nullptr));

    // This thread is the consumer: single-consumer is the contract.
    long got = 0;
    long batches = 0;
    while (got < total) {
        ProtoContext turn(&space, main, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* batch = q->takeAll(&turn);
        const unsigned long n = batch->getSize(&turn);
        if (n == 0) { std::this_thread::yield(); continue; }
        ++batches;
        // Counted by batch size, exactly as the protoST row does.  Walking a
        // ProtoList with getAt is O(log n) per element and would measure
        // list indexing, not the mailbox.
        got += static_cast<long>(n);
    }
    for (const ProtoThread* t : ps) const_cast<ProtoThread*>(t)->join(main);
    rs->remove(pinned);

    return {"ProtoMPSCQueue                          ", producers, total, got,
            job.sendNanosMax.load() / 1e9, job.sendNanosSum.load() / 1e9,
            batches, true};
}

//---------------------------------------------------------- protoClojure
struct Msg { long value; Msg* next; };

SendResult runClojureShape(int producers, long perProducer) {
    std::atomic<Msg*> head{nullptr};
    std::atomic<long> consumed{0};
    std::atomic<long> batches{0};
    std::atomic<long> nanosSum{0};
    std::atomic<long> nanosMax{0};
    const long total = static_cast<long>(producers) * perProducer;

    std::thread consumer([&] {
        long got = 0;
        while (got < total) {
            Msg* chain = head.exchange(nullptr, std::memory_order_acq_rel);
            if (!chain) { std::this_thread::yield(); continue; }
            batches.fetch_add(1, std::memory_order_relaxed);
            while (chain) { Msg* n = chain->next; delete chain; chain = n; ++got; }
        }
        consumed.store(got);
    });

    std::vector<std::thread> ps;
    for (int p = 0; p < producers; ++p) {
        ps.emplace_back([&, p] {
            const auto t0 = std::chrono::steady_clock::now();
            for (long i = 0; i < perProducer; ++i) {
                Msg* m = new Msg{p * kStride + i, nullptr};
                Msg* h = head.load(std::memory_order_relaxed);
                do { m->next = h; }
                while (!head.compare_exchange_weak(h, m, std::memory_order_release,
                                                   std::memory_order_relaxed));
            }
            const long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            nanosSum.fetch_add(ns, std::memory_order_relaxed);
            long prev = nanosMax.load(std::memory_order_relaxed);
            while (ns > prev && !nanosMax.compare_exchange_weak(prev, ns,
                                                                std::memory_order_relaxed)) {}
        });
    }
    for (auto& t : ps) t.join();
    consumer.join();

    return {"protoClojure atomic stack (NOT rooted)  ", producers, total,
            consumed.load(), nanosMax.load() / 1e9, nanosSum.load() / 1e9,
            batches.load(), false};
}

//------------------------------------------------------------- protoST
struct StJob {
    const ProtoObject* actor;
    const ProtoString* key;
    long perProducer;
    std::atomic<long> nextProducer;
    std::atomic<long> sendNanosSum;
    std::atomic<long> sendNanosMax;
};

const ProtoObject* stProducer(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoList* args, const ProtoSparseList*) {
    auto* job = reinterpret_cast<StJob*>(args->getAt(ctx, 0)->asLong(ctx));
    const long p = job->nextProducer.fetch_add(1, std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < job->perProducer; ) {
        ProtoContext turn(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
        const long end = (i + kTurn < job->perProducer) ? i + kTurn : job->perProducer;
        for (; i < end; ++i) {
            for (;;) {
                const ProtoObject* old = job->actor->getOwnAttributeDirect(&turn, job->key);
                const ProtoList* mb =
                    (old && old != PROTO_NONE) ? old->asList(&turn) : turn.newList();
                const ProtoObject* neu =
                    mb->appendLast(&turn, turn.fromInteger(p * kStride + i))->asObject(&turn);
                if (const_cast<ProtoObject*>(job->actor)
                        ->setAttributeIfEqual(&turn, job->key, old, neu))
                    break;
            }
        }
    }
    const long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    job->sendNanosSum.fetch_add(ns, std::memory_order_relaxed);
    long prev = job->sendNanosMax.load(std::memory_order_relaxed);
    while (ns > prev && !job->sendNanosMax.compare_exchange_weak(prev, ns,
                                                                 std::memory_order_relaxed)) {}
    return PROTO_NONE;
}

SendResult runSTShape(ProtoSpace& space, ProtoContext* main, int producers, long perProducer) {
    const ProtoObject* actor = main->newObject(true);
    const ProtoString* key = ProtoString::createSymbol(main, "__mailbox__");
    actor->setAttribute(main, key, main->newList()->asObject(main));

    ProtoRootSet* rs = space.createRootSet("bench-st");
    const ProtoRootSet::Handle pinned = rs->add(actor);

    StJob job{actor, key, perProducer, {0}, {0}, {0}};
    const long total = static_cast<long>(producers) * perProducer;

    const ProtoString* name = ProtoString::createSymbol(main, "bench-st-producer");
    const ProtoObject* handle = main->fromLong(reinterpret_cast<long long>(&job));

    std::vector<const ProtoThread*> ps;
    for (int p = 0; p < producers; ++p)
        ps.push_back(space.newThread(main, name, &stProducer,
                                     main->newList()->appendLast(main, handle), nullptr));

    long got = 0;
    long batches = 0;
    while (got < total) {
        ProtoContext turn(&space, main, nullptr, nullptr, nullptr, nullptr);
        // The whole-batch equivalent of takeAll: swap the list for an empty
        // one, exactly as protoST's per-turn drain does.
        const ProtoObject* old = actor->getOwnAttributeDirect(&turn, key);
        const ProtoList* mb = (old && old != PROTO_NONE) ? old->asList(&turn) : nullptr;
        if (!mb || mb->getSize(&turn) == 0) { std::this_thread::yield(); continue; }
        const ProtoObject* empty = turn.newList()->asObject(&turn);
        if (!const_cast<ProtoObject*>(actor)->setAttributeIfEqual(&turn, key, old, empty))
            continue;
        ++batches;
        got += static_cast<long>(mb->getSize(&turn));
    }
    for (const ProtoThread* t : ps) const_cast<ProtoThread*>(t)->join(main);
    rs->remove(pinned);

    return {"protoST CAS-list mailbox                ", producers, total, got,
            job.sendNanosMax.load() / 1e9, job.sendNanosSum.load() / 1e9,
            batches, true};
}

//------------------------------------------------------- drain batch cost
// Single-threaded, so the numbers are pure per-item costs of the two
// halves of a mailbox turn.
void drainCostTable(bool& ok) {
    std::cout << "\nTable 2 - single-threaded cost of one mailbox turn, by batch size"
              << std::endl;
    std::cout << "  (push = per message on the send side; drain = per message to turn"
                 " the batch\n   into something the consumer can iterate)" << std::endl;

    for (long batch : {10L, 100L, 1000L, 10000L, 100000L}) {
        // --- ProtoMPSCQueue
        double pmqPush = 0, pmqDrain = 0;
        long pmqGot = 0;
        {
            ProtoSpace space;
            ProtoContext main(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
            ProtoRootSet* rs = space.createRootSet("bench-pmq-batch");
            const ProtoMPSCQueue* q = main.newMPSCQueue();
            const ProtoRootSet::Handle pinned = rs->add(q->asObject(&main));
            const long reps = 200000 / batch + 1;
            const auto a = std::chrono::steady_clock::now();
            std::chrono::nanoseconds drainNs{0};
            for (long r = 0; r < reps; ++r) {
                ProtoContext turn(&space, &main, nullptr, nullptr, nullptr, nullptr);
                for (long i = 0; i < batch; ++i) q->push(&turn, turn.fromInteger(i));
                const auto d0 = std::chrono::steady_clock::now();
                const ProtoList* l = q->takeAll(&turn);
                pmqGot += static_cast<long>(l->getSize(&turn));
                drainNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - d0);
            }
            const double totalS = seconds(a, std::chrono::steady_clock::now());
            const double drainS = drainNs.count() / 1e9;
            pmqDrain = drainS * 1e9 / (reps * batch);
            pmqPush = (totalS - drainS) * 1e9 / (reps * batch);
            if (pmqGot != reps * batch) ok = false;
            rs->remove(pinned);
        }

        // --- protoST shape
        double stPush = 0, stDrain = 0;
        long stGot = 0;
        {
            ProtoSpace space;
            ProtoContext main(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
            ProtoRootSet* rs = space.createRootSet("bench-st-batch");
            const ProtoObject* actor = main.newObject(true);
            const ProtoString* key = ProtoString::createSymbol(&main, "__mailbox__");
            actor->setAttribute(&main, key, main.newList()->asObject(&main));
            const ProtoRootSet::Handle pinned = rs->add(actor);
            const long reps = 200000 / batch + 1;
            const auto a = std::chrono::steady_clock::now();
            std::chrono::nanoseconds drainNs{0};
            for (long r = 0; r < reps; ++r) {
                ProtoContext turn(&space, &main, nullptr, nullptr, nullptr, nullptr);
                for (long i = 0; i < batch; ++i) {
                    const ProtoObject* old = actor->getOwnAttributeDirect(&turn, key);
                    const ProtoList* mb =
                        (old && old != PROTO_NONE) ? old->asList(&turn) : turn.newList();
                    const ProtoObject* neu =
                        mb->appendLast(&turn, turn.fromInteger(i))->asObject(&turn);
                    const_cast<ProtoObject*>(actor)->setAttributeIfEqual(&turn, key, old, neu);
                }
                const auto d0 = std::chrono::steady_clock::now();
                const ProtoObject* old = actor->getOwnAttributeDirect(&turn, key);
                const ProtoList* mb = old->asList(&turn);
                stGot += static_cast<long>(mb->getSize(&turn));
                const_cast<ProtoObject*>(actor)->setAttributeIfEqual(
                    &turn, key, old, turn.newList()->asObject(&turn));
                drainNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - d0);
            }
            const double totalS = seconds(a, std::chrono::steady_clock::now());
            const double drainS = drainNs.count() / 1e9;
            stDrain = drainS * 1e9 / (reps * batch);
            stPush = (totalS - drainS) * 1e9 / (reps * batch);
            if (stGot != reps * batch) ok = false;
            rs->remove(pinned);
        }

        std::printf("  batch=%6ld | PMQ push=%8.0f ns  drain=%8.0f ns  total=%8.0f ns"
                    " | protoST push=%8.0f ns  drain=%8.0f ns  total=%8.0f ns\n",
                    batch, pmqPush, pmqDrain, pmqPush + pmqDrain,
                    stPush, stDrain, stPush + stDrain);
        std::fflush(stdout);
    }
}

}  // namespace

int main() {
    // One message count for every mode, so the rows compare directly.
    const long perProducer = 50000;

    std::cout << "--- ProtoMPSCQueue microbenchmark (PMQ-SPEC section 5) ---" << std::endl;
    std::cout << "Each row states the work it did; the runner verifies consumed == pushed."
              << std::endl;
    std::cout << "\nTable 1 - send cost with 1/2/4/8 producers, " << perProducer
              << " messages per producer." << std::endl;
    std::cout << "  'send wall' is the slowest producer's own loop, so the consumer's"
                 " speed\n  cannot flatter or penalise it." << std::endl;

    bool ok = true;
    for (int producers : {1, 2, 4, 8}) {
        {
            ProtoSpace space;
            ProtoContext main(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
            reportSend(runProtoCore(space, &main, producers, perProducer), ok);
        }
        reportSend(runClojureShape(producers, perProducer), ok);
        {
            ProtoSpace space;
            ProtoContext main(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
            reportSend(runSTShape(space, &main, producers, perProducer), ok);
        }
    }

    drainCostTable(ok);

    std::cout << (ok ? "VERIFIED" : "MISMATCH") << std::endl;
    return ok ? 0 : 1;
}
