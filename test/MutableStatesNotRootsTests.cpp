// MutableStatesNotRootsTests.cpp -- the global mutable table's states are not
// roots (docs/GLOBAL_MUTABLE_TABLE.md, "States are not roots").
//
// A state is live only while the mark reaches its handle: when the mark
// reaches a mutable handle, it marks the state the handle's ref names in the
// table snapshot taken at the pause.  What must hold:
//   * an unreachable chain of mutables, each state pointing at the next
//     handle, is freed in one cycle (test/ProtoLazyObjectTests.cpp,
//     MutableChainIsFreedInOneCycle, is the regression test for it);
//   * a live chain held by a root loses nothing, however many cycles run;
//   * a handle reached only through another mutable's state stays live, and
//     so does its own state;
//   * an unreachable cycle among mutables is freed too;
//   * a young handle (in a live context) keeps its state;
//   * handles of two spaces linked into one chain behave the same way.
// External pointers with counting finalizers are the probes: a finalizer runs
// only when the collector frees the cell.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace proto;

namespace {

std::atomic<int> g_freed{0};
void countFree(void*) { g_freed.fetch_add(1, std::memory_order_relaxed); }

const ProtoObject* probe(ProtoContext* c) { return c->fromExternalPointer(nullptr, &countFree); }

const ProtoString* sym(ProtoContext* c, const char* s) { return ProtoString::createSymbol(c, s); }

// Forces at least `minCycles` complete cycles in `space` with garbage
// allocated under a small hard limit (the ProtoLazyObjectTests pattern).
uint64_t forceCycles(ProtoSpace& space, ProtoContext* parent, uint64_t minCycles) {
    const uint64_t start = space.getGCCycleCount();
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + 40000);
    for (int batch = 0; batch < 400 && space.getGCCycleCount() - start < minCycles; ++batch) {
        ProtoContext garbage(&space, parent, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 5000; ++i) {
            (void) garbage.newObject(false);
            if ((i & 1023) == 0) garbage.safepoint();
        }
        garbage.safepoint();
    }
    space.setHeapLimits(0, 0);
    return space.getGCCycleCount() - start;
}

// One complete cycle that starts after the call (see ProtoLazyObjectTests).
bool runOneFreshCycle(ProtoSpace& space, ProtoContext* ctx) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (space.gcStarted.load() && std::chrono::steady_clock::now() < deadline) {
        ctx->safepoint();
        std::this_thread::yield();
    }
    const uint64_t start = space.getGCCycleCount();
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space.gcStarted = true;
        space.gcCV.notify_all();
    }
    while (std::chrono::steady_clock::now() < deadline) {
        ctx->safepoint();
        if (space.getGCCycleCount() > start && !space.gcStarted.load()) return true;
        std::this_thread::yield();
    }
    return false;
}

// Builds a forward chain of `links` mutable objects in `ctx`'s space: each
// handle's state holds a probe and the next handle.  The head is stored in
// local `headSlot` of `ctx` when `keepHead`; the newest handle (no probe) in
// local `tailSlot`.
void buildChain(ProtoSpace& space, ProtoContext& ctx, int links, bool keepHead,
                unsigned headSlot, unsigned tailSlot) {
    const ProtoString* nextKey = sym(&ctx, "next");
    const ProtoString* probeKey = sym(&ctx, "probe");
    {
        ProtoContext build(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* head = build.newObject(true);
        ctx.setAutomaticLocal(tailSlot, head);
        if (keepHead) ctx.setAutomaticLocal(headSlot, head);
    }
    for (int i = 0; i < links; ++i) {
        ProtoContext step(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* cur = ctx.getAutomaticLocal(tailSlot);
        const ProtoObject* next = step.newObject(true);
        cur->setAttribute(&step, probeKey, probe(&step));
        cur->setAttribute(&step, nextKey, next);
        ctx.setAutomaticLocal(tailSlot, next);
    }
}

}  // namespace

// A chain held by a root: every state is reached through its handle, from the
// head, so nothing is freed and every link still reads.
TEST(MutableStatesNotRoots, LiveChainHeldByARootIsKept) {
    ProtoSpace space;
    ProtoContext ctx(&space);
    ctx.resizeAutomaticLocals(2);
    g_freed.store(0);
    constexpr int kLinks = 3000;
    buildChain(space, ctx, kLinks, /*keepHead=*/true, 0, 1);
    ASSERT_GE(forceCycles(space, &ctx, 4), 4u);
    ASSERT_TRUE(runOneFreshCycle(space, &ctx));
    EXPECT_EQ(g_freed.load(), 0);

    const ProtoString* nextKey = sym(&ctx, "next");
    const ProtoString* probeKey = sym(&ctx, "probe");
    const ProtoObject* cur = ctx.getAutomaticLocal(0);
    int walked = 0;
    while (walked < kLinks) {
        const ProtoObject* p = cur->getAttribute(&ctx, probeKey);
        ASSERT_NE(p, nullptr);
        ASSERT_NE(p, PROTO_NONE);
        ASSERT_NE(p->asExternalPointer(&ctx), nullptr) << "link " << walked;
        cur = cur->getAttribute(&ctx, nextKey);
        ASSERT_NE(cur, nullptr);
        ASSERT_NE(cur, PROTO_NONE);
        ++walked;
    }
    EXPECT_EQ(cur, ctx.getAutomaticLocal(1)) << "the walk ends at the newest handle";
}

// B is referenced only from A's state, and B's state only through B.  A
// rooted A keeps both, through two handle-to-state edges.
TEST(MutableStatesNotRoots, HandleReachedOnlyThroughAnotherStateStaysLive) {
    ProtoSpace space;
    ProtoContext ctx(&space);
    ctx.resizeAutomaticLocals(1);
    g_freed.store(0);
    const ProtoString* childKey = sym(&ctx, "child");
    const ProtoString* valueKey = sym(&ctx, "value");
    const ProtoString* probeKey = sym(&ctx, "probe");
    {
        ProtoContext build(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* a = build.newObject(true);
        const ProtoObject* b = build.newObject(true);
        b->setAttribute(&build, valueKey, build.fromInteger(42));
        b->setAttribute(&build, probeKey, probe(&build));
        a->setAttribute(&build, childKey, b);
        ctx.setAutomaticLocal(0, a);
    }
    ASSERT_GE(forceCycles(space, &ctx, 4), 4u);
    ASSERT_TRUE(runOneFreshCycle(space, &ctx));
    EXPECT_EQ(g_freed.load(), 0);
    const ProtoObject* b = ctx.getAutomaticLocal(0)->getAttribute(&ctx, childKey);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(b, PROTO_NONE);
    const ProtoObject* v = b->getAttribute(&ctx, valueKey);
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->isInteger(&ctx));
    EXPECT_EQ(v->asLong(&ctx), 42);
    EXPECT_NE(b->getAttribute(&ctx, probeKey)->asExternalPointer(&ctx), nullptr);
}

// A handle that only a live context's young generation holds, written once:
// the young walk scans the handle, so its state is marked.
TEST(MutableStatesNotRoots, YoungHandleKeepsItsState) {
    ProtoSpace space;
    ProtoContext ctx(&space);
    g_freed.store(0);
    const ProtoString* probeKey = sym(&ctx, "probe");
    // `young` stays alive across the cycles and never submits: the handle and
    // its first state are young cells, the state's replacement too.
    ProtoContext young(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
    const ProtoObject* h = young.newObject(true);
    h->setAttribute(&young, probeKey, probe(&young));
    ASSERT_GE(forceCycles(space, &ctx, 3), 3u);
    EXPECT_EQ(g_freed.load(), 0);
    const ProtoObject* p = h->getAttribute(&young, probeKey);
    ASSERT_NE(p, nullptr);
    EXPECT_NE(p->asExternalPointer(&young), nullptr);
}

// Two mutables whose states point at each other, unreachable: the cycle is
// unmarked as a whole and freed, and its entries leave the table.  Under
// states-as-roots it was retained for the life of the space.
TEST(MutableStatesNotRoots, UnreachableMutableCycleIsFreed) {
#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    GTEST_SKIP() << "without survivor re-inclusion the table's states stay roots";
#endif
    ProtoSpace space;
    ProtoContext ctx(&space);
    g_freed.store(0);
    const ProtoString* peerKey = sym(&ctx, "peer");
    const ProtoString* probeKey = sym(&ctx, "probe");
    constexpr int kPairs = 200;
    for (int i = 0; i < kPairs; ++i) {
        ProtoContext build(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* a = build.newObject(true);
        const ProtoObject* b = build.newObject(true);
        a->setAttribute(&build, probeKey, probe(&build));
        b->setAttribute(&build, probeKey, probe(&build));
        a->setAttribute(&build, peerKey, b);
        b->setAttribute(&build, peerKey, a);
    }
    ASSERT_TRUE(runOneFreshCycle(space, &ctx));
    EXPECT_EQ(g_freed.load(), 2 * kPairs);
    ASSERT_TRUE(runOneFreshCycle(space, &ctx));
    EXPECT_EQ(multispace::countMutableEntriesOfSpace(&ctx, 0), 0u)
        << "Phase 5b released every entry of the dead pairs";
}

// A dead mutable's table node outlives its state.  The node stays in the live
// table until Phase 5b, and the shard-tree versions the mutators build in the
// meantime share it; those versions are young cells, which the next cycle
// scans.  Here the shard roots are taken while the dead entries are in the
// table, kept outside the heap while their handles die, and only then put in a
// root: the next cycles scan versions whose nodes name the dead states, as a
// mutator's young path copy would make them do.
//   * The dead states' contents are freed in the cycle their handles die.
//   * Those cycles must not read freed cells through the old versions: before
//     the dead states were retired (docs/GLOBAL_MUTABLE_TABLE.md, "Dead states
//     become tombstones"), a freed state was marked and scanned through them,
//     and the cell reused for a later object was kept alive by them.  Every
//     later probe must be freed while the old versions are still held.
TEST(MutableStatesNotRoots, OldTableVersionsNeverKeepReusedCells) {
#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    GTEST_SKIP() << "without survivor re-inclusion the table's states stay roots";
#endif
    ProtoSpace space;
    ProtoContext ctx(&space);
    ctx.resizeAutomaticLocals(1);
    g_freed.store(0);
    const ProtoString* probeKey = sym(&ctx, "probe");
    constexpr int kDead = 2000;
    for (int i = 0; i < kDead; ++i) {
        ProtoContext build(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* d = build.newObject(true);
        d->setAttribute(&build, probeKey, probe(&build));
    }
    // The table's current versions, which hold every dead entry.  They stay
    // alive through the cycle below: the table itself holds them.
    std::vector<const ProtoObject*> versions;
    for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
        if (const ProtoSparseList* r = globalMutableShards[s].root.load(std::memory_order_acquire))
            versions.push_back(reinterpret_cast<const ProtoObject*>(r));
    }
    ASSERT_FALSE(versions.empty());
    ASSERT_TRUE(runOneFreshCycle(space, &ctx));
    EXPECT_EQ(g_freed.load(), kDead) << "the dead states' contents are freed in one cycle";

    // Phase 5b has replaced the versions; now a root reaches them.
    {
        ProtoContext hold(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* list = hold.newList();
        for (const ProtoObject* v : versions) list = list->appendLast(&hold, v);
        ctx.setAutomaticLocal(0, list->asObject(&hold));
    }
    versions.clear();

    // Cells freed above are reused by new probes, all garbage.
    g_freed.store(0);
    constexpr int kLater = 60000;
    for (int batch = 0; batch < kLater / 1000; ++batch) {
        ProtoContext garbage(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 1000; ++i) (void) probe(&garbage);
        garbage.safepoint();
    }
    ASSERT_GE(forceCycles(space, &ctx, 3), 3u);
    ASSERT_TRUE(runOneFreshCycle(space, &ctx));
    ASSERT_TRUE(runOneFreshCycle(space, &ctx));
    EXPECT_EQ(g_freed.load(), kLater) << "an old table version kept a reused cell alive";
    EXPECT_NE(ctx.getAutomaticLocal(0), nullptr);
}

// Handles of two spaces linked into one chain: A0 -> B0 -> A1 -> B1 -> ...
// (each state written through its own space, so it is allocated there).
//   * Held by roots of both spaces (each space roots its own head-of-pair
//     list so its own handles stay reachable in their space), nothing is
//     freed.
//   * Unreachable, it is freed.  With two live spaces the states are roots
//     (docs/GLOBAL_MUTABLE_TABLE.md, "Several spaces"), so it unravels one
//     link per cycle: cycles of both spaces are run until every probe is
//     freed, and at least one per link are needed.
TEST(MutableStatesNotRoots, CrossSpaceChainIsKeptWhileHeldAndFreedWhenDropped) {
#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    GTEST_SKIP() << "without survivor re-inclusion the table's states stay roots";
#endif
    ProtoSpace sa, sb;
    ProtoContext ca(&sa), cb(&sb);
    ca.resizeAutomaticLocals(1);
    cb.resizeAutomaticLocals(1);
    g_freed.store(0);
    const ProtoString* nextKey = sym(&ca, "next");
    const ProtoString* probeKey = sym(&ca, "probe");
    constexpr int kLinks = 40;  // per space

    // Each space's root is a list of all its own handles.
    {
        const ProtoObject* prev = nullptr;
        const ProtoList* ownA = nullptr;
        const ProtoList* ownB = nullptr;
        {
            ProtoContext::CriticalSection csa(&ca);
            ownA = ca.newList();
        }
        {
            ProtoContext::CriticalSection csb(&cb);
            ownB = cb.newList();
        }
        ca.setAutomaticLocal(0, ownA->asObject(&ca));
        cb.setAutomaticLocal(0, ownB->asObject(&cb));
        for (int i = 0; i < 2 * kLinks; ++i) {
            const bool inA = (i % 2) == 0;
            ProtoSpace& s = inA ? sa : sb;
            ProtoContext& owner = inA ? ca : cb;
            ProtoContext step(&s, &owner, nullptr, nullptr, nullptr, nullptr);
            const ProtoObject* h = step.newObject(true);
            h->setAttribute(&step, probeKey, probe(&step));
            if (prev) {
                // The previous handle belongs to the other space; its state
                // is written through a context of that space.
                ProtoContext& prevOwner = inA ? cb : ca;
                ProtoSpace& ps = inA ? sb : sa;
                ProtoContext w(&ps, &prevOwner, nullptr, nullptr, nullptr, nullptr);
                prev->setAttribute(&w, nextKey, h);
            }
            if (inA) {
                ownA = ca.getAutomaticLocal(0)->asList(&step)->appendLast(&step, h);
                ca.setAutomaticLocal(0, ownA->asObject(&step));
            } else {
                ownB = cb.getAutomaticLocal(0)->asList(&step)->appendLast(&step, h);
                cb.setAutomaticLocal(0, ownB->asObject(&step));
            }
            prev = h;
        }
    }
    ASSERT_GE(forceCycles(sa, &ca, 3), 3u);
    ASSERT_GE(forceCycles(sb, &cb, 3), 3u);
    EXPECT_EQ(g_freed.load(), 0);
    {
        const ProtoList* ownA = ca.getAutomaticLocal(0)->asList(&ca);
        ASSERT_EQ(ownA->getSize(&ca), static_cast<unsigned long>(kLinks));
        for (int i = 0; i < kLinks; ++i) {
            const ProtoObject* h = ownA->getAt(&ca, i);
            ASSERT_NE(h->getAttribute(&ca, probeKey)->asExternalPointer(&ca), nullptr);
            const ProtoObject* n = h->getAttribute(&ca, nextKey);
            if (i + 1 < kLinks || n != PROTO_NONE) {
                ASSERT_NE(n, nullptr);
                ASSERT_NE(n, PROTO_NONE);
                ASSERT_NE(n->getAttribute(&cb, probeKey)->asExternalPointer(&cb), nullptr);
            }
        }
    }

    // Drop both roots.
    ca.setAutomaticLocal(0, nullptr);
    cb.setAutomaticLocal(0, nullptr);
    int rounds = 0;
    while (g_freed.load() < 2 * kLinks && rounds < 20 * kLinks) {
        ASSERT_TRUE(runOneFreshCycle(sa, &ca));
        ASSERT_TRUE(runOneFreshCycle(sb, &cb));
        ++rounds;
    }
    EXPECT_EQ(g_freed.load(), 2 * kLinks);
    EXPECT_GE(rounds, kLinks / 2) << "two live spaces root the states: one link per cycle";
}

// The rule is chosen per cycle: once the second space is destroyed, a chain
// of the remaining space is freed in one cycle again.
TEST(MutableStatesNotRoots, OneSpaceAgainAfterTheOtherIsDestroyed) {
#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    GTEST_SKIP() << "without survivor re-inclusion the table's states stay roots";
#endif
    ProtoSpace sa;
    ProtoContext ca(&sa);
    ca.resizeAutomaticLocals(2);
    g_freed.store(0);
    {
        ProtoSpace sb;
        ProtoContext cb(&sb);
        buildChain(sa, ca, 500, /*keepHead=*/false, 0, 1);
        ASSERT_TRUE(runOneFreshCycle(sa, &ca));
        EXPECT_LT(g_freed.load(), 500) << "two live spaces: the states are roots";
    }
    ASSERT_TRUE(runOneFreshCycle(sa, &ca));
    ASSERT_TRUE(runOneFreshCycle(sa, &ca));
    EXPECT_EQ(g_freed.load(), 500);
}

// A handle of space A, rooted in A, whose state a context of space B wrote:
// the state and everything in it are cells of B.  B's collector must not free
// them while A's handle lives.  The probe makes a free observable at once,
// without depending on the freed memory being reused.
TEST(MutableStatesNotRoots, StateWrittenByAnotherSpaceLivesWhileItsHandleLives) {
    ProtoSpace sa, sb;
    ProtoContext ca(&sa), cb(&sb);
    ca.resizeAutomaticLocals(1);
    g_freed.store(0);
    const ProtoString* probeKey = sym(&ca, "probe");
    {
        ProtoContext build(&sa, &ca, nullptr, nullptr, nullptr, nullptr);
        ca.setAutomaticLocal(0, build.newObject(true));
    }
    {
        ProtoContext writer(&sb, &cb, nullptr, nullptr, nullptr, nullptr);
        ca.getAutomaticLocal(0)->setAttribute(&writer, probeKey, probe(&writer));
    }
    ASSERT_GE(forceCycles(sb, &cb, 3), 3u);
    ASSERT_GE(forceCycles(sa, &ca, 3), 3u);
    EXPECT_EQ(g_freed.load(), 0) << "B freed the state of a live handle of A";
    const ProtoObject* p = ca.getAutomaticLocal(0)->getAttribute(&ca, probeKey);
    ASSERT_NE(p, nullptr);
    ASSERT_NE(p, PROTO_NONE);
    EXPECT_NE(p->asExternalPointer(&ca), nullptr);
}

// The process-wide count of live spaces (ProtoSpace::liveSpaceCount), which a
// runtime uses to check that it created no space of its own.
TEST(MutableStatesNotRoots, LiveSpaceCountFollowsConstructionAndDestruction) {
    ProtoSpace first;
    const proto_ulong base = ProtoSpace::liveSpaceCount();
    ASSERT_GE(base, 1u);
    {
        ProtoSpace second;
        EXPECT_EQ(ProtoSpace::liveSpaceCount(), base + 1);
    }
    EXPECT_EQ(ProtoSpace::liveSpaceCount(), base);
}
