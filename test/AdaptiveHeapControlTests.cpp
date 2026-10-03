// AdaptiveHeapControlTests.cpp -- deterministic tests of the adaptive heap
// controller's pure parts: the control law, the memory-limit parsers and the
// default hard limit.  No ProtoSpace, no clock.
//
// Design: docs/specs/2026-10-02-adaptive-heap-controller-design.md, section 4.

#include <gtest/gtest.h>
#include "../core/AdaptiveHeap.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace proto;
using namespace proto::adaptive;

namespace {

const LawParams kDefaults;              // p_high 0.05, g 1.5, k_live 3, k_cap 8, S0 128 MiB
constexpr proto_ulong kS0 = kDefaultInitialSoftCells;   // 2,097,152 cells (128 MiB)
constexpr proto_ulong kH = 100000000;   // 6.4 GB of cells

proto_ulong ceilTimes(double k, proto_ulong cells) {
    return static_cast<proto_ulong>(std::ceil(k * static_cast<double>(cells)));
}

}  // namespace

// --- Control law -------------------------------------------------------------

TEST(AdaptiveHeapLaw, DefaultsAreTheCalibratedOnes) {
    // docs/reports/2026-10-03-adaptive-heap-calibration.md
    EXPECT_DOUBLE_EQ(kDefaults.highPressure, 0.05);
    EXPECT_DOUBLE_EQ(kDefaults.growthFactor, 1.5);
    EXPECT_DOUBLE_EQ(kDefaults.liveHeadroom, 3.0);
    EXPECT_DOUBLE_EQ(kDefaults.liveCap, 8.0);
    EXPECT_EQ(kDefaults.initialSoft, 2097152u);
    EXPECT_GT(kTriggerFraction, 0.0);
    EXPECT_LT(kTriggerFraction, 1.0);
    const AdaptiveHeapConfig c;
    const LawParams p = sanitizedParams(c);
    EXPECT_DOUBLE_EQ(p.liveHeadroom, c.liveHeadroom) << "header and law defaults disagree";
    EXPECT_DOUBLE_EQ(p.highPressure, c.highPressure);
    EXPECT_DOUBLE_EQ(p.growthFactor, c.growthFactor);
}

TEST(AdaptiveHeapLaw, SteadyWorkingSetConvergesAndStays) {
    // L = 1,000,000 cells, low pressure: S jumps to the floor 3 L once and
    // then never moves.
    proto_ulong S = kS0;
    for (int cycle = 0; cycle < 50; ++cycle) {
        S = nextSoftLimit(S, kH, 1000000, 0.01, kDefaults);
        EXPECT_EQ(S, 3000000u) << "cycle " << cycle;
    }
}

TEST(AdaptiveHeapLaw, SmallSteadyWorkingSetKeepsTheInitialLimit) {
    proto_ulong S = kS0;
    for (int cycle = 0; cycle < 20; ++cycle)
        S = nextSoftLimit(S, kH, 100000, 0.0, kDefaults);
    EXPECT_EQ(S, kS0) << "a live set far below S0 / k_live must not move S";
}

TEST(AdaptiveHeapLaw, FloorFollowsTheLiveSet) {
    EXPECT_EQ(nextSoftLimit(kS0, kH, 2000000, 0.0, kDefaults), 6000000u);
    // ceil(3 * 1,000,001) = 3,000,003
    EXPECT_EQ(nextSoftLimit(kS0, kH, 1000001, 0.0, kDefaults), 3000003u);
    LawParams tight = kDefaults;
    tight.liveHeadroom = 1.0;
    EXPECT_EQ(nextSoftLimit(kS0, kH, 2000000, 0.0, tight), 2097152u) << "S0 stays";
    EXPECT_EQ(nextSoftLimit(kS0, kH, 3000000, 0.0, tight), 3000000u);
}

TEST(AdaptiveHeapLaw, GrowingWorkingSetGrowsSInBoundedSteps) {
    // The live set doubles every 5 cycles; S tracks 3 L, and each cycle's
    // step is exactly the floor's (bounded by k_live times the growth of L).
    proto_ulong S = kS0;
    proto_ulong L = 400000;
    for (int cycle = 0; cycle < 40; ++cycle) {
        if (cycle % 5 == 4) L *= 2;
        const proto_ulong next = nextSoftLimit(S, kH, L, 0.0, kDefaults);
        EXPECT_EQ(next, std::min<proto_ulong>(kH, std::max(S, ceilTimes(3.0, L)))) << "cycle " << cycle;
        S = next;
    }
    EXPECT_EQ(S, kH) << "a live set beyond H / k_live pins S at H";
}

TEST(AdaptiveHeapLaw, StormRaisesSUntilPressureFalls) {
    // A synthetic storm: pressure is high while S is below 6x the live set,
    // as it would be when the headroom S - L is too small for the allocation
    // rate.  S must grow by g each cycle and stop as soon as pressure falls.
    const proto_ulong L = 1000000;
    proto_ulong S = kS0;
    int growthSteps = 0;
    for (int cycle = 0; cycle < 100; ++cycle) {
        const double p = (S < 6 * L) ? 0.40 : 0.01;
        const proto_ulong next = nextSoftLimit(S, kH, L, p, kDefaults);
        if (p > kDefaults.highPressure) {
            const proto_ulong grown = ceilTimes(1.5, S);
            EXPECT_EQ(next, std::max<proto_ulong>(3 * L, std::min<proto_ulong>(grown, 8 * L)));
            ++growthSteps;
        } else {
            EXPECT_EQ(next, S) << "S must stop rising once pressure falls";
        }
        S = next;
    }
    EXPECT_GE(S, 6 * L);
    EXPECT_LE(S, 8 * L) << "pressure never takes S past k_cap x L";
    // From the floor 3 L: log_1.5(6 / 3) rounded up, plus the first step.
    EXPECT_LE(growthSteps, 3);
}

// A permanent storm is what a collector slower than the allocation rate
// produces: the stall comes back at every soft limit.  S stops at k_cap x L
// (2.10.0 took S to H, and measured 20 x L and beyond before a run ended).
TEST(AdaptiveHeapLaw, PermanentStormStopsAtTheLiveCap) {
    const proto_ulong L = 1000000;
    proto_ulong S = kS0;
    int steps = 0;
    for (int cycle = 0; cycle < 200; ++cycle) {
        const proto_ulong next = nextSoftLimit(S, kH, L, 1.0, kDefaults);
        if (next != S) ++steps;
        S = next;
    }
    EXPECT_EQ(S, 8 * L);
    // 3 L first (the floor), then log_1.5(8 / 3) rounded up.
    EXPECT_LE(steps, 4);
}

TEST(AdaptiveHeapLaw, LiveCapIsNeverBelowS0NorTheFloor) {
    // No live set: pressure cannot take S past S0.
    EXPECT_EQ(nextSoftLimit(kS0, kH, 0, 1.0, kDefaults), kS0);
    // A tiny live set: the cap is S0, not 8 L.
    EXPECT_EQ(nextSoftLimit(1000000, kH, 1000, 1.0, kDefaults), 1500000u);
    EXPECT_EQ(nextSoftLimit(1500000, kH, 1000, 1.0, kDefaults), kS0);
    // k_live above k_cap: the cap follows the floor (sanitizedParams also
    // raises k_cap to k_live).
    LawParams wide = kDefaults;
    wide.liveHeadroom = 10.0;
    EXPECT_EQ(nextSoftLimit(kS0, kH, 1000000, 1.0, wide), 10000000u);
    EXPECT_EQ(nextSoftLimit(10000000, kH, 1000000, 1.0, wide), 10000000u);
}

TEST(AdaptiveHeapLaw, StormWithALargeLiveSetReachesH) {
    // k_cap x L beyond H: H binds, in log steps.
    const proto_ulong H = 50000000;
    const proto_ulong L = 10000000;
    proto_ulong S = kS0;
    int steps = 0;
    while (S < H) {
        S = nextSoftLimit(S, H, L, 1.0, kDefaults);
        ++steps;
        ASSERT_LE(steps, 64);
    }
    EXPECT_EQ(S, H);
    EXPECT_LE(steps, 3) << "the floor 3 L, then log_1.5(H / 3 L) rounded up";
}

TEST(AdaptiveHeapLaw, NeverDecreasesAndNeverExceedsH) {
    std::mt19937_64 rng(20261002);
    std::uniform_int_distribution<proto_ulong> live(0, 2 * kH / 3);
    std::uniform_real_distribution<double> pressure(0.0, 0.2);
    const proto_ulong H = 50000000;
    for (int run = 0; run < 100; ++run) {
        proto_ulong S = kS0;
        for (int cycle = 0; cycle < 200; ++cycle) {
            const proto_ulong next = nextSoftLimit(S, H, live(rng) % H, pressure(rng), kDefaults);
            ASSERT_GE(next, S) << "S decreased (run " << run << ", cycle " << cycle << ")";
            ASSERT_LE(next, H) << "S above H (run " << run << ", cycle " << cycle << ")";
            S = next;
        }
    }
}

TEST(AdaptiveHeapLaw, NeverExceedsTheLiveCapThroughPressure) {
    // Whatever the pressure, S' <= max(S, floor, cap): pressure alone never
    // takes S past max(S0, 8 L).
    std::mt19937_64 rng(20261003);
    std::uniform_int_distribution<proto_ulong> live(0, 5000000);
    std::uniform_real_distribution<double> pressure(0.0, 2.0);
    for (int run = 0; run < 100; ++run) {
        proto_ulong S = kS0;
        for (int cycle = 0; cycle < 100; ++cycle) {
            const proto_ulong L = live(rng);
            const proto_ulong next = nextSoftLimit(S, kH, L, pressure(rng), kDefaults);
            const proto_ulong bound = std::max({S, kS0, ceilTimes(8.0, L)});
            ASSERT_LE(next, bound) << "run " << run << ", cycle " << cycle;
            S = next;
        }
    }
}

TEST(AdaptiveHeapLaw, PressureExactlyAtThresholdDoesNotGrow) {
    const proto_ulong L = 1000000;    // floor 3 M, cap 8 M
    EXPECT_EQ(nextSoftLimit(4000000, kH, L, 0.05, kDefaults), 4000000u);
    EXPECT_EQ(nextSoftLimit(4000000, kH, L, 0.0500001, kDefaults), 6000000u);
}

TEST(AdaptiveHeapLaw, SanitizedParamsRejectsNonsense) {
    AdaptiveHeapConfig c;
    c.highPressure = 0.0;
    c.growthFactor = 1.0;
    c.liveHeadroom = 0.5;
    LawParams p = sanitizedParams(c);
    EXPECT_DOUBLE_EQ(p.highPressure, 0.05);
    EXPECT_DOUBLE_EQ(p.growthFactor, 1.5);
    EXPECT_DOUBLE_EQ(p.liveHeadroom, 3.0);
    EXPECT_DOUBLE_EQ(p.liveCap, 8.0);
    c.highPressure = 0.02;
    c.growthFactor = 2.0;
    c.liveHeadroom = 1.0;
    p = sanitizedParams(c);
    EXPECT_DOUBLE_EQ(p.highPressure, 0.02);
    EXPECT_DOUBLE_EQ(p.growthFactor, 2.0);
    EXPECT_DOUBLE_EQ(p.liveHeadroom, 1.0);
    EXPECT_DOUBLE_EQ(p.liveCap, 8.0);
    c.liveHeadroom = 12.0;
    p = sanitizedParams(c);
    EXPECT_DOUBLE_EQ(p.liveCap, 12.0) << "k_cap is never below k_live";
}

// --- Default hard limit ------------------------------------------------------

TEST(AdaptiveHeapLimits, DefaultHardCellsIsThreeQuartersOfTheSmallerBound) {
    const std::uint64_t GiB = std::uint64_t(1) << 30;
    EXPECT_EQ(defaultHardCells(16 * GiB, 0), 16 * GiB / 4 * 3 / 64);
    EXPECT_EQ(defaultHardCells(16 * GiB, 2 * GiB), 2 * GiB / 4 * 3 / 64);
    EXPECT_EQ(defaultHardCells(2 * GiB, 16 * GiB), 2 * GiB / 4 * 3 / 64)
        << "a limit above physical memory does not raise H";
    EXPECT_EQ(defaultHardCells(0, 4 * GiB), 4 * GiB / 4 * 3 / 64);
    EXPECT_EQ(defaultHardCells(0, 0), 8 * GiB / 4 * 3 / 64) << "nothing known: 8 GiB";
}

TEST(AdaptiveHeapLimits, DefaultHardCellsIsClampedToIntMax) {
    const std::uint64_t TiB = std::uint64_t(1) << 40;
    EXPECT_EQ(defaultHardCells(1 * TiB, 0), static_cast<proto_ulong>(INT_MAX));
    EXPECT_EQ(defaultHardCells(64, 0), 1u) << "never zero";
}

TEST(AdaptiveHeapLimits, PhysicalMemoryIsDetected) {
    EXPECT_GT(physicalMemoryBytes(), std::uint64_t(64) << 20);
}

TEST(AdaptiveHeapLimits, ParseHeapLimitCells) {
    int soft = -1, hard = -1;
    EXPECT_TRUE(parseHeapLimitCells("1000", soft, hard));
    EXPECT_EQ(soft, 0);
    EXPECT_EQ(hard, 1000);
    EXPECT_TRUE(parseHeapLimitCells("10,2000", soft, hard));
    EXPECT_EQ(soft, 10);
    EXPECT_EQ(hard, 2000);
    EXPECT_TRUE(parseHeapLimitCells("2147483647", soft, hard));
    EXPECT_EQ(hard, INT_MAX);
    EXPECT_FALSE(parseHeapLimitCells("2147483648", soft, hard));
    EXPECT_FALSE(parseHeapLimitCells("", soft, hard));
    EXPECT_FALSE(parseHeapLimitCells("12x", soft, hard));
    EXPECT_FALSE(parseHeapLimitCells("1,", soft, hard));
    EXPECT_FALSE(parseHeapLimitCells(nullptr, soft, hard));
}

// --- cgroup parsing ----------------------------------------------------------

TEST(AdaptiveHeapCgroup, ParseV2Max) {
    EXPECT_EQ(parseCgroupV2Max("max\n"), 0u);
    EXPECT_EQ(parseCgroupV2Max("1073741824\n"), 1073741824u);
    EXPECT_EQ(parseCgroupV2Max("  536870912  "), 536870912u);
    EXPECT_EQ(parseCgroupV2Max(""), 0u);
    EXPECT_EQ(parseCgroupV2Max("12abc"), 0u);
}

TEST(AdaptiveHeapCgroup, ParseV1Limit) {
    EXPECT_EQ(parseCgroupV1Limit("9223372036854771712\n"), 0u) << "v1 'unlimited'";
    EXPECT_EQ(parseCgroupV1Limit("536870912\n"), 536870912u);
    EXPECT_EQ(parseCgroupV1Limit("-1"), 0u);
    EXPECT_EQ(parseCgroupV1Limit("99999999999999999999999"), 0u) << "overflow";
}

TEST(AdaptiveHeapCgroup, PathFromProcSelf) {
    const std::string v2 = "0::/user.slice/user-1000.slice/session-2.scope\n";
    EXPECT_EQ(cgroupPathFromProcSelf(v2, true), "/user.slice/user-1000.slice/session-2.scope");
    EXPECT_EQ(cgroupPathFromProcSelf(v2, false), "");
    const std::string v1 =
        "12:pids:/docker/abc\n"
        "11:cpu,cpuacct:/docker/abc\n"
        "9:memory:/docker/abc\n"
        "1:name=systemd:/docker/abc\n";
    EXPECT_EQ(cgroupPathFromProcSelf(v1, false), "/docker/abc");
    EXPECT_EQ(cgroupPathFromProcSelf(v1, true), "");
    const std::string hybrid = "10:memory:/a\n0::/b\n";
    EXPECT_EQ(cgroupPathFromProcSelf(hybrid, false), "/a");
    EXPECT_EQ(cgroupPathFromProcSelf(hybrid, true), "/b");
    EXPECT_EQ(cgroupPathFromProcSelf("0::/\n", true), "/");
    EXPECT_EQ(cgroupPathFromProcSelf("garbage\n", true), "");
}

namespace {

// A throw-away directory standing in for "/", removed on destruction.
class FakeRoot {
public:
    FakeRoot() {
        static int counter = 0;
        dir_ = std::filesystem::temp_directory_path() /
               ("protocore_cgroup_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "_" + std::to_string(++counter) + "_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    ~FakeRoot() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    void write(const std::string& rel, const std::string& content) {
        const std::filesystem::path p = dir_ / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << content;
    }
    std::string root() const { return dir_.generic_string(); }

private:
    std::filesystem::path dir_;
};

}  // namespace

TEST(AdaptiveHeapCgroup, V2LimitOfTheOwnCgroup) {
    FakeRoot fs;
    fs.write("proc/self/cgroup", "0::/app.slice/job.scope\n");
    fs.write("sys/fs/cgroup/app.slice/job.scope/memory.max", "2147483648\n");
    fs.write("sys/fs/cgroup/app.slice/memory.max", "max\n");
    EXPECT_EQ(cgroupMemoryLimit(fs.root()), 2147483648u);
}

TEST(AdaptiveHeapCgroup, V2AncestorLimitBinds) {
    FakeRoot fs;
    fs.write("proc/self/cgroup", "0::/app.slice/job.scope\n");
    fs.write("sys/fs/cgroup/app.slice/job.scope/memory.max", "max\n");
    fs.write("sys/fs/cgroup/app.slice/memory.max", "1073741824\n");
    EXPECT_EQ(cgroupMemoryLimit(fs.root()), 1073741824u);
}

TEST(AdaptiveHeapCgroup, V2SmallestOfSeveralLimitsWins) {
    FakeRoot fs;
    fs.write("proc/self/cgroup", "0::/a/b/c\n");
    fs.write("sys/fs/cgroup/a/b/c/memory.max", "3000000000\n");
    fs.write("sys/fs/cgroup/a/b/memory.max", "1000000000\n");
    fs.write("sys/fs/cgroup/a/memory.max", "2000000000\n");
    EXPECT_EQ(cgroupMemoryLimit(fs.root()), 1000000000u);
}

TEST(AdaptiveHeapCgroup, V2ContainerNamespaceRoot) {
    // Inside a container with a cgroup namespace the path is "/" and the
    // limit sits at the mount point itself.
    FakeRoot fs;
    fs.write("proc/self/cgroup", "0::/\n");
    fs.write("sys/fs/cgroup/memory.max", "536870912\n");
    EXPECT_EQ(cgroupMemoryLimit(fs.root()), 536870912u);
}

TEST(AdaptiveHeapCgroup, V2Unlimited) {
    FakeRoot fs;
    fs.write("proc/self/cgroup", "0::/user.slice\n");
    fs.write("sys/fs/cgroup/user.slice/memory.max", "max\n");
    EXPECT_EQ(cgroupMemoryLimit(fs.root()), 0u);
}

TEST(AdaptiveHeapCgroup, V1Limit) {
    FakeRoot fs;
    fs.write("proc/self/cgroup", "9:memory:/docker/abc\n3:cpu:/docker/abc\n");
    fs.write("sys/fs/cgroup/memory/docker/abc/memory.limit_in_bytes", "805306368\n");
    fs.write("sys/fs/cgroup/memory/docker/memory.limit_in_bytes", "9223372036854771712\n");
    EXPECT_EQ(cgroupMemoryLimit(fs.root()), 805306368u);
}

TEST(AdaptiveHeapCgroup, V1HostPathHiddenInContainer) {
    // The host path from /proc/self/cgroup does not exist under the
    // container's mount; the limit is found at the mount point.
    FakeRoot fs;
    fs.write("proc/self/cgroup", "9:memory:/docker/abc\n");
    fs.write("sys/fs/cgroup/memory/memory.limit_in_bytes", "268435456\n");
    EXPECT_EQ(cgroupMemoryLimit(fs.root()), 268435456u);
}

TEST(AdaptiveHeapCgroup, HybridTakesTheSmaller) {
    FakeRoot fs;
    fs.write("proc/self/cgroup", "9:memory:/x\n0::/y\n");
    fs.write("sys/fs/cgroup/memory/x/memory.limit_in_bytes", "700000000\n");
    fs.write("sys/fs/cgroup/y/memory.max", "900000000\n");
    EXPECT_EQ(cgroupMemoryLimit(fs.root()), 700000000u);
}

TEST(AdaptiveHeapCgroup, NoCgroupFileMeansNoLimit) {
    FakeRoot fs;
    EXPECT_EQ(cgroupMemoryLimit(fs.root()), 0u);
}

#if defined(_WIN32)
// The process-memory limit of a job object.  Assigning a process to a job is
// irreversible, so it is done in a child process (a death-test child): the
// child joins a fresh job limited to 3 GiB and exits 0 when
// processMemoryLimitBytes reports exactly that.
static int jobObjectLimitChild() {
    const std::uint64_t limit = std::uint64_t(3) << 30;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return 2;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    info.ProcessMemoryLimit = static_cast<SIZE_T>(limit);
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)))
        return 3;
    if (!AssignProcessToJobObject(job, GetCurrentProcess())) return 4;
    return processMemoryLimitBytes() == limit ? 0 : 1;
}

TEST(AdaptiveHeapJobObjectDeathTest, ProcessMemoryLimitIsDetected) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT(std::_Exit(jobObjectLimitChild()), ::testing::ExitedWithCode(0), "");
}
#endif
