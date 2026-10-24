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
