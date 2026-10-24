// ProtoLazyObjectTests.cpp - lazy objects (ProtoObject::newLazyChild).
//
// Semantics: the state machine, exclusive claims, a single publication, the
// object being an ordinary immutable object otherwise.
//
// Collection, with cycles forced by a small hard heap limit (the pattern of
// ProtoMPSCQueueGCTests.cpp):
//   1. a published value whose only reference is the lazy object survives;
//   2. the producer is released once the value is published, while the
//      object itself stays alive;
//   3. a long forward chain of lazy objects (each value pointing at the next
//      object) is freed as soon as its head is unreachable -- the property
//      a chain of MUTABLE objects does not have, since the global mutable
//      table roots every state until the handle has been swept.
// External pointers with counting finalizers are the probes: a finalizer
// runs only when the collector frees the cell.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
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

std::atomic<int> g_finalized{0};
void countFinalize(void*) { g_finalized.fetch_add(1, std::memory_order_relaxed); }

const ProtoObject* probe(ProtoContext* c) {
    return c->fromExternalPointer(nullptr, &countFinalize);
}

} // namespace

TEST(ProtoLazyObject, StateMachine) {
    ProtoSpace space;
    ProtoContext ctx(&space);
    ctx.resizeAutomaticLocals(3);
    const ProtoObject* proto = ctx.newObject(false);
    ctx.setAutomaticLocal(0, proto);
    const ProtoObject* producer = ctx.newList()->appendLast(&ctx, ctx.fromInteger(7))->asObject(&ctx);
    ctx.setAutomaticLocal(1, producer);
    const ProtoObject* lazy = proto->newLazyChild(&ctx, producer);
    ctx.setAutomaticLocal(2, lazy);

    EXPECT_TRUE(lazy->isLazyObject(&ctx));
    EXPECT_FALSE(proto->isLazyObject(&ctx));
    EXPECT_EQ(lazy->getPrototype(&ctx), proto);
    EXPECT_TRUE(lazy->isInstanceOf(&ctx, proto));
    EXPECT_EQ(lazy->getLazyState(&ctx), ProtoObject::LazyUnrealized);
    EXPECT_EQ(proto->getLazyState(&ctx), ProtoObject::LazyNotLazy);
    EXPECT_EQ(lazy->getLazyProducer(&ctx), producer);
    EXPECT_EQ(lazy->getLazyValue(&ctx), nullptr);

    EXPECT_TRUE(lazy->claimLazy(&ctx));
    EXPECT_FALSE(lazy->claimLazy(&ctx));
    EXPECT_EQ(lazy->getLazyState(&ctx), ProtoObject::LazyClaimed);
    lazy->unclaimLazy(&ctx);
    EXPECT_EQ(lazy->getLazyState(&ctx), ProtoObject::LazyUnrealized);
    EXPECT_TRUE(lazy->claimLazy(&ctx));

    const ProtoObject* v = ctx.fromInteger(42);
    EXPECT_TRUE(lazy->publishLazy(&ctx, v));
    EXPECT_FALSE(lazy->publishLazy(&ctx, ctx.fromInteger(43)));
    EXPECT_FALSE(lazy->claimLazy(&ctx));
    EXPECT_EQ(lazy->getLazyState(&ctx), ProtoObject::LazyRealized);
    EXPECT_EQ(lazy->getLazyValue(&ctx), v);
    EXPECT_EQ(lazy->getLazyProducer(&ctx), nullptr);

    // Publishing without a claim, and nullptr as a value.
    const ProtoObject* lazy2 = proto->newLazyChild(&ctx, PROTO_NONE);
    EXPECT_TRUE(lazy2->publishLazy(&ctx, nullptr));
    EXPECT_EQ(lazy2->getLazyValue(&ctx), PROTO_NONE);

    // Otherwise an immutable object: setAttribute answers a new, plain one.
    const ProtoString* key = ProtoString::createSymbol(&ctx, "k");
    const ProtoObject* changed = lazy->setAttribute(&ctx, key, ctx.fromInteger(1));
    EXPECT_NE(changed, lazy);
    EXPECT_FALSE(changed->isLazyObject(&ctx));
    EXPECT_EQ(lazy->hasOwnAttribute(&ctx, key), PROTO_FALSE);

    // Non-objects: not lazy, and newLazyChild makes a child of the prototype.
    EXPECT_EQ(ctx.fromInteger(3)->getLazyState(&ctx), ProtoObject::LazyNotLazy);
    EXPECT_FALSE(ctx.fromInteger(3)->claimLazy(&ctx));
    EXPECT_FALSE(ctx.fromInteger(3)->publishLazy(&ctx, PROTO_NONE));
}

TEST(ProtoLazyObject, OneClaimAndOnePublicationAcrossThreads) {
    ProtoSpace space;
    ProtoContext ctx(&space);
    ctx.resizeAutomaticLocals(1);
    const ProtoObject* lazy = ctx.newObject(false)->newLazyChild(&ctx, PROTO_NONE);
    ctx.setAutomaticLocal(0, lazy);
    std::atomic<int> claims{0}, publications{0};
    std::vector<std::thread> threads;
    // Plain threads touch no ProtoObject beyond these compare-and-sets, which
    // allocate nothing.
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 1000; ++i) {
                if (lazy->claimLazy(&ctx)) {
                    claims.fetch_add(1);
                    lazy->unclaimLazy(&ctx);
                }
            }
            if (lazy->publishLazy(&ctx, reinterpret_cast<const ProtoObject*>(
                    static_cast<uintptr_t>(((t + 1) << 10) | 1))))
                publications.fetch_add(1);
        });
    for (auto& th : threads) th.join();
    EXPECT_GE(claims.load(), 1);
    EXPECT_EQ(publications.load(), 1);
    EXPECT_EQ(lazy->getLazyState(&ctx), ProtoObject::LazyRealized);
}

TEST(ProtoLazyObject, ValueSurvivesAndProducerIsReleased) {
    ProtoSpace space;
    ProtoContext ctx(&space);
    ctx.resizeAutomaticLocals(2);
    g_finalized.store(0);
    {
        ProtoContext build(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* producer = probe(&build);
        const ProtoObject* lazy = build.newObject(false)->newLazyChild(&build, producer);
        ctx.setAutomaticLocal(0, lazy);
        // The value: a list holding a second probe, reachable only from the
        // lazy object.
        const ProtoObject* value = build.newList()->appendLast(&build, probe(&build))->asObject(&build);
        ASSERT_TRUE(lazy->publishLazy(&build, value));
    }
    ASSERT_GE(forceCycles(space, &ctx, 3), 3u);
    const ProtoObject* lazy = ctx.getAutomaticLocal(0);
    // The producer was freed (one finalizer), the value was not.
    EXPECT_EQ(g_finalized.load(), 1);
    const ProtoObject* value = lazy->getLazyValue(&ctx);
    ASSERT_NE(value, nullptr);
    ASSERT_EQ(value->asList(&ctx)->getSize(&ctx), 1u);
    EXPECT_NE(value->asList(&ctx)->getAt(&ctx, 0)->asExternalPointer(&ctx), nullptr);
    EXPECT_EQ(lazy->getLazyProducer(&ctx), nullptr);
}

TEST(ProtoLazyObject, UnpublishedProducerSurvives) {
    ProtoSpace space;
    ProtoContext ctx(&space);
    ctx.resizeAutomaticLocals(1);
    g_finalized.store(0);
    {
        ProtoContext build(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        ctx.setAutomaticLocal(0, build.newObject(false)->newLazyChild(&build, probe(&build)));
    }
    ASSERT_GE(forceCycles(space, &ctx, 3), 3u);
    EXPECT_EQ(g_finalized.load(), 0);
    const ProtoObject* producer = ctx.getAutomaticLocal(0)->getLazyProducer(&ctx);
    ASSERT_NE(producer, nullptr);
    EXPECT_NE(producer->asExternalPointer(&ctx), nullptr);
    // Claimed is not Realized: still traced.
    ASSERT_TRUE(ctx.getAutomaticLocal(0)->claimLazy(&ctx));
    ASSERT_GE(forceCycles(space, &ctx, 2), 2u);
    EXPECT_EQ(g_finalized.load(), 0);
}

TEST(ProtoLazyObject, UnreachableForwardChainIsFreedAtOnce) {
    ProtoSpace space;
    ProtoContext ctx(&space);
    ctx.resizeAutomaticLocals(1);
    g_finalized.store(0);
    constexpr int kLinks = 3000;
    // L0 -> (probe, L1) -> (probe, L2) -> ... built front to back, as a lazy
    // sequence is realised; only the newest link stays rooted.
    const ProtoObject* proto = nullptr;
    {
        ProtoContext build(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        build.resizeAutomaticLocals(1);
        proto = build.newObject(false);
        build.setAutomaticLocal(0, proto);
        ctx.setAutomaticLocal(0, proto->newLazyChild(&build, PROTO_NONE));
    }
    for (int i = 0; i < kLinks; ++i) {
        ProtoContext step(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* cur = ctx.getAutomaticLocal(0);
        const ProtoObject* next = cur->getPrototype(&step)->newLazyChild(&step, PROTO_NONE);
        const ProtoObject* value = step.newList()->appendLast(&step, probe(&step))
                                       ->appendLast(&step, next)->asObject(&step);
        ASSERT_TRUE(cur->publishLazy(&step, value));
        ctx.setAutomaticLocal(0, next);
    }
    ASSERT_GE(forceCycles(space, &ctx, 3), 3u);
    // Every link but nothing is rooted, so every probe is freed (a few
    // cycles at most, not one per link).
    EXPECT_EQ(g_finalized.load(), kLinks);
}


// Control: the same chain built from MUTABLE objects (each state pointing at
// the next handle) is not freed in a few cycles -- the global mutable table
// roots every state until its handle has been swept, so the chain unravels
// one link per cycle. This is the behaviour lazy objects exist to avoid; if
// the table ever stops rooting dead handles' states, this control fails and
// should be removed.
TEST(ProtoLazyObject, MutableChainControlUnravelsOneLinkPerCycle) {
    ProtoSpace space;
    ProtoContext ctx(&space);
    ctx.resizeAutomaticLocals(1);
    g_finalized.store(0);
    constexpr int kLinks = 3000;
    const ProtoString* nextKey = nullptr;
    const ProtoString* probeKey = nullptr;
    {
        ProtoContext build(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        nextKey = ProtoString::createSymbol(&build, "next");
        probeKey = ProtoString::createSymbol(&build, "probe");
        ctx.setAutomaticLocal(0, build.newObject(true));
    }
    for (int i = 0; i < kLinks; ++i) {
        ProtoContext step(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* cur = ctx.getAutomaticLocal(0);
        const ProtoObject* next = step.newObject(true);
        cur->setAttribute(&step, probeKey, probe(&step));
        cur->setAttribute(&step, nextKey, next);
        ctx.setAutomaticLocal(0, next);
    }
    ASSERT_GE(forceCycles(space, &ctx, 3), 3u);
    EXPECT_LT(g_finalized.load(), kLinks / 2);
}

// ---------------------------------------------------------------------------
// Review hardening (3.1.0).

namespace {

// Requests a collection cycle on demand (the BulkListBuildTests pattern:
// triggerGC() is gated on heap pressure).
void requestCycle(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    space.gcStarted = true;
    space.gcCV.notify_all();
}

// A plain OS thread, outside every quorum, that asks for cycles until stopped.
class CycleKicker {
public:
    explicit CycleKicker(ProtoSpace& space)
        : thread_([this, &space] {
              while (!stop_.load(std::memory_order_relaxed)) {
                  requestCycle(space);
                  std::this_thread::sleep_for(std::chrono::microseconds(300));
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

} // namespace

// A lazy object whose value reaches the object itself (a self-referential
// lazy sequence) is a cycle with no mutable handle. It is an ordinary
// immutable cycle: findMutableCycles must not report it (before 3.1.0's
// review it was reported as a "protoCore invariant violation"), and the
// collector frees it once unreachable.
TEST(ProtoLazyObject, SelfReferentialValueIsNotAMutableCycleAndIsFreed) {
    ProtoSpace space;
    ProtoContext ctx(&space);
    ctx.resizeAutomaticLocals(2);
    g_finalized.store(0);
    const ProtoString* key = nullptr;
    {
        ProtoContext build(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        key = ProtoString::createSymbol(&build, "seq");
        const ProtoObject* lazy = build.newObject(false)->newLazyChild(&build, PROTO_NONE);
        ctx.setAutomaticLocal(0, lazy);
        const ProtoObject* value = build.newList()->appendLast(&build, probe(&build))
                                       ->appendLast(&build, lazy)->asObject(&build);
        ASSERT_TRUE(lazy->publishLazy(&build, value));
        // The detector walks from mutable states: hold the loop in one, as a
        // runtime's global variable would.
        const ProtoObject* holder = build.newObject(true);
        ctx.setAutomaticLocal(1, holder);
        holder->setAttribute(&build, key, lazy);
    }
    const ProtoObject* lazy = ctx.getAutomaticLocal(0);
    ASSERT_EQ(lazy->getLazyValue(&ctx)->asList(&ctx)->getAt(&ctx, 1), lazy);

    const MutableGraphReport report = space.findMutableCycles(&ctx);
    EXPECT_TRUE(report.acyclicAndComplete())
        << report.cycles.size() << " cycle(s) reported: "
        << (report.cycles.empty() ? std::string() : report.cycles[0].path);

    ASSERT_GE(forceCycles(space, &ctx, 2), 2u);
    EXPECT_EQ(g_finalized.load(), 0);  // still reachable
    ctx.setAutomaticLocal(0, PROTO_NONE);
    ctx.setAutomaticLocal(1, PROTO_NONE);
    // The holder's entry roots the loop until the holder is swept; the loop
    // goes in the cycle after.
    ASSERT_GE(forceCycles(space, &ctx, 4), 4u);
    EXPECT_EQ(g_finalized.load(), 1);
}

// A thread that waits for a lazy object another thread has claimed must not
// hold up a collection. The waiter is a registered ProtoThread polling the
// slot with ProtoThread::yield / sleepFor; while it waits, the space must
// complete collection cycles. A registered thread polling with a bare OS
// yield reaches no safepoint, so no stop-the-world can complete and the
// cycle count does not move: verified by substituting std::this_thread::yield
// / sleep_for in the waiter. The deadline then fails the test, and `giveUp`
// lets the waiter leave its loop, reach a safepoint and release the pause,
// so the failure does not become a hang.
namespace {
struct WaiterShared {
    const ProtoObject* lazy = nullptr;
    std::atomic<bool> waiting{false};
    std::atomic<bool> sawValue{false};
    std::atomic<unsigned> polls{0};
    std::atomic<bool> giveUp{false};
};
WaiterShared* g_waiter = nullptr;
}

TEST(ProtoLazyObject, WaiterOnClaimedLazyObjectDoesNotBlockCollection) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    ProtoContext ctx(&space, root, nullptr, nullptr, nullptr, nullptr);
    ctx.resizeAutomaticLocals(1);
    WaiterShared shared;
    g_waiter = &shared;
    shared.lazy = ctx.newObject(false)->newLazyChild(&ctx, PROTO_NONE);
    ctx.setAutomaticLocal(0, shared.lazy);
    ASSERT_TRUE(shared.lazy->claimLazy(&ctx));  // this thread is the claimer

    auto waiterMain = [](ProtoContext* c, const ProtoObject*, const ParentLink*,
                         const ProtoArguments*, const ProtoKeywordArguments*) -> const ProtoObject* {
        const ProtoObject* lazy = g_waiter->lazy;  // rooted by the test's context
        g_waiter->waiting.store(true);
        for (unsigned round = 0; lazy->getLazyState(c) != ProtoObject::LazyRealized; ++round) {
            if (g_waiter->giveUp.load()) return PROTO_NONE;
            g_waiter->polls.fetch_add(1, std::memory_order_relaxed);
            if (round < 64) ProtoThread::yield(c);
            else ProtoThread::sleepFor(c, 50);
        }
        const ProtoObject* v = lazy->getLazyValue(c);
        g_waiter->sawValue.store(v && v->asLong(c) == 4242);
        return PROTO_NONE;
    };
    const ProtoThread* waiter = space.newThread(
        &ctx, ProtoString::createSymbol(&ctx, "lazy-waiter"), waiterMain, nullptr, nullptr);

    uint64_t advanced = 0;
    {
        // The claimer itself waits off the quorum too.
        ProtoContext::UnmanagedScope parked(&ctx);
        const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!shared.waiting.load() && std::chrono::steady_clock::now() < startDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const uint64_t c0 = space.getGCCycleCount();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (space.getGCCycleCount() < c0 + 3 && std::chrono::steady_clock::now() < deadline) {
            requestCycle(space);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        advanced = space.getGCCycleCount() - c0;
        if (advanced < 3) shared.giveUp.store(true);
    }
    ASSERT_TRUE(shared.waiting.load());
    if (advanced < 3) {
        const_cast<ProtoThread*>(waiter)->join(&ctx);
        g_waiter = nullptr;
        FAIL() << "collections stalled while a thread waited for a claimed lazy object";
    }
    EXPECT_EQ(shared.lazy->getLazyState(&ctx), ProtoObject::LazyClaimed);
    EXPECT_GT(shared.polls.load(), 0u);

    ASSERT_TRUE(shared.lazy->publishLazy(&ctx, ctx.fromInteger(4242)));
    const_cast<ProtoThread*>(waiter)->join(&ctx);
    EXPECT_TRUE(shared.sawValue.load());
    g_waiter = nullptr;
}

// Eight ProtoThreads walk one shared lazy chain, realising it as they go:
// whoever claims a link builds the next link and publishes [i, next]; the
// others wait with ProtoThread::yield. Cycles are requested continuously, so
// the marker reads slots while they are being claimed and published, and
// producers are released while other threads still walk. Every thread must
// see 0, 1, ..., kLinks - 1. Run under ThreadSanitizer and AddressSanitizer.
namespace {
constexpr int kSharedLinks = 1500;
struct ChainShared {
    const ProtoObject* head = nullptr;
    const ProtoObject* proto = nullptr;
    std::atomic<int> bad{0};
    std::atomic<int> finished{0};
    std::atomic<int> producedHere{0};
};
ChainShared* g_chain = nullptr;

// The value of `node`, realising it when this thread wins the claim.
const ProtoObject* realizeLink(ProtoContext* c, const ProtoObject* node) {
    for (unsigned round = 0;; ++round) {
        const int state = node->getLazyState(c);
        if (state == ProtoObject::LazyRealized) return node->getLazyValue(c);
        if (state == ProtoObject::LazyUnrealized) {
            ProtoContext s(c->space, c, nullptr, nullptr, nullptr, nullptr);
            s.resizeAutomaticLocals(2);
            const ProtoObject* producer = node->getLazyProducer(&s);
            if (!producer) continue;
            s.setAutomaticLocal(0, producer);
            if (!node->claimLazy(&s)) continue;
            const long long i = s.getAutomaticLocal(0)->asList(&s)->getAt(&s, 0)->asLong(&s);
            // Garbage, so the collector has work while the claim is held.
            for (int g = 0; g < 32; ++g) (void) s.newObject(false);
            s.setAutomaticLocal(1, s.newList()->appendLast(&s, s.fromInteger(i + 1))->asObject(&s));
            s.setAutomaticLocal(1, g_chain->proto->newLazyChild(&s, s.getAutomaticLocal(1)));
            const ProtoObject* value = s.newList()->appendLast(&s, s.fromInteger(i))
                                           ->appendLast(&s, s.getAutomaticLocal(1))->asObject(&s);
            if (!node->publishLazy(&s, value)) g_chain->bad.fetch_add(1);
            g_chain->producedHere.fetch_add(1, std::memory_order_relaxed);
            s.returnValue = value;  // anchored in `c` when `s` ends
            return value;
        }
        ProtoThread::yield(c);
    }
}
}  // namespace

TEST(ProtoLazyObject, EightThreadsRealiseOneSharedChainUnderCollection) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    ProtoContext ctx(&space, root, nullptr, nullptr, nullptr, nullptr);
    ctx.resizeAutomaticLocals(2);
    ChainShared shared;
    g_chain = &shared;
    shared.proto = ctx.newObject(false);
    ctx.setAutomaticLocal(0, shared.proto);
    shared.head = shared.proto->newLazyChild(
        &ctx, ctx.newList()->appendLast(&ctx, ctx.fromInteger(0))->asObject(&ctx));
    ctx.setAutomaticLocal(1, shared.head);

    auto walker = [](ProtoContext* c, const ProtoObject*, const ParentLink*,
                     const ProtoArguments*, const ProtoKeywordArguments*) -> const ProtoObject* {
        ProtoContext w(c->space, c, nullptr, nullptr, nullptr, nullptr);
        w.resizeAutomaticLocals(1);
        w.setAutomaticLocal(0, g_chain->head);
        for (int i = 0; i < kSharedLinks; ++i) {
            ProtoContext step(w.space, &w, nullptr, nullptr, nullptr, nullptr);
            const ProtoObject* value = realizeLink(&step, w.getAutomaticLocal(0));
            const ProtoList* fields = value ? value->asList(&step) : nullptr;
            if (!fields || fields->getSize(&step) != 2 || fields->getAt(&step, 0)->asLong(&step) != i) {
                g_chain->bad.fetch_add(1);
                break;
            }
            w.setAutomaticLocal(0, fields->getAt(&step, 1));
        }
        g_chain->finished.fetch_add(1);
        return PROTO_NONE;
    };

    constexpr int kThreads = 8;
    std::vector<const ProtoThread*> threads;
    const uint64_t c0 = space.getGCCycleCount();
    {
        CycleKicker kicker(space);
        for (int t = 0; t < kThreads; ++t)
            threads.push_back(space.newThread(
                &ctx, ProtoString::createSymbol(&ctx, "lazy-walker"), walker, nullptr, nullptr));
        ProtoContext::UnmanagedScope parked(&ctx);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(600);
        while (shared.finished.load() < kThreads && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (const ProtoThread* t : threads) const_cast<ProtoThread*>(t)->join(&ctx);
    EXPECT_EQ(shared.finished.load(), kThreads);
    EXPECT_EQ(shared.bad.load(), 0);
    EXPECT_EQ(shared.producedHere.load(), kSharedLinks);
    EXPECT_GT(space.getGCCycleCount() - c0, 0u);
    g_chain = nullptr;
}

// The converse: a real mutable cycle closed through a lazy value -- a mutable
// holder whose state reaches a lazy object whose value reaches the holder --
// is retained by the holder's table entry, and findMutableCycles must report
// it. Before the review the detector did not follow lazy values at all and
// answered "acyclic and complete".
TEST(ProtoLazyObject, MutableCycleThroughALazyValueIsReported) {
    ProtoSpace space;
    ProtoContext ctx(&space);
    ctx.resizeAutomaticLocals(1);
    {
        ProtoContext build(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoString* key = ProtoString::createSymbol(&build, "seq");
        const ProtoObject* holder = build.newObject(true);
        ctx.setAutomaticLocal(0, holder);
        const ProtoObject* lazy = build.newObject(false)->newLazyChild(&build, PROTO_NONE);
        holder->setAttribute(&build, key, lazy);
        ASSERT_TRUE(lazy->publishLazy(
            &build, build.newList()->appendLast(&build, holder)->asObject(&build)));
    }
    const MutableGraphReport report = space.findMutableCycles(&ctx);
    EXPECT_FALSE(report.truncated);
    ASSERT_EQ(report.cycles.size(), 1u);
    EXPECT_EQ(report.cycles[0].refs.size(), 1u);
}
