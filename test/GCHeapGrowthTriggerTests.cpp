// GCHeapGrowthTriggerTests.cpp — automatic collection pacing without a heap
// limit.
//
// With no heap limit configured (the default), the allocator must still start
// collection cycles on its own: once the cells handed out since the previous
// cycle exceed the allocation budget — max(PROTOCORE_GC_MIN_BUDGET_CELLS,
// cells retained after that cycle × PROTOCORE_GC_GROWTH_PERCENT / 100) — the
// refill path in ProtoSpace::getFreeCells wakes the GC thread.  A workload
// that allocates garbage with a constant live set must therefore keep the
// heap bounded instead of growing it from the OS forever.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

using namespace proto;

namespace {

// Sets (or, with value == nullptr, unsets) an environment variable for the
// lifetime of the guard and restores the previous state afterwards.  The
// pacing variables are read only by the ProtoSpace constructor.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            hadOld_ = true;
            old_ = old;
        }
        if (value) setenv(name, value, 1);
        else unsetenv(name);
    }
    ~ScopedEnv() {
        if (hadOld_) setenv(name_, old_.c_str(), 1);
        else unsetenv(name_);
    }
private:
    const char* name_;
    bool hadOld_ = false;
    std::string old_;
};

// Allocate `count` non-mutable objects in a short-lived child context of
// `parent`.  Destroying the context submits its young generation to the
// collector, so every object allocated here is garbage.
//
// The loop calls safepoint() every 64 allocations, as an embedder's dispatch
// loop does.  Every allocation in newObject runs inside a critical section,
// where a thread never parks, so without explicit safepoints this thread
// would never let a requested stop-the-world phase begin.
void allocGarbageBatch(ProtoSpace& space, ProtoContext* parent, int count) {
    ProtoContext sub(&space, parent, nullptr, nullptr, nullptr, nullptr);
    for (int j = 0; j < count; ++j) {
        (void) sub.newObject(false);
        if ((j & 0x3F) == 0) sub.safepoint();
    }
}

} // namespace

// C1 regression: without a heap limit, garbage allocation with a constant
// live set starts GC cycles automatically and keeps the heap bounded.  Before
// the allocation-budget trigger existed, no cycle ever started (the collector
// only ran on triggerGC() or under a heap limit), so this workload grew the
// heap by every cell it allocated: 16 million cells (about 976 MiB).
TEST(GCHeapGrowthTriggerTest, GarbageWithoutHeapLimitStartsCyclesAndBoundsHeap) {
    ScopedEnv growth("PROTOCORE_GC_GROWTH_PERCENT", nullptr);   // defaults
    ScopedEnv budget("PROTOCORE_GC_MIN_BUDGET_CELLS", nullptr);
    ProtoSpace space;
    ASSERT_EQ(space.maxHeapSize, 0) << "the test requires no heap limit";
    ProtoContext* root = space.rootContext;

    // Constant live set: 1000 one-element lists pinned through a root set.
    constexpr int kLive = 1000;
    ProtoRootSet* rs = space.createRootSet("heap-growth-live-set");
    ASSERT_NE(rs, nullptr);
    std::vector<ProtoRootSet::Handle> handles;
    handles.reserve(kLive);
    {
        ProtoContext sub(&space, root, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kLive; ++i) {
            const ProtoList* l = sub.newList()->appendLast(&sub, sub.fromInteger(i));
            handles.push_back(rs->add(l->asObject(&sub)));
        }
    }

    const long base = space.heapSize;
    const uint64_t cyclesBefore = space.getGCCycleCount();

    // 400 batches x 20,000 objects = 8,000,000 objects of garbage.  A
    // non-mutable object takes two cells (the object and its attribute map).
    constexpr int kBatches = 400;
    constexpr int kPerBatch = 20000;
    constexpr long kTotalCells = 2L * kBatches * kPerBatch;
    for (int b = 0; b < kBatches; ++b) {
        allocGarbageBatch(space, root, kPerBatch);
    }

    const uint64_t cycles = space.getGCCycleCount() - cyclesBefore;
    const long grown = static_cast<long>(space.heapSize) - base;

    EXPECT_GE(cycles, 3u)
        << "no automatic GC cycles without a heap limit: the collector never "
           "ran while " << kTotalCells << " cells of garbage were allocated";
    // The heap never shrinks, so its final size is its peak.  Bounded means
    // well below the cells allocated; unbounded growth reaches kTotalCells.
    EXPECT_LT(grown, kTotalCells / 4)
        << "heap grew by " << grown << " cells for " << kTotalCells
        << " cells of garbage with a " << kLive << "-object live set";

    // Automatic cycles must not over-collect: the live set is intact.
    for (int i = 0; i < kLive; ++i) {
        const ProtoObject* obj = rs->resolve(handles[i]);
        ASSERT_NE(obj, nullptr);
        const ProtoList* l = obj->asList(root);
        ASSERT_NE(l, nullptr);
        ASSERT_EQ(l->getSize(root), 1u);
        EXPECT_EQ(l->getAt(root, 0)->asLong(root), i);
    }

    for (auto h : handles) rs->remove(h);
    space.destroyRootSet(rs);
}

// PROTOCORE_GC_GROWTH_PERCENT=0 disables automatic cycles: without a heap
// limit, allocation alone then never starts a collection (the behaviour before
// the trigger existed), which embedders can use as an escape hatch.
TEST(GCHeapGrowthTriggerTest, GrowthPercentZeroDisablesAutomaticCycles) {
    ScopedEnv growth("PROTOCORE_GC_GROWTH_PERCENT", "0");
    ScopedEnv budget("PROTOCORE_GC_MIN_BUDGET_CELLS", "65536");
    ProtoSpace space;
    EXPECT_EQ(space.gcGrowthPercent, 0u);
    EXPECT_EQ(space.gcMinBudgetCells, 65536u);

    const uint64_t cyclesBefore = space.getGCCycleCount();
    // 2,000,000 cells: about 30 budgets of 65,536 cells.
    for (int b = 0; b < 50; ++b) {
        allocGarbageBatch(space, space.rootContext, 20000);
    }
    EXPECT_EQ(space.getGCCycleCount() - cyclesBefore, 0u)
        << "a GC cycle started although automatic cycles are disabled";
}

// Cells still owned by a live context's young generation are never candidates
// for reclamation, so a spent budget alone must not start a cycle: with nothing
// submitted since the last snapshot a cycle could only re-scan the young chain
// under stop-the-world and reclaim zero cells.  Once garbage is submitted (a
// short-lived context is destroyed), the pending budget starts a cycle.
TEST(GCHeapGrowthTriggerTest, SpentBudgetWaitsForSubmittedGarbage) {
    ScopedEnv growth("PROTOCORE_GC_GROWTH_PERCENT", nullptr);
    ScopedEnv budget("PROTOCORE_GC_MIN_BUDGET_CELLS", "65536");
    // Keep safepoint() from submitting the keeper's young chain.
    ScopedEnv threshold("PROTOCORE_GC_CONTEXT_THRESHOLD", "4000000000");
    ProtoSpace space;
    ASSERT_EQ(space.gcMinBudgetCells, 65536u);

    ProtoContext keeper(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const uint64_t cyclesStart = space.getGCCycleCount();
    // 3,000,000 cells, about 45 budgets, all retained by the live keeper.
    // The safepoints let a requested cycle stop the world, so a cycle that
    // was requested would run.
    for (int i = 0; i < 1500000; ++i) {
        (void) keeper.newObject(false);
        if ((i & 0x3F) == 0) keeper.safepoint();
    }
    EXPECT_EQ(space.getGCCycleCount() - cyclesStart, 0u)
        << "a cycle started although no garbage had been submitted";

    // Destroying short-lived contexts submits garbage; the spent budget now
    // starts a cycle on a following refill.
    for (int b = 0; b < 100 && space.getGCCycleCount() == cyclesStart; ++b) {
        allocGarbageBatch(space, &keeper, 20000);
    }
    EXPECT_GE(space.getGCCycleCount() - cyclesStart, 1u)
        << "submitted garbage did not start the pending cycle";
}

// Malformed or out-of-range pacing variables fall back to the defaults.
TEST(GCHeapGrowthTriggerTest, InvalidEnvironmentValuesFallBackToDefaults) {
    const char* badGrowth[] = { "", "abc", "50x", "20000" };
    const char* badBudget[] = { "", "abc", "0", "99999999999999" };
    for (int i = 0; i < 4; ++i) {
        ScopedEnv growth("PROTOCORE_GC_GROWTH_PERCENT", badGrowth[i]);
        ScopedEnv budget("PROTOCORE_GC_MIN_BUDGET_CELLS", badBudget[i]);
        ProtoSpace space;
        EXPECT_EQ(space.gcGrowthPercent, ProtoSpace::GC_GROWTH_PERCENT_DEFAULT)
            << "PROTOCORE_GC_GROWTH_PERCENT=\"" << badGrowth[i] << "\"";
        EXPECT_EQ(space.gcMinBudgetCells, ProtoSpace::GC_MIN_BUDGET_CELLS_DEFAULT)
            << "PROTOCORE_GC_MIN_BUDGET_CELLS=\"" << badBudget[i] << "\"";
    }
}

// The allocation budget between cycles is proportional to the cells the
// previous cycle retained, not a fixed amount: with a large retained set,
// collections are spaced by at least retained x growth% cells of allocation,
// so a big live heap does not make the collector run back to back.
TEST(GCHeapGrowthTriggerTest, AllocationBudgetScalesWithRetainedCells) {
    ScopedEnv growth("PROTOCORE_GC_GROWTH_PERCENT", nullptr);   // 100%
    ScopedEnv budget("PROTOCORE_GC_MIN_BUDGET_CELLS", "65536");
    ProtoSpace space;
    ASSERT_EQ(space.gcGrowthPercent, 100u);

    // Retained set: 1,000,000 objects (2,000,000 cells) held by a live
    // context's young generation, which is never submitted while it lives.
    ProtoContext keeper(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    constexpr unsigned long kRetainedCells = 2000000;
    for (unsigned long i = 0; i < kRetainedCells / 2; ++i) {
        (void) keeper.newObject(false);
    }

    // Allocate garbage until two more cycles have started.  The second one
    // can start only after the first completed, and every cycle completed
    // from here on computes its budget with the whole retained set in place.
    const uint64_t cyclesStart = space.getGCCycleCount();
    for (int b = 0; b < 400 && space.getGCCycleCount() - cyclesStart < 2; ++b) {
        allocGarbageBatch(space, &keeper, 20000);
    }
    ASSERT_GE(space.getGCCycleCount() - cyclesStart, 2u)
        << "the allocation-budget trigger did not start cycles";

    unsigned long budgetCells;
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        budgetCells = space.gcAllocationBudget;
    }
    EXPECT_GE(budgetCells, kRetainedCells)
        << "budget " << budgetCells << " cells is not proportional to the "
        << kRetainedCells << " retained cells (growth 100%)";
}
