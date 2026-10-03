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

constexpr proto_ulong kS0 = kDefaultInitialSoftCells;   // 2,097,152 cells (128 MiB)
constexpr proto_ulong kB = 100000000;   // 6.4 GB of cells

LawInputs inputs(proto_ulong S, proto_ulong L, double r, double T, double w, proto_ulong B = kB) {
    LawInputs in;
    in.softCells = S;
    in.budgetCells = B;
    in.liveCells = L;
    in.rate = r;
    in.throughput = T;
    in.waitShare = w;
    return in;
}

}  // namespace

// --- Control law (collector-throughput spec, 4.5) ------------------------------
//
// Replaces the 2.10.1 law: k_live, k_cap and p_high are gone.  Each case
// feeds a synthetic sequence of (L, r, T, w) and checks the properties of
// section 9 (P5: bounded convergence, never decreasing; P6: regime 2 does not
// spend memory for nothing) and of 10.2.1.

TEST(AdaptiveHeapLaw, ConstantsAreStructural) {
    EXPECT_DOUBLE_EQ(kCycleSlack, 0.25);
    EXPECT_EQ(kNonImprovingProbesToStop, 2);
    EXPECT_DOUBLE_EQ(kRearmFactor, 2.0);
    EXPECT_DOUBLE_EQ(pacing::kCycleSlack, kCycleSlack);
}

TEST(AdaptiveHeapLaw, RegimeOneReachesTheTargetInOneStepAndHolds) {
    // rho = 0.5: G* = L, target = L + 1.25 L.
    LawState st;
    const proto_ulong L = 4000000;
    proto_ulong S = nextSoftLimit(inputs(kS0, L, 5e6, 1e7, 0.2), st);
    EXPECT_EQ(S, static_cast<proto_ulong>(std::ceil(L + 1.25 * L)));
    for (int i = 0; i < 50; ++i) {
        const proto_ulong next = nextSoftLimit(inputs(S, L, 5e6, 1e7, 0.0), st);
        EXPECT_EQ(next, S) << "S moved on a steady regime-1 workload";
    }
    EXPECT_EQ(st.changes, 1u);
    EXPECT_EQ(st.probes, 0u);
}

TEST(AdaptiveHeapLaw, RegimeOneWithoutWaitsKeepsS) {
    // The heap does not grow for nothing (4.1): no wait, no growth.
    LawState st;
    EXPECT_EQ(nextSoftLimit(inputs(kS0, 4000000, 5e6, 1e7, 0.0), st), kS0);
    EXPECT_EQ(st.changes, 0u);
}

TEST(AdaptiveHeapLaw, RegimeOneNeverShrinksALargerS) {
    LawState st;
    EXPECT_EQ(nextSoftLimit(inputs(50000000, 1000000, 1e6, 1e7, 0.0), st), 50000000u);
}

TEST(AdaptiveHeapLaw, RhoApproachingOneGrowsGStarButNeverPastB) {
    LawState st;
    proto_ulong last = 0;
    for (double rho = 0.1; rho < 1.0; rho += 0.05) {
        LawState fresh;
        const proto_ulong S = nextSoftLimit(inputs(kS0, 5000000, rho * 1e7, 1e7, 0.1), fresh);
        EXPECT_GE(S, last);
        EXPECT_LE(S, kB);
        last = S;
    }
    EXPECT_EQ(nextSoftLimit(inputs(kS0, 5000000, 0.9999 * 1e7, 1e7, 0.1), st), kB);
}

TEST(AdaptiveHeapLaw, RegimeTwoDoublesWhileTheWaitsFall) {
    LawState st;
    proto_ulong S = kS0;
    double w = 0.5;
    for (int i = 0; i < 5; ++i) {
        const proto_ulong next = nextSoftLimit(inputs(S, 1000000, 2e7, 1e7, w), st);
        EXPECT_EQ(next, std::min<proto_ulong>(kB, 2 * S));
        S = next;
        w *= 0.7;   // each doubling reduced the waits
    }
    EXPECT_FALSE(st.stopped);
    EXPECT_EQ(st.probes, 5u);
}

TEST(AdaptiveHeapLaw, RegimeTwoStopsAfterTwoNonImprovingProbes) {
    LawState st;
    proto_ulong S = kS0;
    // Probe 1 (w = 0.5), then the waits do not fall: probe 2 is non-improving
    // #1, probe 3's verdict is non-improving #2, growth stops.
    S = nextSoftLimit(inputs(S, 1000000, 2e7, 1e7, 0.5), st);
    EXPECT_EQ(S, 2 * kS0);
    S = nextSoftLimit(inputs(S, 1000000, 2e7, 1e7, 0.5), st);
    EXPECT_EQ(S, 4 * kS0);
    EXPECT_FALSE(st.stopped);
    const proto_ulong held = nextSoftLimit(inputs(S, 1000000, 2e7, 1e7, 0.55), st);
    EXPECT_TRUE(st.stopped);
    EXPECT_EQ(held, S) << "growth did not stop after two non-improving probes";
    for (int i = 0; i < 100; ++i)
        EXPECT_EQ(nextSoftLimit(inputs(S, 1000000, 2e7, 1e7, 0.6), st), S);
}

TEST(AdaptiveHeapLaw, ASingleNoisyProbeDoesNotStopGrowth) {
    LawState st;
    proto_ulong S = kS0;
    S = nextSoftLimit(inputs(S, 1000000, 2e7, 1e7, 0.5), st);
    S = nextSoftLimit(inputs(S, 1000000, 2e7, 1e7, 0.6), st);   // worse once
    S = nextSoftLimit(inputs(S, 1000000, 2e7, 1e7, 0.3), st);   // better again
    EXPECT_FALSE(st.stopped);
    EXPECT_EQ(S, 8 * kS0);
}

// Measured on protoJS records N = 12 (H = 40 M cells): the probes fell at a
// phase change of the program (build -> twelve parallel tasks, r x 5), the
// waits rose with the load, two probes counted as non-improving, and S sat
// at 8 M cells for 12 cycles with a third of the mutators' time waiting.
TEST(AdaptiveHeapLaw, AProbeAcrossAWorkloadChangeDoesNotCount) {
    LawState st;
    proto_ulong S = kS0;
    S = nextSoftLimit(inputs(S, 1000000, 2e7, 1e7, 0.1), st);   // probe
    S = nextSoftLimit(inputs(S, 1000000, 9e7, 1e7, 0.3), st);   // r x 4.5: void
    S = nextSoftLimit(inputs(S, 2100000, 9e7, 1e7, 0.4), st);   // L x 2.1: void
    EXPECT_FALSE(st.stopped);
    EXPECT_EQ(st.voidProbes, 2u);
    EXPECT_EQ(st.nonImproving, 0);
    EXPECT_EQ(S, 8 * kS0);
}

TEST(AdaptiveHeapLaw, AFactorOfTwoInLiveOrRateRearmsGrowth) {
    for (int which = 0; which < 4; ++which) {
        LawState st;
        proto_ulong S = kS0;
        for (int i = 0; i < 3; ++i) S = nextSoftLimit(inputs(S, 1000000, 2e7, 1e7, 0.5), st);
        ASSERT_TRUE(st.stopped);
        const proto_ulong L = which == 0 ? 2000000 : which == 1 ? 500000 : 1000000;
        const double r = which == 2 ? 4e7 : which == 3 ? 1.2e7 /* still above T */ : 2e7;
        const proto_ulong next = nextSoftLimit(inputs(S, L, r, 1e7, 0.5), st);
        if (which == 3) {
            EXPECT_TRUE(st.stopped) << "a rate change below a factor of 2 re-armed growth";
            EXPECT_EQ(next, S);
        } else {
            EXPECT_FALSE(st.stopped) << "case " << which;
            EXPECT_EQ(next, 2 * S) << "case " << which;
            EXPECT_EQ(st.rearms, 1u);
        }
    }
}

TEST(AdaptiveHeapLaw, NoWaitsNoGrowthInRegimeTwo) {
    LawState st;
    EXPECT_EQ(nextSoftLimit(inputs(kS0, 1000000, 2e7, 1e7, 0.0), st), kS0);
    EXPECT_EQ(st.probes, 0u);
}

TEST(AdaptiveHeapLaw, UnmeasuredInputsKeepS) {
    LawState st;
    EXPECT_EQ(nextSoftLimit(inputs(kS0, 1000000, 0.0, 1e7, 0.9), st), kS0);
    EXPECT_EQ(nextSoftLimit(inputs(kS0, 1000000, 1e7, 0.0, 0.9), st), kS0);
}

// P5 and P6 over random sequences: S never decreases, never exceeds B, and
// changes at most log2(B / S0) times per armed period plus the regime-1
// jumps that follow a change of the inputs.
TEST(AdaptiveHeapLaw, NeverDecreasesNeverExceedsBAndConvergesBoundedly) {
    std::mt19937_64 rng(20261003);
    for (int run = 0; run < 200; ++run) {
        LawState st;
        const proto_ulong B = 1000000 + rng() % 400000000;
        proto_ulong S = std::min<proto_ulong>(kS0, B);
        // A steady workload: fixed L, r, T, with noisy waits.
        const proto_ulong L = rng() % (B / 2 + 1);
        const double T = 1e6 + static_cast<double>(rng() % 50000000);
        const double r = T * (0.2 + static_cast<double>(rng() % 300) / 100.0);
        std::uniform_real_distribution<double> wd(0.0, 0.6);
        for (int i = 0; i < 400; ++i) {
            const proto_ulong next = nextSoftLimit(inputs(S, L, r, T, wd(rng), B), st);
            ASSERT_GE(next, S);
            ASSERT_LE(next, B);
            S = next;
        }
        const double bound = std::ceil(std::log2(static_cast<double>(B) / static_cast<double>(std::min<proto_ulong>(kS0, B)))) + 1;
        EXPECT_LE(static_cast<double>(st.changes), bound) << "run " << run;
        EXPECT_EQ(st.rearms, 0u);
    }
}

// The budget is hard: a doubling stops at B.
TEST(AdaptiveHeapLaw, ProbesStopAtTheBudget) {
    LawState st;
    const proto_ulong B = 3 * kS0;
    proto_ulong S = kS0;
    double w = 0.9;
    for (int i = 0; i < 10; ++i) {
        S = nextSoftLimit(inputs(S, 100000, 2e7, 1e7, w, B), st);
        EXPECT_LE(S, B);
        w *= 0.5;
    }
    EXPECT_EQ(S, B);
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
