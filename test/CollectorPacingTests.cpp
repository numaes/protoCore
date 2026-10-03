// CollectorPacingTests.cpp -- pacing and early wake (Part A, M1).
//
// Design: docs/specs/2026-10-03-collector-throughput-design.md, sections 4.3
// (pacing: a cycle is requested when the cells left before the ceiling fall
// below the runway r x C) and 4.4 (a thread waiting for headroom wakes when
// cells are published, not on the 50 ms watchdog).
//
//   * pacing::runway is a pure function, tested over synthetic inputs;
//   * on a real space with a fixed limit, a wait at the ceiling ends on a
//     publication of cells, counted by the wake-reason counters;
//   * a fixed-limit space requests cycles before the ceiling;
//   * PROTOCORE_GC_PACING=0 restores the ceiling trigger and the watchdog
//     wake (the 2.11 behaviour), which is also how these cases were seen to
//     fail: with the switch, the counters they assert stay at zero.
//
// The gating cases assert counts.  The rate-controlled case asserts that a
// steady allocator below the collector's capacity stops waiting once the
// cycles are paced; its verdict depends on the machine's speed, so it is in
// CLOCK_DEPENDENT_TESTS (.github/workflows/ci.yml).

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../core/AdaptiveHeap.h"
#include "../headers/proto_internal.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

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

struct CleanEnv {
    ScopedEnv limit{"PROTOCORE_HEAP_LIMIT_CELLS", nullptr};
    ScopedEnv adaptive{"PROTOCORE_ADAPTIVE_HEAP", nullptr};
    ScopedEnv pacing{"PROTOCORE_GC_PACING", nullptr};
};

volatile std::uint64_t g_sink = 0;

// `count` objects of garbage through short-lived contexts of 100 objects, a
// safepoint after each, as a runtime places one between bytecodes.
// `work` iterations of arithmetic per object stand for interpretation.
void churn(ProtoSpace& space, int count, int work = 0) {
    for (int done = 0; done < count; done += 100) {
        {
            ProtoContext sub(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
            for (int j = 0; j < 100; ++j) {
                (void) sub.newObject(false);
                std::uint64_t x = static_cast<std::uint64_t>(j);
                for (int w = 0; w < work; ++w) x = x * 2862933555777941757ULL + 3037000493ULL;
                g_sink = g_sink + x;
            }
        }
        space.rootContext->safepoint();
    }
}

adaptive::WaitStats statsOf(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    return adaptive::waitStats(&space);
}

void print(const char* what, const adaptive::WaitStats& s) {
    std::printf("[ %s ] waits=%llu cells=%llu cycle=%llu watchdog=%llu paced=%llu "
                "cycles=%llu runway=%lld r=%.0f C=%.3fms\n", what,
                (unsigned long long) s.waits, (unsigned long long) s.cellWakes,
                (unsigned long long) s.cycleWakes, (unsigned long long) s.watchdogWakes,
                (unsigned long long) s.pacedRequests, (unsigned long long) s.cyclesCompleted,
                s.runway, s.rate, s.cycleSeconds * 1e3);
}

}  // namespace

// --- pacing::runway, the pure function ----------------------------------------

TEST(PacingRunway, ZeroBeforeTheFirstCycle) {
    // No measured rate or no measured cycle: no runway, which requests a
    // cycle only at the ceiling, as before pacing.
    EXPECT_EQ(adaptive::pacing::runway(1000000, 100000, 0.0, 0.0), 0);
    EXPECT_EQ(adaptive::pacing::runway(1000000, 100000, 5e6, 0.0), 0);
    EXPECT_EQ(adaptive::pacing::runway(1000000, 100000, 0.0, 0.5), 0);
}

TEST(PacingRunway, IsRateTimesCycleWithTheSlack) {
    // r = 1 M cells/s, C = 0.1 s, m = 0.25: 125,000 cells.
    EXPECT_EQ(adaptive::pacing::runway(10000000, 0, 1e6, 0.1), 125000);
    EXPECT_EQ(adaptive::pacing::runway(10000000, 0, 1e6, 0.1, 0.0), 100000);
    EXPECT_DOUBLE_EQ(adaptive::pacing::kCycleSlack, 0.25);
}

TEST(PacingRunway, NeverAboveTheHeadroom) {
    // The headroom is ceiling - retained: a collector that cannot keep up
    // gets the whole headroom (cycles back to back), never more.
    EXPECT_EQ(adaptive::pacing::runway(1000000, 400000, 1e9, 10.0), 600000);
    EXPECT_EQ(adaptive::pacing::runway(1000000, 1000000, 1e6, 0.1), 0);
    EXPECT_EQ(adaptive::pacing::runway(1000000, 2000000, 1e6, 0.1), 0);
    for (double r = 1e3; r < 1e10; r *= 3.7)
        for (double c = 1e-4; c < 100.0; c *= 4.1)
            EXPECT_LE(adaptive::pacing::runway(5000000, 1000000, r, c), 4000000);
}

TEST(PacingRunway, MonotoneInRateAndCycleTime) {
    long long last = 0;
    for (double r = 1e3; r < 1e9; r *= 1.9) {
        const long long w = adaptive::pacing::runway(50000000, 1000000, r, 0.05);
        EXPECT_GE(w, last);
        last = w;
    }
    last = 0;
    for (double c = 1e-5; c < 30.0; c *= 1.7) {
        const long long w = adaptive::pacing::runway(50000000, 1000000, 2e6, c);
        EXPECT_GE(w, last);
        last = w;
    }
}

// --- Early wake (4.4) --------------------------------------------------------

// A fixed limit far below the garbage volume: one thread allocates garbage
// with no work between allocations, so it reaches the ceiling and waits for
// the collector again and again.  Each wait ends when the sweep publishes
// cells; before 2.12 it ended on the cycle START (an empty freelist, so the
// thread waited again) and then on the 50 ms watchdog.
TEST(CollectorPacing, WaitAtTheCeilingEndsWhenCellsArePublished) {
    CleanEnv env;
    ProtoSpace space;
    space.setHeapLimits(0, relaxedLoad(space.heapSize) + 600000);
    churn(space, 6000000);
    const adaptive::WaitStats s = statsOf(space);
    print("early wake", s);
    ASSERT_GT(s.waits, 0u) << "the thread never waited at the ceiling";
    EXPECT_GT(s.cellWakes, 0u) << "no wait ended on a publication of cells";
}

// The switch restores the ceiling trigger and the watchdog wake.
TEST(CollectorPacing, PacingOffRestoresTheCeilingTrigger) {
    CleanEnv env;
    ScopedEnv off("PROTOCORE_GC_PACING", "0");
    ProtoSpace space;
    space.setHeapLimits(0, relaxedLoad(space.heapSize) + 600000);
    churn(space, 3000000);
    const adaptive::WaitStats s = statsOf(space);
    print("pacing off", s);
    EXPECT_GT(s.waits, 0u);
    EXPECT_EQ(s.cellWakes, 0u);
    EXPECT_EQ(s.pacedRequests, 0u);
}

// --- Pacing under a fixed limit (4.3) -----------------------------------------

// After the first cycle has measured r and C, the next cycles are requested
// while there are still cells left before the ceiling.
TEST(CollectorPacing, FixedLimitRequestsCyclesBeforeTheCeiling) {
    CleanEnv env;
    ProtoSpace space;
    space.setHeapLimits(0, relaxedLoad(space.heapSize) + 2000000);
    churn(space, 12000000, 20);
    const adaptive::WaitStats s = statsOf(space);
    print("fixed pacing", s);
    ASSERT_GE(s.cyclesCompleted, 2u) << "too few cycles to measure r and C";
    EXPECT_GT(s.runway, 0) << "no runway after measured cycles";
    EXPECT_GT(s.pacedRequests, 0u) << "no cycle was requested before the ceiling";
}

// Clock-dependent (CLOCK_DEPENDENT_TESTS): a steady allocator well below the
// collector's capacity (interpretation work between allocations), a generous
// fixed limit.  Once the first cycles have measured r and C, paced cycles run
// behind the mutator and it stops waiting for headroom.  Asserts counts, but
// whether the collector keeps up is the machine's speed.
TEST(CollectorPacingRate, SteadyAllocatorBelowCapacityStopsWaiting) {
    CleanEnv env;
    ProtoSpace space;
    space.setHeapLimits(0, relaxedLoad(space.heapSize) + 3000000);
    // Converge: the first cycles start at the ceiling.
    churn(space, 8000000, 200);
    const adaptive::WaitStats before = statsOf(space);
    churn(space, 8000000, 200);
    const adaptive::WaitStats after = statsOf(space);
    print("rate before", before);
    print("rate after", after);
    ASSERT_GT(after.cyclesCompleted, before.cyclesCompleted) << "no cycle in the measured window";
    EXPECT_EQ(after.waits - before.waits, 0u)
        << "the allocator still waited for headroom after the cycles were paced";
}
