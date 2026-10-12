// GCMarkStackTests.cpp — the collector's mark work list stays bounded.
//
// The mark phase keeps its grey cells on a stack that lives outside the cell
// heap.  Before 2.14.6 that stack was a std::vector that grew without limit:
// the young-chain walk pushed every reference of every young cell before the
// first pop, and a cell was marked when popped, so a cell referenced N times
// sat on the stack N times.  protoST's `(1 to: 500000) asArray` under a
// 640 MB cell limit pushed 35 million entries (a 512 MB buffer after
// doubling) and peaked at 1.19 GB resident (protoST report
// 2026-10-12-pre-talk-063, section 3).  The main thread's young chain was
// also walked twice, once through its thread and once as the space's
// mainContext.
//
// These tests pin the three properties of the fix (docs/GarbageCollector.md,
// "Mark work list"):
//   * a large young generation does not flood the stack;
//   * each context's young chain is walked once per cycle;
//   * the stack's capacity is a hard bound: when a push finds it full, the
//     overflow rescan still marks exactly the cells a cycle with room marks.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"
#include "../core/MarkStack.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

using namespace proto;

namespace {

// Request one collection cycle and park the calling thread at safepoints
// until it has completed.  Returns false when none completed in time.
bool runOneCycle(ProtoSpace& space, ProtoContext* ctx) {
    const uint64_t start = space.getGCCycleCount();
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space.gcStarted = true;
        space.gcCV.notify_all();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        ctx->safepoint();
        if (space.getGCCycleCount() > start && !space.gcStarted.load()) return true;
        std::this_thread::yield();
    }
    return false;
}

std::size_t chainLength(const Cell* head) {
    std::size_t n = 0;
    for (const Cell* c = head; c; c = c->getNext()) ++n;
    return n;
}

// Restores the default mark stack capacity when a test ends, pass or fail.
struct CapacityGuard {
    ~CapacityGuard() { gcmark::setMarkStackCapacityForTest(0); }
};

}  // namespace

// A context that appends 50,000 integers to a list, one version at a time,
// leaves about 800,000 young cells (every intermediate version's path copy)
// that the collector walks as roots.  The old walk pushed both children of
// every one of them before marking anything (about 1.6 million entries);
// tracing each young cell's references as it is walked keeps the stack at
// the depth of one traversal.
TEST(GCMarkStack, LargeYoungGenerationDoesNotFloodTheStack) {
    ProtoSpace space;
    ProtoContext sub(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoList* list = sub.newList();
    for (int i = 0; i < 50000; ++i) list = list->appendLast(&sub, sub.fromInteger(i));
    ASSERT_GT(chainLength(sub.lastAllocatedCell), 200000u) << "the young generation is large";

    // Park through a child context: its safepoint does not submit sub's
    // young chain, which therefore stays young for the cycle.
    ProtoContext inner(&space, &sub, nullptr, nullptr, nullptr, nullptr);
    ASSERT_TRUE(runOneCycle(space, &inner));

    const gcmark::MarkStats ms = gcmark::lastMarkStats();
    EXPECT_GT(ms.youngCellsWalked, 200000u);
    EXPECT_LT(ms.peakStackEntries, 4096u)
        << "the young walk must not push every young cell's references before marking";
    EXPECT_EQ(list->getSize(&sub), 50000u);
    EXPECT_EQ(list->getAt(&sub, 49999)->asLong(&sub), 49999);
}

// The main thread's root context is both on the thread's context chain and
// the space's mainContext.  Its young chain must be walked once, not once
// per route.
TEST(GCMarkStack, EachYoungChainIsWalkedOnce) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    ASSERT_EQ(space.mainContext, root) << "precondition: the root context is mainContext";
    const ProtoList* list = root->newList();
    for (int i = 0; i < 20000; ++i) list = list->appendLast(root, root->fromInteger(i));

    ProtoContext inner(&space, root, nullptr, nullptr, nullptr, nullptr);
    const std::size_t rootChain = chainLength(root->lastAllocatedCell);
    ASSERT_GT(rootChain, 100000u);
    ASSERT_TRUE(runOneCycle(space, &inner));

    const gcmark::MarkStats ms = gcmark::lastMarkStats();
    EXPECT_GE(ms.youngCellsWalked, rootChain);
    EXPECT_LT(ms.youngCellsWalked, rootChain + rootChain / 10)
        << "the root context's young chain was walked more than once";
    EXPECT_EQ(list->getAt(root, 19999)->asLong(root), 19999);
}

// With a mark stack far too small for the structure, pushes fail and the
// collector rescans.  The cycle must still mark exactly the cells a cycle
// with room marks, and every element must survive later cycles intact.
TEST(GCMarkStack, OverflowRescanMarksTheSameLiveSet) {
    CapacityGuard guard;
    ProtoSpace space;
    ProtoRootSet* rs = space.createRootSet("GCMarkStack.overflow");
    {
        ProtoContext build(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* list = build.newList();
        for (int i = 0; i < 20000; ++i) {
            const ProtoList* element = build.newList()->appendLast(&build, build.fromInteger(i));
            list = list->appendLast(&build, element->asObject(&build));
        }
        rs->add(list->asObject(&build));
    }  // the builder's young chain is submitted: its cells are candidates now

    ProtoContext inner(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    // Two cycles with the default capacity: the first frees the builder's
    // garbage, the second measures the live set.
    ASSERT_TRUE(runOneCycle(space, &inner));
    ASSERT_TRUE(runOneCycle(space, &inner));
    const gcmark::MarkStats roomy = gcmark::lastMarkStats();
    EXPECT_EQ(roomy.overflowRescans, 0u);

    gcmark::setMarkStackCapacityForTest(16);
    ASSERT_TRUE(runOneCycle(space, &inner));
    const gcmark::MarkStats tight = gcmark::lastMarkStats();
    EXPECT_EQ(tight.stackCapacity, 16u);
    EXPECT_LE(tight.peakStackEntries, 16u);
    EXPECT_GT(tight.overflowRescans, 0u) << "a 16-entry stack must overflow on this structure";
    EXPECT_EQ(tight.markedCells, roomy.markedCells)
        << "the overflow rescan must mark the same live set";

    // A few more tight cycles with fresh garbage in between, so a cell freed
    // by mistake would be reused and its contents overwritten.
    for (int round = 0; round < 3; ++round) {
        {
            ProtoContext garbage(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
            const ProtoList* g = garbage.newList();
            for (int i = 0; i < 20000; ++i) g = g->appendLast(&garbage, garbage.fromInteger(-i));
        }
        ASSERT_TRUE(runOneCycle(space, &inner));
    }

    struct Found { ProtoContext* ctx; const ProtoList* list; } found{&inner, nullptr};
    rs->forEachRoot([](void* u, const ProtoObject* obj) {
        auto* f = static_cast<Found*>(u);
        f->list = obj->asList(f->ctx);
    }, &found);
    const ProtoList* list = found.list;
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->getSize(&inner), 20000u);
    for (int i = 0; i < 20000; ++i) {
        const ProtoList* element = list->getAt(&inner, i)->asList(&inner);
        ASSERT_NE(element, nullptr) << "element " << i;
        ASSERT_EQ(element->getSize(&inner), 1u) << "element " << i;
        ASSERT_EQ(element->getAt(&inner, 0)->asLong(&inner), i) << "element " << i;
    }
    space.destroyRootSet(rs);
}
