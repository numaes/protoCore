// AdaptiveHeapTests.cpp -- the adaptive heap controller on real spaces.
//
// Design: docs/specs/2026-10-02-adaptive-heap-controller-design.md, section 4.
// The control law and the limit detection have deterministic unit tests in
// AdaptiveHeapControlTests.cpp; these cases run the controller end to end:
//
//   * configuration: defaults, the environment, PROTOCORE_ADAPTIVE_HEAP=0,
//     setHeapLimits disabling it;
//   * a cycle starts when the heap reaches S even when no thread waits;
//   * steady workload: the heap follows S, far below H;
//   * storm workload: S rises under stall (and, in the clock-dependent
//     PressureFalls case, pressure then falls below p_high);
//   * true out-of-memory: the callback, then the controlled abort;
//   * not out-of-memory: a retained set at 60 % of H with heavy garbage;
//   * multi-space: H is a process budget.
//
// Every test restores the environment variables it changes.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../core/AdaptiveHeap.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
static int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
static int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

using namespace proto;

namespace {

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            hadValue_ = true;
            oldValue_ = old;
        }
        if (value) ::setenv(name, value, 1);
        else ::unsetenv(name);
    }
    ~ScopedEnv() {
        if (hadValue_) ::setenv(name_, oldValue_.c_str(), 1);
        else ::unsetenv(name_);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    bool hadValue_ = false;
    std::string oldValue_;
};

// The environment of every case: none of the controller's variables set.
struct CleanEnv {
    ScopedEnv limit{"PROTOCORE_HEAP_LIMIT_CELLS", nullptr};
    ScopedEnv off{"PROTOCORE_ADAPTIVE_HEAP", nullptr};
    ScopedEnv trace{"PROTOCORE_HEAP_TRACE", nullptr};
};

// `count` objects of garbage through short-lived contexts of 100 objects, a
// "call" each: destroying a context submits its young generation, so
// everything is reclaimable.  A safepoint follows every call, as a runtime
// places one between bytecodes, so a requested cycle can stop the world
// without waiting for this thread to run out of memory.
//
// `work` adds that many iterations of non-allocating arithmetic per object,
// standing for the interpretation between allocations: 0 allocates as fast
// as the allocator goes (several GB/s), a rate no interpreter reaches.
// Returns the number of cells allocated.
volatile std::uint64_t g_sink = 0;
//
// Without `safepoints` the thread reaches none: a requested cycle can stop
// the world only once this thread waits for memory, so every cycle stalls it
// for the cycle's whole length, whatever the relative speed of the mutator
// and the collector (a sanitizer slows them differently).
proto_ulong churn(ProtoSpace& space, int count, int work = 0, bool safepoints = true) {
    proto_ulong cells = 0;
    for (int done = 0; done < count; done += 100) {
        {
            ProtoContext sub(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
            for (int j = 0; j < 100; ++j) {
                (void) sub.newObject(false);
                std::uint64_t x = static_cast<std::uint64_t>(j);
                for (int w = 0; w < work; ++w) x = x * 2862933555777941757ULL + 3037000493ULL;
                g_sink = g_sink + x;
            }
            cells += sub.allocatedCellsCount;
        }
        if (safepoints) space.rootContext->safepoint();
    }
    return cells;
}

// A live set reachable from a root set: a list of `elements` objects, built
// in chunks so the path copies of the build are reclaimable as it goes.
// About two cells per element stay live (the object and its list node).
struct RootedList {
    ProtoRootSet* rs = nullptr;
    ProtoRootSet::Handle h = ProtoRootSet::kNullHandle;

    RootedList(ProtoSpace& space, int elements) {
        rs = space.createRootSet("adaptive-heap-test");
        h = rs->add(space.rootContext->newList()->asObject(space.rootContext));
        int done = 0;
        while (done < elements) {
            ProtoContext sub(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
            const ProtoList* list = rs->resolve(h)->asList(&sub);
            const int chunk = std::min(10000, elements - done);
            for (int i = 0; i < chunk; ++i) list = list->appendLast(&sub, sub.newObject(false));
            rs->remove(h);
            h = rs->add(list->asObject(&sub));
            done += chunk;
            space.rootContext->safepoint();
        }
    }
    proto_ulong size(ProtoSpace& space) const {
        return rs->resolve(h)->asList(space.rootContext)->getSize(space.rootContext);
    }
};

// The space's fields are written by the collector thread too (its own
// allocations, the controller at the end of a cycle): read them the way the
// library does outside globalMutex, or through the locked snapshot.
int heapOf(ProtoSpace& space) { return relaxedLoad(space.heapSize); }
int ceilingOf(ProtoSpace& space) { return relaxedLoad(space.maxHeapSize); }
proto_ulong softOf(ProtoSpace& space) { return space.adaptiveHeapStats().softCells; }

// Polls `cond` for up to `seconds`; a hang detector, not a measurement.
template <class F>
bool eventually(F cond, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

}  // namespace

// --- Configuration -----------------------------------------------------------

TEST(AdaptiveHeap, DisabledByDefault) {
    CleanEnv env;
    ProtoSpace space;
    const AdaptiveHeapStats s = space.adaptiveHeapStats();
    EXPECT_FALSE(s.enabled);
    EXPECT_EQ(ceilingOf(space), 0) << "protoCore alone sets no limit";
}

TEST(AdaptiveHeap, EnableAppliesTheDefaults) {
    CleanEnv env;
    ProtoSpace space;
    space.enableAdaptiveHeap();
    const AdaptiveHeapStats s = space.adaptiveHeapStats();
    const proto_ulong H = adaptive::defaultHardCells(adaptive::physicalMemoryBytes(),
                                                     adaptive::processMemoryLimitBytes());
    EXPECT_TRUE(s.enabled);
    EXPECT_EQ(s.hardCells, H);
    EXPECT_EQ(s.softCells, std::min<proto_ulong>(adaptive::kDefaultInitialSoftCells, H));
    EXPECT_EQ(softOf(space), s.softCells);
    EXPECT_GT(ceilingOf(space), 0);
    EXPECT_LE(static_cast<proto_ulong>(ceilingOf(space)), H);
}

TEST(AdaptiveHeap, ConfigurationIsHonoured) {
    CleanEnv env;
    ProtoSpace space;
    AdaptiveHeapConfig c;
    c.hardCells = 5000000;
    c.initialSoftCells = 700000;
    space.enableAdaptiveHeap(c);
    const AdaptiveHeapStats s = space.adaptiveHeapStats();
    EXPECT_EQ(s.hardCells, 5000000u);
    EXPECT_EQ(s.softCells, 700000u);
    EXPECT_EQ(ceilingOf(space), 5000000);
}

TEST(AdaptiveHeap, HardLimitIsClampedToIntMax) {
    CleanEnv env;
    ProtoSpace space;
    AdaptiveHeapConfig c;
    c.hardCells = static_cast<proto_ulong>(INT_MAX) * 4;
    space.enableAdaptiveHeap(c);
    EXPECT_EQ(space.adaptiveHeapStats().hardCells, static_cast<proto_ulong>(INT_MAX));
}

TEST(AdaptiveHeap, EnvironmentOverridesHardAndInitialSoft) {
    CleanEnv env;
    ScopedEnv limit("PROTOCORE_HEAP_LIMIT_CELLS", "600000,9000000");
    ProtoSpace space;   // the constructor applies them as fixed limits
    EXPECT_EQ(ceilingOf(space), 9000000);
    AdaptiveHeapConfig c;
    c.hardCells = 5000000;  // the environment wins over the embedder's value
    space.enableAdaptiveHeap(c);
    const AdaptiveHeapStats s = space.adaptiveHeapStats();
    EXPECT_TRUE(s.enabled);
    EXPECT_EQ(s.hardCells, 9000000u);
    EXPECT_EQ(s.softCells, 600000u);
}

TEST(AdaptiveHeap, AdaptiveOffAppliesTheHardLimitAsAFixedOne) {
    CleanEnv env;
    ScopedEnv off("PROTOCORE_ADAPTIVE_HEAP", "0");
    ProtoSpace space;
    AdaptiveHeapConfig c;
    c.hardCells = 4000000;
    space.enableAdaptiveHeap(c);
    EXPECT_FALSE(space.adaptiveHeapStats().enabled);
    EXPECT_EQ(ceilingOf(space), 4000000);
    EXPECT_EQ(softOf(space), 0u);
}

TEST(AdaptiveHeap, SetHeapLimitsDisablesTheController) {
    CleanEnv env;
    ProtoSpace space;
    space.enableAdaptiveHeap();
    space.setHeapLimits(0, 3000000);
    EXPECT_FALSE(space.adaptiveHeapStats().enabled);
    EXPECT_EQ(softOf(space), 0u);
    EXPECT_EQ(ceilingOf(space), 3000000);
    // Cycles no longer touch the soft limit.
    const uint64_t c0 = space.getGCCycleCount();
    for (int i = 0; i < 100 && space.getGCCycleCount() < c0 + 3; ++i) {
        churn(space, 20000);
        space.triggerGC();
    }
    EXPECT_EQ(softOf(space), 0u);
    EXPECT_EQ(ceilingOf(space), 3000000);
}

// PROTOCORE_ADAPTIVE_HEAP=1, the diagnostic switch: a space runs the
// controller from its creation, and an embedder's fixed limits do not turn
// it off, so an existing binary can be measured under the controller.
TEST(AdaptiveHeap, AdaptiveOnEnablesTheControllerAtCreation) {
    CleanEnv env;
    ScopedEnv on("PROTOCORE_ADAPTIVE_HEAP", "1");
    ProtoSpace space;
    AdaptiveHeapStats s = space.adaptiveHeapStats();
    EXPECT_TRUE(s.enabled);
    EXPECT_EQ(s.softCells, static_cast<proto_ulong>(adaptive::kDefaultInitialSoftCells));
    space.setHeapLimits(0, 3000000);   // what a runtime does today
    s = space.adaptiveHeapStats();
    EXPECT_TRUE(s.enabled) << "setHeapLimits disabled the forced controller";
    EXPECT_EQ(s.softCells, static_cast<proto_ulong>(adaptive::kDefaultInitialSoftCells));
    EXPECT_NE(ceilingOf(space), 3000000);
}

// With the switch, PROTOCORE_HEAP_LIMIT_CELLS still gives H and S0.
TEST(AdaptiveHeap, AdaptiveOnHonoursTheLimitVariable) {
    CleanEnv env;
    ScopedEnv on("PROTOCORE_ADAPTIVE_HEAP", "1");
    ScopedEnv limit("PROTOCORE_HEAP_LIMIT_CELLS", "700000,8000000");
    ProtoSpace space;
    const AdaptiveHeapStats s = space.adaptiveHeapStats();
    EXPECT_TRUE(s.enabled);
    EXPECT_EQ(s.hardCells, 8000000u);
    EXPECT_EQ(s.softCells, 700000u);
}

// An embedder that knows its run fits starts at the budget (decision 2 of
// the collector-throughput spec): the controller never grows there on
// speculation.
TEST(AdaptiveHeap, StartAtTheBudgetByConfigurationOrEnvironment) {
    CleanEnv env;
    {
        ProtoSpace space;
        AdaptiveHeapConfig c;
        c.hardCells = 5000000;
        c.initialSoftCells = 5000000;
        space.enableAdaptiveHeap(c);
        EXPECT_EQ(space.adaptiveHeapStats().softCells, 5000000u);
    }
    {
        ScopedEnv start("PROTOCORE_ADAPTIVE_HEAP_START", "budget");
        ProtoSpace space;
        AdaptiveHeapConfig c;
        c.hardCells = 6000000;
        space.enableAdaptiveHeap(c);
        EXPECT_EQ(space.adaptiveHeapStats().softCells, 6000000u);
    }
    {
        ScopedEnv start("PROTOCORE_ADAPTIVE_HEAP_START", "nonsense");
        ProtoSpace space;
        AdaptiveHeapConfig c;
        c.hardCells = 6000000;
        space.enableAdaptiveHeap(c);
        EXPECT_EQ(space.adaptiveHeapStats().softCells, adaptive::kDefaultInitialSoftCells);
    }
}

// Clock-dependent (CLOCK_DEPENDENT_TESTS), property P1 of the spec: a steady
// allocator below the collector's capacity, a generous budget.  Once the
// law and pacing have converged the mutator does not wait for headroom.
// Asserts counts, but whether the collector keeps up is the machine's speed.
TEST(AdaptiveHeapRate, SteadyAllocatorBelowCapacityStopsWaiting) {
    CleanEnv env;
    ProtoSpace space;
    AdaptiveHeapConfig c;
    c.hardCells = 16000000;   // 1 GB
    space.enableAdaptiveHeap(c);
    RootedList live(space, 100000);
    churn(space, 8000000, 200);   // converge
    adaptive::WaitStats before;
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        before = adaptive::waitStats(&space);
    }
    churn(space, 8000000, 200);
    adaptive::WaitStats after;
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        after = adaptive::waitStats(&space);
    }
    const AdaptiveHeapStats s = space.adaptiveHeapStats();
    std::printf("[ rate ] S=%lu L=%lu cycles=%llu waits %llu -> %llu w=%.4f\n",
                (unsigned long) s.softCells, (unsigned long) s.liveCellsLastCycle,
                (unsigned long long) s.cycles, (unsigned long long) before.waits,
                (unsigned long long) after.waits, s.lastPressure);
    ASSERT_GT(after.cyclesCompleted, before.cyclesCompleted) << "no cycle in the measured window";
    EXPECT_EQ(after.waits - before.waits, 0u) << "the mutator still waited after convergence";
    EXPECT_EQ(live.size(space), 100000u);
}

// --- Cycle request at S ------------------------------------------------------

// A thread inside a critical section never waits in the soft zone, so before
// the controller's request it could take the heap past S without any cycle
// starting.  Now the growth that reaches S requests one.
TEST(AdaptiveHeap, CycleStartsAtSoftLimitWithoutAWaitingThread) {
    CleanEnv env;
    ProtoSpace space;
    AdaptiveHeapConfig c;
    c.initialSoftCells = static_cast<proto_ulong>(heapOf(space)) + 50000;
    space.enableAdaptiveHeap(c);
    const uint64_t c0 = space.getGCCycleCount();
    {
        ProtoContext work(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
        ProtoContext::CriticalSection cs(&work);
        const int target = static_cast<int>(softOf(space)) + 300000;
        while (heapOf(space) < target) (void) work.newObject(false);
    }
    // The thread reaches safepoints, as a runtime's interpreter loop does, so
    // a requested cycle can stop the world; nothing here waits for memory.
    EXPECT_TRUE(eventually([&] {
        space.rootContext->safepoint();
        return space.getGCCycleCount() > c0;
    }, 30))
        << "no cycle started although the heap passed S (" << softOf(space)
        << " cells) at " << heapOf(space) << " cells";
}

// --- Steady workload ---------------------------------------------------------

// A small live set and a lot of garbage at an interpreter-like rate (some
// work between allocations).
namespace {
struct SteadyRun {
    AdaptiveHeapStats stats;
    proto_ulong garbageCells = 0;
    proto_ulong liveElements = 0;
    bool softNeverDecreased = true;
};

SteadyRun runSteady(ProtoSpace& space) {
    // An explicit budget: since 2.12 the law may use the budget where the
    // mutators wait, and the default one is 75 % of physical memory.
    AdaptiveHeapConfig c;
    c.hardCells = 16000000;   // 1 GB
    space.enableAdaptiveHeap(c);
    RootedList live(space, 100000);   // ~300,000 live cells
    constexpr int kRounds = 200;
    constexpr int kPerRound = 50000;   // 10,000,000 objects of garbage in all
    SteadyRun run;
    proto_ulong lastSoft = 0;
    for (int r = 0; r < kRounds; ++r) {
        run.garbageCells += churn(space, kPerRound, 50);
        const proto_ulong soft = space.adaptiveHeapStats().softCells;
        if (soft < lastSoft) run.softNeverDecreased = false;
        lastSoft = soft;
    }
    run.stats = space.adaptiveHeapStats();
    run.liveElements = live.size(space);
    const AdaptiveHeapStats& s = run.stats;
    std::printf("[ steady ] L=%lu S=%lu heap=%lu cycles=%llu p=%.4f H=%lu garbage=%lu\n",
                (unsigned long) s.liveCellsLastCycle, (unsigned long) s.softCells,
                (unsigned long) s.heapCells, (unsigned long long) s.cycles,
                s.lastPressure, (unsigned long) s.hardCells, (unsigned long) run.garbageCells);
    return run;
}
}  // namespace

// Gating, clock-free invariants: the live set survives, cycles run, S never
// decreases and never exceeds the budget, and the heap follows S.
TEST(AdaptiveHeap, SteadyWorkloadHeapFollowsTheSoftLimit) {
    CleanEnv env;
    ProtoSpace space;
    const SteadyRun run = runSteady(space);
    const AdaptiveHeapStats& s = run.stats;
    EXPECT_EQ(run.liveElements, 100000u) << "the live set did not survive";
    EXPECT_GT(s.cycles, 0u);
    EXPECT_TRUE(run.softNeverDecreased);
    EXPECT_LE(s.softCells, s.hardCells);
    // The heap is S plus what grew while cycles ran (16 MiB blocks).
    EXPECT_LT(s.heapCells, s.softCells + 4000000u);
}

// Clock-dependent (listed in CLOCK_DEPENDENT_TESTS): how far the heap grows
// past S while cycles run depends on the allocation rate against the
// collector's speed.  The heap ends far below the garbage volume.  2.10.0
// asserted a quarter of it and was flaky: S itself ran to 1-5 M cells of 20 M.
// 2.10.1 capped S at max(S0, 8 x L), 2.4 M cells here (heap 2.71 M cells,
// 13.6 %, in ten runs on Linux x86-64).  Since 2.13.0 S grows only while the
// mutator waits and the growth reduces the waits; with pacing this workload
// rarely waits, and the heap measured 2.1 M cells.  Half the garbage volume
// leaves room for a slow collector (sanitizers, loaded CI machines) without
// losing the point of the check.
TEST(AdaptiveHeapSteady, HeapStaysFarBelowTheGarbageVolume) {
    CleanEnv env;
    ProtoSpace space;
    const SteadyRun run = runSteady(space);
    EXPECT_LT(run.stats.heapCells, run.garbageCells / 2)
        << run.garbageCells << " cells allocated";
}

// --- Fast allocator ----------------------------------------------------------

namespace {
// The bound of P5 (collector-throughput spec, section 9): S changes at most
// log2(B / S0) times by doubling, plus one regime-1 jump, plus re-arms.
std::uint64_t changeBound(proto_ulong budget, proto_ulong s0, std::uint64_t rearms) {
    return static_cast<std::uint64_t>(std::ceil(std::log2(
               static_cast<double>(budget) / static_cast<double>(std::max<proto_ulong>(1, s0)))))
           + 1 + rearms;
}

adaptive::LawState lawOf(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    return adaptive::lawState(&space);
}
}  // namespace

// A large live set and garbage allocated as fast as the allocator goes, the
// case that took 2.10.0's soft limit to about 20 x the live set.  Since 2.12
// the law grows S only while the growth reduces the mutators' waits, within
// the budget.  Gating, on invariants that hold whatever the machine's speed:
// S never decreases, never exceeds B, the live set survives, and the heap
// passes S only by what grows while a cycle runs.  How many times S changed
// depends on the measured samples (a regime-1 target is recomputed from
// noisy r and T at every cycle), so it is printed, not asserted here; the
// bound on changes is asserted on the pure law
// (AdaptiveHeapLaw.NeverDecreasesNeverExceedsBAndConvergesBoundedly).
TEST(AdaptiveHeapFastAllocator, SoftLimitGrowthIsBoundedByTheBudgetAndTheProbes) {
    CleanEnv env;
    ProtoSpace space;
    AdaptiveHeapConfig c;
    c.hardCells = 32000000;   // 2 GB
    space.enableAdaptiveHeap(c);
    RootedList live(space, 400000);   // ~800,000 live cells (object + node)
    proto_ulong garbage = 0;
    proto_ulong lastSoft = 0;
    bool neverDecreased = true;
    for (int r = 0; r < 60; ++r) {
        garbage += churn(space, 500000, 0);   // 30,000,000 objects in all
        const proto_ulong soft = space.adaptiveHeapStats().softCells;
        if (soft < lastSoft) neverDecreased = false;
        lastSoft = soft;
    }
    const AdaptiveHeapStats s = space.adaptiveHeapStats();
    const adaptive::LawState law = lawOf(space);
    std::printf("[ fast ] L=%lu S=%lu heap=%lu cycles=%llu w=%.4f changes=%llu probes=%llu "
                "stopped=%d garbage=%lu\n",
                (unsigned long) s.liveCellsLastCycle, (unsigned long) s.softCells,
                (unsigned long) s.heapCells, (unsigned long long) s.cycles, s.lastPressure,
                (unsigned long long) law.changes, (unsigned long long) law.probes,
                law.stopped ? 1 : 0, (unsigned long) garbage);
    EXPECT_EQ(live.size(space), 400000u) << "the live set did not survive";
    ASSERT_GT(s.cycles, 0u);
    EXPECT_TRUE(neverDecreased);
    EXPECT_LE(s.softCells, s.hardCells);
    std::printf("[ fast ] changes %llu against the pure law's bound %llu (not asserted: sample noise)\n",
                (unsigned long long) law.changes,
                (unsigned long long) changeBound(s.hardCells, adaptive::kDefaultInitialSoftCells, law.rearms));
    // What grows while cycles run: a few 16 MiB blocks.
    EXPECT_LT(s.heapCells, s.softCells + 4000000u);
}

// --- Storm workload ----------------------------------------------------------

namespace {
struct StormRun {
    proto_ulong initialSoft = 0;
    proto_ulong finalSoft = 0;
    proto_ulong budget = 0;
    double maxWait = 0.0;
    uint64_t cycles = 0;
    uint64_t lastChangeAt = 0;   // the cycle after which S last changed
    adaptive::LawState law;
};

// A large live set and a mutator that allocates as fast as it can.  With
// `safepoints` false every cycle is a stall (see churn).
StormRun runStorm(ProtoSpace& space, int maxRounds, bool safepoints) {
    AdaptiveHeapConfig c;
    c.hardCells = 32000000;   // 2 GB
    space.enableAdaptiveHeap(c);
    StormRun run;
    // S before the live set is built: on a slow build (MSVC Debug) the
    // build's own cycles may already have grown it.
    run.initialSoft = space.adaptiveHeapStats().softCells;
    RootedList live(space, 300000);   // ~900,000 live cells
    proto_ulong soft = run.initialSoft;
    uint64_t seen = space.adaptiveHeapStats().cycles;
    for (int r = 0; r < maxRounds; ++r) {
        churn(space, 100000, 0, safepoints);
        const AdaptiveHeapStats s = space.adaptiveHeapStats();
        if (s.cycles != seen) {
            seen = s.cycles;
            run.maxWait = std::max(run.maxWait, s.lastPressure);
            if (s.softCells != soft) {
                soft = s.softCells;
                run.lastChangeAt = s.cycles;
            }
        }
    }
    const AdaptiveHeapStats s = space.adaptiveHeapStats();
    // The last cycle too: one that completed during the last round is not
    // seen by the loop.
    run.maxWait = std::max(run.maxWait, s.lastPressure);
    run.finalSoft = s.softCells;
    run.budget = s.hardCells;
    run.cycles = s.cycles;
    run.law = lawOf(space);
    std::printf("[ storm ] L=%lu S=%lu heap=%lu cycles=%llu maxw=%.3f lastw=%.4f "
                "last-change-at=%llu changes=%llu probes=%llu stopped=%d\n",
                (unsigned long) s.liveCellsLastCycle, (unsigned long) s.softCells,
                (unsigned long) s.heapCells, (unsigned long long) s.cycles,
                run.maxWait, s.lastPressure, (unsigned long long) run.lastChangeAt,
                (unsigned long long) run.law.changes, (unsigned long long) run.law.probes,
                run.law.stopped ? 1 : 0);
    return run;
}
}  // namespace

// Gating: under a storm the mutator waits for every cycle (it reaches no
// safepoint), and the law grows S: the regime-1 headroom or a probe.  The
// verdict does not depend on the machine's speed.
TEST(AdaptiveHeapStorm, SoftLimitRisesUnderStall) {
    CleanEnv env;
    ProtoSpace space;
    // 30 M objects: since 2.14.2 the live-set floor raises S above twice the
    // live set from the first cycle, and 6 M (the storm of 2.10-2.14.1) then
    // fitted under S, so no cycle ran during the storm and nothing stalled.
    const StormRun run = runStorm(space, 300, false);
    EXPECT_GT(run.maxWait, 0.0) << "the storm produced no wait";
    EXPECT_GT(run.finalSoft, run.initialSoft) << "S never grew under the stall";
    EXPECT_LE(run.finalSoft, run.budget);
}

// Clock-dependent (listed in CLOCK_DEPENDENT_TESTS): with safepoints, S
// settles within a bounded number of changes and cycles -- the waits vanish
// (the headroom was what stalled the mutator) or two probes that did not
// reduce them stop the growth (the collector's throughput is what stalls it,
// which no soft limit removes).  Which one happens, and when, depends on the
// relative speed of the mutator and the collector, hence not a gate.
TEST(AdaptiveHeapStorm, SoftLimitSettlesWithinBoundedCycles) {
    CleanEnv env;
    ProtoSpace space;
    const StormRun run = runStorm(space, 3000, true);
    EXPECT_LE(run.law.changes,
              changeBound(run.budget, adaptive::kDefaultInitialSoftCells, run.law.rearms));
    EXPECT_LE(run.lastChangeAt, 60u) << "S was still changing after 60 cycles";
    EXPECT_LE(run.finalSoft, run.budget);
}

// --- Out of memory -----------------------------------------------------------

static ProtoObject* adaptiveOomMarker(ProtoContext*) {
    std::fprintf(stderr, "OOM_CALLBACK_FIRED ");
    std::fflush(stderr);
    return nullptr;
}

// A retained set larger than H: the callback, then the controlled abort with
// the controller's message.
TEST(AdaptiveHeapDeathTest, RetainedSetLargerThanHIsOutOfMemory) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_DEATH({
        CleanEnv env;
        ProtoSpace space;
        space.outOfMemoryCallback = adaptiveOomMarker;
        AdaptiveHeapConfig c;
        c.hardCells = 1500000;   // 96 MB
        space.enableAdaptiveHeap(c);
        // Every context stays alive: its young generation is a root, so the
        // retained set grows without bound.
        std::vector<ProtoContext*> retained;
        for (;;) {
            ProtoContext* ctx = new ProtoContext(&space, space.rootContext,
                                                 nullptr, nullptr, nullptr, nullptr);
            retained.push_back(ctx);
            for (int j = 0; j < 5000; ++j) (void) ctx->newObject(false);
        }
    }, "OOM_CALLBACK_FIRED protoCore: adaptive heap hard limit");
}

// A retained set at 60 % of H with heavy garbage completes: the data fits.
TEST(AdaptiveHeap, RetainedSetAtSixtyPercentOfHWithHeavyGarbageCompletes) {
    CleanEnv env;
    ProtoSpace space;
    const int H = 3000000;
    AdaptiveHeapConfig c;
    c.hardCells = H;
    space.enableAdaptiveHeap(c);
    // Cells, not objects: an object takes more than one cell.
    ProtoContext keep(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    while (keep.allocatedCellsCount < static_cast<proto_ulong>(H) * 6 / 10)
        (void) keep.newObject(false);
    for (int r = 0; r < 300; ++r) churn(space, 50000);   // 15,000,000 cells
    EXPECT_LE(heapOf(space), H + 300000) << "heap far past H";
    EXPECT_GT(space.adaptiveHeapStats().cycles, 0u);
}

// --- Start-up live set near S0 (2.14.2) -------------------------------------------

// The pattern of protoST under the controller (2026-10-04 re-measurement,
// fib.st 3.4 times slower): the program's start-up live set sits just below
// the initial soft limit, so a cycle starts at once and frees little, and S
// stayed at S0 after it (the law skips the first cycle; later ones grow S
// only on waits), so the next cycle followed after a thin slice of
// allocation.  Since 2.14.2 a cycle that reclaimed less than it marked, the
// first included, leaves S >= min(H, 2 L + runway).  Gating: L, S and the
// cycle's reclamation are read together under globalMutex after every chunk
// of garbage; at least one such cycle must be seen.
TEST(AdaptiveHeapStartUp, ACycleThatReclaimsLessThanItMarksLeavesSAtLeastTwiceTheLiveSet) {
    CleanEnv env;
    ProtoSpace space;
    // The controller from the start, S0 = 600,000 cells.
    AdaptiveHeapConfig c;
    c.hardCells = 16000000;   // 1 GB
    c.initialSoftCells = 600000;
    space.enableAdaptiveHeap(c);
    const proto_ulong s0 = softOf(space);
    int observed = 0;
    int futile = 0;
    std::uint64_t firstChecked = 0;
    auto check = [&](const char* phase) {
        AdaptiveHeapStats st;
        proto_ulong reclaimed = 0;
        {
            // L, S and the cycle's reclamation are published together under
            // globalMutex at the cycle end.
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            st = space.adaptiveHeapStats();
            reclaimed = space.reclaimedLastCycle.load();
        }
        if (st.cycles == 0) return true;
        if (observed++ == 0) firstChecked = st.cycles;
        if (reclaimed >= st.liveCellsLastCycle) return true;
        ++futile;   // a cycle that reclaimed less than it marked
        const proto_ulong floor = std::min<proto_ulong>(st.hardCells, 2 * st.liveCellsLastCycle);
        EXPECT_GE(st.softCells, floor)
            << phase << ": after " << st.cycles << " cycle(s) that reclaimed " << reclaimed
            << " cells, S = " << st.softCells << " cells is below twice the live set (L = "
            << st.liveCellsLastCycle << ")";
        return st.softCells >= floor;
    };
    // Start-up: 700,000 objects, each rooted in a root set (whose slots are
    // not cells), so the start-up allocates almost nothing but live cells and
    // the first cycle, at S0, finds almost no garbage.
    constexpr int kLiveObjects = 700000;
    ProtoRootSet* rs = space.createRootSet("start-up-live-set");
    for (int done = 0; done < kLiveObjects; done += 1000) {
        {
            ProtoContext sub(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
            for (int i = 0; i < 1000; ++i) (void) rs->add(sub.newObject(false));
        }
        space.rootContext->safepoint();
        ASSERT_TRUE(check("start-up"));
    }
    // The program proper: garbage at an interpreter-like rate.
    proto_ulong garbage = 0;
    for (int r = 0; r < 300; ++r) {
        garbage += churn(space, 10000, 20);
        ASSERT_TRUE(check("run"));
    }
    const AdaptiveHeapStats st = space.adaptiveHeapStats();
    std::printf("[ start-up ] S0=%lu L=%lu S=%lu cycles=%llu first-checked=%llu futile-seen=%d garbage=%lu\n",
                (unsigned long) s0, (unsigned long) st.liveCellsLastCycle,
                (unsigned long) st.softCells, (unsigned long long) st.cycles,
                (unsigned long long) firstChecked, futile, (unsigned long) garbage);
    EXPECT_GT(observed, 0);
    EXPECT_GT(futile, 0) << "no cycle reclaimed less than it marked: the case does not reproduce the pattern";
    EXPECT_GT(st.liveCellsLastCycle, s0 / 2)
        << "the live set is not near S0: the case does not reproduce the pattern";
}

// --- Several spaces ----------------------------------------------------------

// H is a process budget: a space's growth is limited by what the others
// hold, and each space keeps its own soft limit.
TEST(AdaptiveHeap, SeveralSpacesShareTheProcessBudget) {
    CleanEnv env;
    const int H = 3000000;
    ProtoSpace a;
    ProtoSpace b;
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        (void) adaptive::processHeapPeakCells(true);
    }
    AdaptiveHeapConfig c;
    c.hardCells = H;
    a.enableAdaptiveHeap(c);
    b.enableAdaptiveHeap(c);

    // A retains ~55 % of the budget.
    ProtoContext keepA(&a, a.rootContext, nullptr, nullptr, nullptr, nullptr);
    while (keepA.allocatedCellsCount < static_cast<proto_ulong>(H) * 55 / 100)
        (void) keepA.newObject(false);

    // B churns far more than what is left; it must live within it.
    for (int r = 0; r < 200; ++r) {
        churn(b, 50000);
        ASSERT_LE(static_cast<long long>(heapOf(a)) + heapOf(b), H + 400000LL)
            << "the process budget was exceeded at round " << r;
    }
    // A churns too, within its own share.
    for (int r = 0; r < 50; ++r) churn(a, 50000);
    EXPECT_LE(static_cast<long long>(heapOf(a)) + heapOf(b), H + 400000LL);

    // P2 at every heap growth, not only where the loop sampled it.  The
    // slack is the bounded overshoot of a refill inside a critical section.
    long long peak = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        peak = adaptive::processHeapPeakCells(false);
    }
    EXPECT_LE(peak, H + 400000LL) << "the process budget was exceeded at a heap growth";
    const AdaptiveHeapStats sa = a.adaptiveHeapStats();
    const AdaptiveHeapStats sb = b.adaptiveHeapStats();
    EXPECT_TRUE(sa.enabled);
    EXPECT_TRUE(sb.enabled);
    EXPECT_GT(sb.cycles, 0u);
    EXPECT_EQ(sa.hardCells, static_cast<proto_ulong>(H));
    EXPECT_EQ(sb.hardCells, static_cast<proto_ulong>(H));
    std::printf("[ multi ] A heap=%lu S=%lu  B heap=%lu S=%lu\n",
                (unsigned long) sa.heapCells, (unsigned long) sa.softCells,
                (unsigned long) sb.heapCells, (unsigned long) sb.softCells);
}

// When a space dies, its share of the budget returns to the others.
TEST(AdaptiveHeap, DestroyedSpaceReturnsItsBudget) {
    CleanEnv env;
    const int H = 3000000;
    ProtoSpace a;
    AdaptiveHeapConfig c;
    c.hardCells = H;
    a.enableAdaptiveHeap(c);
    int ceilingWithB = 0;
    {
        ProtoSpace b;
        b.enableAdaptiveHeap(c);
        ProtoContext keepB(&b, b.rootContext, nullptr, nullptr, nullptr, nullptr);
        while (keepB.allocatedCellsCount < 1000000u) (void) keepB.newObject(false);
        ceilingWithB = ceilingOf(a);
        EXPECT_LE(static_cast<long long>(ceilingWithB), H - static_cast<long long>(heapOf(b)) + heapOf(a));
    }
    EXPECT_GT(ceilingOf(a), ceilingWithB);
    EXPECT_LE(ceilingOf(a), H);
}
