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
    EXPECT_EQ(s.softCells, std::min<proto_ulong>(524288, H));
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
    space.enableAdaptiveHeap();
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
// decreases, the heap follows S, and S does not run away towards H.
TEST(AdaptiveHeap, SteadyWorkloadHeapFollowsTheSoftLimit) {
    CleanEnv env;
    ProtoSpace space;
    const SteadyRun run = runSteady(space);
    const AdaptiveHeapStats& s = run.stats;
    EXPECT_EQ(run.liveElements, 100000u) << "the live set did not survive";
    EXPECT_GT(s.cycles, 0u);
    EXPECT_TRUE(run.softNeverDecreased);
    EXPECT_LT(s.softCells, s.hardCells / 4) << "S ran away towards H";
    // The heap is S plus what grew while cycles ran (16 MiB blocks).
    EXPECT_LT(s.heapCells, s.softCells + 4000000u);
}

// Clock-dependent (listed in CLOCK_DEPENDENT_TESTS): where S settles depends
// on the allocation rate against the collector's speed.  The heap ends far
// below the garbage volume (a quarter of it; 1-5 M cells of 20 M measured on
// Linux x86-64 and macOS arm64).
TEST(AdaptiveHeapSteady, HeapStaysFarBelowTheGarbageVolume) {
    CleanEnv env;
    ProtoSpace space;
    const SteadyRun run = runSteady(space);
    EXPECT_LT(run.stats.heapCells, run.garbageCells / 4)
        << run.garbageCells << " cells allocated";
}

// --- Storm workload ----------------------------------------------------------

namespace {
struct StormRun {
    proto_ulong floor = 0;   // max(S0, k_live * L), k_live = 1, L of the last cycle
    proto_ulong finalSoft = 0;
    double maxPressure = 0.0;
    double lastPressure = 1.0;
    uint64_t cycles = 0;
    uint64_t cyclesToCalm = 0;   // 0: pressure never fell below p_high
};

// A large live set with k_live = 1, so S starts at the live set itself.
// With `safepoints` false every cycle is a stall (see churn).
StormRun runStorm(ProtoSpace& space, int maxRounds, bool stopWhenCalm, bool safepoints) {
    AdaptiveHeapConfig c;
    c.liveHeadroom = 1.0;
    space.enableAdaptiveHeap(c);
    RootedList live(space, 300000);   // ~900,000 live cells
    StormRun run;
    uint64_t seen = space.adaptiveHeapStats().cycles;
    for (int r = 0; r < maxRounds; ++r) {
        churn(space, 100000, 0, safepoints);
        const AdaptiveHeapStats s = space.adaptiveHeapStats();
        if (s.cycles != seen) {
            seen = s.cycles;
            run.lastPressure = s.lastPressure;
            if (s.lastPressure > run.maxPressure) run.maxPressure = s.lastPressure;
            if (run.cyclesToCalm == 0 && run.maxPressure > 0.05 && s.lastPressure <= 0.05) {
                run.cyclesToCalm = s.cycles;
                if (stopWhenCalm) break;
            }
        }
    }
    const AdaptiveHeapStats s = space.adaptiveHeapStats();
    run.finalSoft = s.softCells;
    run.floor = std::max<proto_ulong>(524288, s.liveCellsLastCycle);
    run.cycles = s.cycles;
    std::printf("[ storm ] L=%lu S=%lu heap=%lu cycles=%llu maxp=%.3f lastp=%.4f calm-at=%llu\n",
                (unsigned long) s.liveCellsLastCycle, (unsigned long) s.softCells,
                (unsigned long) s.heapCells, (unsigned long long) s.cycles,
                run.maxPressure, run.lastPressure, (unsigned long long) run.cyclesToCalm);
    return run;
}
}  // namespace

// Gating: under a storm (pressure far above p_high) S grows past the floor.
// The mutator reaches no safepoint, so every cycle stalls it: the verdict
// does not depend on the machine's speed.
TEST(AdaptiveHeapStorm, SoftLimitRisesUnderStall) {
    CleanEnv env;
    ProtoSpace space;
    const StormRun run = runStorm(space, 60, false, false);
    EXPECT_GT(run.maxPressure, 0.05) << "the storm produced no stall";
    EXPECT_GT(run.finalSoft, run.floor * 5 / 4)
        << "S never grew beyond max(S0, L)";
}

// Clock-dependent (listed in CLOCK_DEPENDENT_TESTS): with safepoints, after
// the storm, pressure falls below p_high within a bounded number of cycles.
// Whether the start is a storm at all depends on the relative speed of the
// mutator and the collector, hence not a gate.
TEST(AdaptiveHeapStorm, PressureFallsWithinBoundedCycles) {
    CleanEnv env;
    ProtoSpace space;
    const StormRun run = runStorm(space, 3000, true, true);
    ASSERT_GT(run.maxPressure, 0.05) << "the storm produced no stall";
    EXPECT_GT(run.cyclesToCalm, 0u) << "pressure never fell below p_high";
    EXPECT_LE(run.cyclesToCalm, 60u);
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

// --- Several spaces ----------------------------------------------------------

// H is a process budget: a space's growth is limited by what the others
// hold, and each space keeps its own soft limit.
TEST(AdaptiveHeap, SeveralSpacesShareTheProcessBudget) {
    CleanEnv env;
    const int H = 3000000;
    ProtoSpace a;
    ProtoSpace b;
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
