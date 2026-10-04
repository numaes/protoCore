/*
 * AdaptiveHeap.cpp -- the adaptive heap controller.  See AdaptiveHeap.h and
 * docs/specs/2026-10-02-adaptive-heap-controller-design.md.
 */

#include "AdaptiveHeap.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

namespace proto {
namespace adaptive {

    // --- 1. Control law ------------------------------------------------------

    namespace {
        double raiseToFloor(double next, proto_ulong S, proto_ulong L, double runway, proto_ulong B) {
            const double floor = static_cast<double>(liveSetFloor(L, runway, B));
            if (next >= floor) return next;
            return std::min(static_cast<double>(B),
                            std::max(floor, 2.0 * static_cast<double>(std::max<proto_ulong>(S, 1))));
        }
    }  // namespace

    proto_ulong nextSoftLimit(const LawInputs& in, LawState& st) {
        const proto_ulong S = in.softCells;
        const proto_ulong B = in.budgetCells;
        const double L = static_cast<double>(in.liveCells);
        const double r = in.rate;
        const double w = in.waitShare;

        // Re-arm: the workload changed by a factor of 2 since the stop.
        if (st.stopped) {
            const double l0 = static_cast<double>(st.liveAtStop);
            const bool liveMoved = (L >= kRearmFactor * l0 && L > 0.0) || (l0 >= kRearmFactor * L && l0 > 0.0);
            const bool rateMoved = (r >= kRearmFactor * st.rateAtStop && r > 0.0)
                                || (st.rateAtStop >= kRearmFactor * r && st.rateAtStop > 0.0);
            if (liveMoved || rateMoved) {
                st.stopped = false;
                st.nonImproving = 0;
                st.probePending = false;
                ++st.rearms;
            }
        }
        // The verdict on the last probe.  A probe compares the wait share
        // before and after a doubling of S; if the workload itself changed
        // meanwhile (L or r by a factor of 2, the re-arm rule), the
        // comparison says nothing about S and the probe does not count.
        if (st.probePending) {
            st.probePending = false;
            const double lp = static_cast<double>(st.liveAtProbe);
            const bool moved = (L >= kRearmFactor * lp && L > 0.0) || (lp >= kRearmFactor * L && lp > 0.0)
                            || (r >= kRearmFactor * st.rateAtProbe && r > 0.0)
                            || (st.rateAtProbe >= kRearmFactor * r && st.rateAtProbe > 0.0);
            if (moved) {
                ++st.voidProbes;
            } else if (w < st.waitBeforeProbe) {
                st.nonImproving = 0;
            } else if (++st.nonImproving >= kNonImprovingProbesToStop) {
                st.stopped = true;
                st.liveAtStop = in.liveCells;
                st.rateAtStop = r;
            }
        }

        double next = static_cast<double>(S);
        const bool measured = r > 0.0 && in.throughput > 0.0;
        if (measured && r < in.throughput && w > 0.0) {
            // Regime 1: the headroom that lets a cycle run behind the
            // mutators -- only while they wait (4.1: the heap does not grow
            // for nothing; with no wait, pacing already hides the cycles).
            const double rho = r / in.throughput;
            const double gStar = rho * L / (1.0 - rho);
            const double target = std::min(static_cast<double>(B), std::ceil(L + gStar * (1.0 + kCycleSlack)));
            next = std::max(next, target);
        } else if (measured && w > 0.0 && !st.stopped && S < B) {
            // Regime 2: does a larger heap reduce the waits?
            next = std::min(static_cast<double>(B), 2.0 * static_cast<double>(std::max<proto_ulong>(S, 1)));
            st.probePending = true;
            st.waitBeforeProbe = w;
            st.liveAtProbe = in.liveCells;
            st.rateAtProbe = r;
            ++st.probes;
        }
        // The live-set floor: a cycle reclaims at least as many cells as it
        // marks, whatever the waits.  Raised to it by at least a doubling, so
        // that a floor that creeps with the measured runway changes S no more
        // often than the probes do (bounded convergence).
        next = raiseToFloor(next, S, in.liveCells, in.runwayCells, B);
        next = std::min(next, static_cast<double>(B));
        proto_ulong result = static_cast<proto_ulong>(next);
        if (result < S) result = S <= B ? S : B;   // never decreases; never above B
        if (result != S) ++st.changes;
        return result;
    }

    proto_ulong liveSetFloor(proto_ulong liveCells, double runwayCells, proto_ulong budgetCells) {
        const double floor = kLiveSetFloorFactor * static_cast<double>(liveCells)
                           + (runwayCells > 0.0 ? std::ceil(runwayCells) : 0.0);
        if (floor >= static_cast<double>(budgetCells)) return budgetCells;
        return static_cast<proto_ulong>(floor);
    }

    // --- 1b. Pacing -----------------------------------------------------------

    namespace pacing {
        long long runway(long long ceiling, long long retained, double rate,
                         double cycleSeconds, double slack) {
            const long long headroom = std::max(0LL, ceiling - retained);
            if (!(rate > 0.0) || !(cycleSeconds > 0.0)) return 0;
            if (!(slack >= 0.0)) slack = 0.0;
            const double need = std::ceil(rate * cycleSeconds * (1.0 + slack));
            if (need >= static_cast<double>(headroom)) return headroom;
            return static_cast<long long>(need);
        }
    }  // namespace pacing

    // --- 2. Memory limits ----------------------------------------------------

    namespace {
        std::string trim(const std::string& s) {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
            return s.substr(b, e - b);
        }

        // Decimal digits only; false on overflow or anything else.
        bool parseU64(const std::string& s, std::uint64_t& out) {
            if (s.empty()) return false;
            std::uint64_t v = 0;
            for (char c : s) {
                if (c < '0' || c > '9') return false;
                const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
                if (v > (UINT64_MAX - d) / 10) return false;
                v = v * 10 + d;
            }
            out = v;
            return true;
        }

        bool readFile(const std::string& path, std::string& out) {
            std::ifstream in(path, std::ios::in | std::ios::binary);
            if (!in) return false;
            std::ostringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        }

        // The smallest limit found in `dir/file` and in every ancestor of
        // `dir` down to `mount` (inclusive).  0 when none.
        std::uint64_t smallestLimitUpwards(const std::string& mount,
                                           std::string path,
                                           const char* file,
                                           std::uint64_t (*parse)(const std::string&)) {
            std::uint64_t best = 0;
            // Normalise: no trailing slash except the root itself.
            while (path.size() > 1 && path.back() == '/') path.pop_back();
            for (;;) {
                const std::string dir = (path == "/" || path.empty()) ? mount : mount + path;
                std::string content;
                if (readFile(dir + "/" + file, content)) {
                    const std::uint64_t v = parse(content);
                    if (v > 0 && (best == 0 || v < best)) best = v;
                }
                if (path.empty() || path == "/") break;
                const size_t slash = path.find_last_of('/');
                if (slash == std::string::npos || slash == 0) path = "/";
                else path = path.substr(0, slash);
            }
            return best;
        }
    }  // namespace

    std::uint64_t parseCgroupV2Max(const std::string& content) {
        const std::string v = trim(content);
        std::uint64_t bytes = 0;
        if (v == "max" || !parseU64(v, bytes)) return 0;
        return bytes;
    }

    std::uint64_t parseCgroupV1Limit(const std::string& content) {
        std::uint64_t bytes = 0;
        if (!parseU64(trim(content), bytes)) return 0;
        // "No limit" is LONG_MAX rounded down to a page (9223372036854771712);
        // no real limit comes anywhere near 2^60 bytes.
        if (bytes >= (static_cast<std::uint64_t>(1) << 60)) return 0;
        return bytes;
    }

    std::string cgroupPathFromProcSelf(const std::string& procSelfCgroup, bool v2) {
        std::istringstream lines(procSelfCgroup);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // hierarchy-ID:controller-list:cgroup-path (the path may contain ':')
            const size_t c1 = line.find(':');
            if (c1 == std::string::npos) continue;
            const size_t c2 = line.find(':', c1 + 1);
            if (c2 == std::string::npos) continue;
            const std::string id = line.substr(0, c1);
            const std::string controllers = line.substr(c1 + 1, c2 - c1 - 1);
            const std::string path = line.substr(c2 + 1);
            if (v2) {
                if (id == "0" && controllers.empty()) return path.empty() ? "/" : path;
            } else {
                std::istringstream list(controllers);
                std::string name;
                while (std::getline(list, name, ',')) {
                    if (name == "memory") return path.empty() ? "/" : path;
                }
            }
        }
        return std::string();
    }

    std::uint64_t cgroupMemoryLimit(const std::string& root) {
        std::string self;
        if (!readFile(root + "/proc/self/cgroup", self)) return 0;
        std::uint64_t best = 0;
        auto consider = [&best](std::uint64_t v) {
            if (v > 0 && (best == 0 || v < best)) best = v;
        };
        // A hybrid system can have both; the smaller limit binds.
        const std::string v2 = cgroupPathFromProcSelf(self, true);
        if (!v2.empty())
            consider(smallestLimitUpwards(root + "/sys/fs/cgroup", v2,
                                          "memory.max", &parseCgroupV2Max));
        const std::string v1 = cgroupPathFromProcSelf(self, false);
        if (!v1.empty())
            consider(smallestLimitUpwards(root + "/sys/fs/cgroup/memory", v1,
                                          "memory.limit_in_bytes", &parseCgroupV1Limit));
        return best;
    }

    std::uint64_t physicalMemoryBytes() {
#if defined(_WIN32)
        MEMORYSTATUSEX status;
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) return static_cast<std::uint64_t>(status.ullTotalPhys);
        return 0;
#elif defined(__APPLE__)
        std::uint64_t bytes = 0;
        size_t len = sizeof(bytes);
        if (sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) == 0) return bytes;
        return 0;
#else
        const long pages = sysconf(_SC_PHYS_PAGES);
        const long pageSize = sysconf(_SC_PAGE_SIZE);
        if (pages <= 0 || pageSize <= 0) return 0;
        return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageSize);
#endif
    }

    std::uint64_t processMemoryLimitBytes() {
#if defined(_WIN32)
        // The job object this process belongs to, if any (nullptr = the
        // caller's job).  A process limit and a job limit may both be set;
        // the smaller binds.
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
        std::memset(&info, 0, sizeof(info));
        if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
                                       &info, sizeof(info), nullptr))
            return 0;
        std::uint64_t best = 0;
        if (info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY)
            best = static_cast<std::uint64_t>(info.ProcessMemoryLimit);
        if (info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) {
            const std::uint64_t job = static_cast<std::uint64_t>(info.JobMemoryLimit);
            if (job > 0 && (best == 0 || job < best)) best = job;
        }
        return best;
#elif defined(__linux__)
        return cgroupMemoryLimit("");
#else
        return 0;
#endif
    }

    proto_ulong defaultHardCells(std::uint64_t physicalBytes, std::uint64_t limitBytes) {
        std::uint64_t bytes = physicalBytes;
        if (limitBytes > 0 && (bytes == 0 || limitBytes < bytes)) bytes = limitBytes;
        if (bytes == 0) bytes = static_cast<std::uint64_t>(8) << 30;  // unknown: 8 GiB
        std::uint64_t cells = (bytes / 4 * 3) / 64;
        if (cells < 1) cells = 1;
        if (cells > kMaxCells) cells = kMaxCells;
        return static_cast<proto_ulong>(cells);
    }

    bool parseHeapLimitCells(const char* text, int& softCells, int& hardCells) {
        if (!text) return false;
        auto parseCells = [](const char* begin, const char* end, int& out) {
            if (begin == end) return false;
            long long value = 0;
            for (const char* p = begin; p != end; ++p) {
                if (*p < '0' || *p > '9') return false;
                value = value * 10 + (*p - '0');
                if (value > INT_MAX) return false;
            }
            out = static_cast<int>(value);
            return true;
        };
        const char* end = text;
        const char* comma = nullptr;
        for (; *end; ++end) {
            if (*end == ',' && !comma) comma = end;
        }
        int soft = 0;
        int hard = 0;
        const bool valid = comma
            ? parseCells(text, comma, soft) && parseCells(comma + 1, end, hard)
            : parseCells(text, end, hard);
        if (!valid) return false;
        softCells = soft;
        hardCells = hard;
        return true;
    }

    // --- 3. Per-space state --------------------------------------------------

    namespace {
        using Clock = std::chrono::steady_clock;

        struct SpaceState {
            ProtoSpace* space = nullptr;
            bool enabled = false;
            bool trace = false;
            LawState law;
            double lastThroughput = 0.0;
            proto_ulong hardCells = 0;
            // Mutator waits since the last cycle end, in nanoseconds.
            std::atomic<std::uint64_t> waitNanos{0};
            Clock::time_point lastCycleEnd;
            // When the collector began the current cycle (the trace's Tc).
            Clock::time_point cycleStart;
            // Cycles completed while the controller was enabled.
            std::uint64_t cycles = 0;
            double lastPressure = 0.0;
            // A refill went past S without waiting; cleared at cycle end.
            bool pending = false;
            // Cells the last cycle left unreclaimed: heapSize minus the
            // global freelist, right after the sweep published its cells.
            proto_ulong retainedLastCycle = 0;

            // --- Pacing and waits (every space, enabled or not) ---
            // PROTOCORE_GC_PACING=0 turns pacing and the early wake off for
            // this space (read at registration): the 2.11 behaviour.
            bool pacing = true;
            // pace(): a cycle is requested when the cells left before the
            // ceiling fall below this (pacing::runway at the last cycle end).
            long long runway = 0;
            std::uint64_t completed = 0;
            CycleMeasures last;
            // The first request of the cycle in flight (C runs from here).
            bool requested = false;
            Clock::time_point requestedAt;
            // heapSize - freeCellsCount at the last cycle end (-1: none yet),
            // and cells returned unused since: with the cells the cycle
            // reclaimed they give the cells handed out in the interval.
            long long occupiedLastEnd = -1;
            proto_ulong returnedSinceEnd = 0;
            Clock::time_point intervalStart;
            // The last two cycles' r and C; pacing uses the larger of each.
            double rate[2] = {0.0, 0.0};
            double cycleSeconds[2] = {0.0, 0.0};
            // The mutators' wait share over the last interval.
            double waitShare = 0.0;
            // Threads in reclaimWaitLocked now.
            int waiters = 0;
            WaitStats stats;
        };

        // Guarded by ProtoSpace::globalMutex.  States live as long as their
        // space (a waiting thread may hold one across a wait), so disabling
        // only clears the flag.
        std::vector<SpaceState*>& registry() {
            static std::vector<SpaceState*> states;
            return states;
        }
        int enabledCount = 0;
        // The sum of every live space's heapSize, and the process budget H
        // (0 until a space enables the controller).
        long long processHeapCells = 0;
        // The largest processHeapCells seen at a growth (P2, tests).
        long long processHeapPeak = 0;
        proto_ulong processBudgetCells = 0;

        SpaceState* find(const ProtoSpace* space) {
            for (SpaceState* s : registry())
                if (s->space == space) return s;
            return nullptr;
        }

        SpaceState* findEnabled(const ProtoSpace* space) {
            if (enabledCount == 0) return nullptr;
            SpaceState* s = find(space);
            return (s && s->enabled) ? s : nullptr;
        }

        // Each enabled space may grow by what is left of the process budget.
        void recomputeCeilings() {
            if (enabledCount == 0) return;
            const long long budget = static_cast<long long>(processBudgetCells);
            const long long room = std::max(0LL, budget - processHeapCells);
            for (SpaceState* s : registry()) {
                if (!s->enabled) continue;
                const long long own = relaxedLoad(s->space->heapSize);
                long long ceiling = std::min(budget, own + room);
                if (ceiling < 1) ceiling = 1;
                if (ceiling > static_cast<long long>(kMaxCells))
                    ceiling = static_cast<long long>(kMaxCells);
                relaxedStore(s->space->maxHeapSize, static_cast<int>(ceiling));
            }
        }
    }  // namespace

    std::atomic<int> softWaitPending{0};

    namespace {
        void clearPending(SpaceState* s) {
            if (s->pending) {
                s->pending = false;
                softWaitPending.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        void noteRequest(SpaceState* s) {
            if (s && !s->requested) {
                s->requested = true;
                s->requestedAt = Clock::now();
            }
        }
        void requestCycle(ProtoSpace* space, SpaceState* s) {
            if (!space->gcStarted) {
                noteRequest(s);
                space->gcStarted = true;
                space->gcCV.notify_all();
            }
        }
        bool pacingDisabledByEnvironment() {
            const char* v = std::getenv("PROTOCORE_GC_PACING");
            return v && std::strcmp(v, "0") == 0;
        }
        SpaceState* ensureState(ProtoSpace* space) {
            SpaceState* s = find(space);
            if (!s) {
                s = new SpaceState();
                s->space = space;
                s->pacing = !pacingDisabledByEnvironment();
                s->lastCycleEnd = Clock::now();
                s->intervalStart = s->lastCycleEnd;
                s->cycleStart = s->lastCycleEnd;
                registry().push_back(s);
            }
            return s;
        }
    }  // namespace

    SoftZone softZoneDecision(const ProtoSpace* space) {
        SpaceState* s = findEnabled(space);
        if (!s) return SoftZone::Fixed;
        // A cycle requested or running: wait for it rather than grow.
        return space->gcStarted ? SoftZone::Wait : SoftZone::Grow;
    }

    bool isEnabled(const ProtoSpace* space) {
        return findEnabled(space) != nullptr;
    }

    std::uint64_t completedCycles(const ProtoSpace* space) {
        SpaceState* s = findEnabled(space);
        return s ? s->cycles : 0;
    }

    void markSoftWaitPending(ProtoSpace* space) {
        SpaceState* s = findEnabled(space);
        if (!s) return;
        if (!s->pending) {
            s->pending = true;
            softWaitPending.fetch_add(1, std::memory_order_relaxed);
        }
        requestCycle(space, s);
    }

    bool softWaitPendingFor(const ProtoSpace* space) {
        SpaceState* s = findEnabled(space);
        return s && s->pending;
    }


    void registerSpace(ProtoSpace* space) { (void) ensureState(space); }

    void noteCycleRequested(ProtoSpace* space) { noteRequest(find(space)); }

    std::uint64_t cyclesCompleted(const ProtoSpace* space) {
        SpaceState* s = find(space);
        return s ? s->completed : 0;
    }

    bool earlyWake(const ProtoSpace* space) {
        SpaceState* s = find(space);
        return s && s->pacing;
    }

    std::atomic<int> headroomWaitersTotal{0};

    void waitBegin(ProtoSpace* space) {
        headroomWaitersTotal.fetch_add(1, std::memory_order_relaxed);
        if (SpaceState* s = find(space)) {
            ++s->waiters;
            ++s->stats.waits;
        }
    }

    void waitEnd(ProtoSpace* space, WakeReason reason) {
        headroomWaitersTotal.fetch_sub(1, std::memory_order_relaxed);
        SpaceState* s = find(space);
        if (!s) return;
        --s->waiters;
        switch (reason) {
            case WakeReason::Watchdog: ++s->stats.watchdogWakes; break;
            case WakeReason::Cells: ++s->stats.cellWakes; break;
            case WakeReason::Cycle: ++s->stats.cycleWakes; break;
            case WakeReason::Ending: break;
        }
    }

    void cellsPublished(ProtoSpace* space) {
        SpaceState* s = find(space);
        if (!s || !s->pacing) return;
        if (s->waiters > 0) space->memoryReclaimedCV.notify_one();
        // A soft-zone wait pending under the controller (a refill went past
        // S without waiting) is for cells: they have arrived, so the next
        // critical-section checkpoints need not wait.  Left set, every
        // outermost checkpoint took globalMutex until the cycle ended and
        // returned at once (the freelist satisfies the wait): about 2
        // million empty waits per run on the single-threaded benchmark,
        // contending with the sweep's publications.
        clearPending(s);
    }

    void cellsReturned(ProtoSpace* space, proto_ulong cells) {
        SpaceState* s = find(space);
        if (!s) return;
        s->returnedSinceEnd += cells;
    }

    WaitStats waitStats(const ProtoSpace* space) {
        WaitStats r;
        if (SpaceState* s = find(space)) {
            r = s->stats;
            r.runway = s->runway;
            r.cyclesCompleted = s->completed;
            r.rate = s->rate[0];
            r.cycleSeconds = s->cycleSeconds[0];
        }
        return r;
    }

    void pace(ProtoSpace* space) {
        if (space->gcStarted) return;
        SpaceState* s = find(space);
        if (!s || !s->pacing || s->runway <= 0) return;
        // The ceiling: S under the controller, the hard limit otherwise.
        const long long ceiling = s->enabled
            ? static_cast<long long>(space->softHeapLimit)
            : static_cast<long long>(relaxedLoad(space->maxHeapSize));
        if (ceiling <= 0) return;
        const long long left = static_cast<long long>(space->freeCellsCount)
            + std::max(0LL, ceiling - static_cast<long long>(space->heapSize));
        if (left < s->runway) {
            ++s->stats.pacedRequests;
            requestCycle(space, s);
        }
    }

    void afterHeapGrowth(ProtoSpace* space, int cells) {
        processHeapCells += cells;
        if (processHeapCells > processHeapPeak) processHeapPeak = processHeapCells;
        if (enabledCount == 0) return;
        recomputeCeilings();
        SpaceState* s = findEnabled(space);
        if (!s) return;
        // A cycle starts when the heap reaches S, whether or not a thread
        // waits for it: a thread inside a critical section, or one that has
        // already waited once in this refill, grows the heap without asking.
        if (space->softHeapLimit > 0 && space->heapSize >= space->softHeapLimit)
            requestCycle(space, s);
    }

    void onCycleStart(ProtoSpace* space) {
        if (SpaceState* s = find(space)) {
            s->cycleStart = Clock::now();
            noteRequest(s);
        }
    }

    void recordMutatorWait(const ProtoSpace* space, std::uint64_t nanos) {
        if (SpaceState* s = find(space))
            s->waitNanos.fetch_add(nanos, std::memory_order_relaxed);
    }

    namespace {
        // The pacing signals of the interval that ends with this cycle
        // (section 4.2), and the runway for the next one.
        void updatePacing(ProtoSpace* space, SpaceState* s, Clock::time_point now,
                          std::uint64_t waits) {
            const long long occupied = static_cast<long long>(space->heapSize)
                                     - static_cast<long long>(space->freeCellsCount);
            const double interval = std::chrono::duration<double>(now - s->intervalStart).count();
            const double cycle = s->requested
                ? std::chrono::duration<double>(now - s->requestedAt).count()
                : std::chrono::duration<double>(now - s->cycleStart).count();
            // Cells handed out in the interval: the change of the occupied
            // cells plus what went back to the freelist meanwhile (the
            // cycle's reclaimed cells and the unused batches returned).
            const long long previous = s->occupiedLastEnd >= 0 ? s->occupiedLastEnd : 0;
            const long long handed = occupied - previous
                + static_cast<long long>(space->reclaimedLastCycle.load(std::memory_order_relaxed))
                + static_cast<long long>(s->returnedSinceEnd);
            // The mutators' time not spent waiting: the interval less the
            // mean wait per thread, never below a tenth of the interval.
            const double threads = static_cast<double>(
                std::max(1, space->runningThreads.load() + s->waiters));
            double active = interval - static_cast<double>(waits) / 1e9 / threads;
            active = std::max(active, interval * 0.1);
            const double r = (handed > 0 && active > 0.0) ? static_cast<double>(handed) / active : 0.0;
            s->rate[1] = s->rate[0];
            s->rate[0] = r;
            s->cycleSeconds[1] = s->cycleSeconds[0];
            s->cycleSeconds[0] = cycle;
            s->occupiedLastEnd = occupied;
            s->returnedSinceEnd = 0;
            s->intervalStart = now;
            s->requested = false;
            ++s->completed;
            s->waitShare = interval > 0.0
                ? static_cast<double>(waits) / 1e9 / (interval * threads) : 0.0;
        }

        // The runway for the next interval, from the ceiling in force now.
        void setRunway(ProtoSpace* space, SpaceState* s) {
            const long long ceiling = s->enabled
                ? static_cast<long long>(space->softHeapLimit)
                : static_cast<long long>(relaxedLoad(space->maxHeapSize));
            s->runway = ceiling > 0
                ? pacing::runway(ceiling, std::max(0LL, s->occupiedLastEnd),
                                 std::max(s->rate[0], s->rate[1]),
                                 std::max(s->cycleSeconds[0], s->cycleSeconds[1]))
                : 0;
        }
    }  // namespace

    long long processHeapPeakCells(bool reset) {
        const long long peak = processHeapPeak;
        if (reset) processHeapPeak = processHeapCells;
        return peak;
    }

    LawState lawState(const ProtoSpace* space) {
        SpaceState* s = findEnabled(space);
        return s ? s->law : LawState();
    }

    CycleMeasures lastCycleMeasures(const ProtoSpace* space) {
        SpaceState* s = find(space);
        return s ? s->last : CycleMeasures();
    }

    void onCycleEnd(ProtoSpace* space, const CycleMeasures& measures) {
        const std::uint64_t stopTheWorldNanos = measures.stwNanos;
        SpaceState* any = find(space);
        const Clock::time_point now = Clock::now();
        if (any) {
            any->last = measures;
            any->last.busyNanos = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - any->cycleStart).count());
        }
        SpaceState* s = (any && any->enabled) ? any : nullptr;
        if (!s) {
            const std::uint64_t waits = any ? any->waitNanos.exchange(0, std::memory_order_relaxed) : 0;
            if (any) {
                updatePacing(space, any, now, waits);
                setRunway(space, any);
            }
            // Fixed limits: PROTOCORE_HEAP_TRACE prints the cycle too, so a
            // fixed policy can be measured against the controller.
            const char* traceEnv = std::getenv("PROTOCORE_HEAP_TRACE");
            if (traceEnv && *traceEnv && std::strcmp(traceEnv, "0") != 0) {
                std::fprintf(stderr,
                    "protoCore heap: space=%p fixed cycle=%llu L=%" PROTO_FMT_U
                    " soft=%d hard=%d heap=%d stw=%.3fms r=%.0f C=%.3fms runway=%lld"
                    " waits=%llu watchdog=%llu paced=%llu\n",
                    static_cast<void*>(space),
                    static_cast<unsigned long long>(
                        space->gcCycleCount.load(std::memory_order_relaxed)),
                    static_cast<proto_ulong>(
                        space->liveCellsLastCycle.load(std::memory_order_relaxed)),
                    space->softHeapLimit, relaxedLoad(space->maxHeapSize),
                    relaxedLoad(space->heapSize), stopTheWorldNanos / 1e6,
                    any ? any->rate[0] : 0.0, any ? any->cycleSeconds[0] * 1e3 : 0.0,
                    any ? any->runway : 0LL,
                    any ? static_cast<unsigned long long>(any->stats.waits) : 0ULL,
                    any ? static_cast<unsigned long long>(any->stats.watchdogWakes) : 0ULL,
                    any ? static_cast<unsigned long long>(any->stats.pacedRequests) : 0ULL);
                std::fflush(stderr);
            }
            return;
        }
        const std::uint64_t waits = s->waitNanos.exchange(0, std::memory_order_relaxed);
        updatePacing(space, s, now, waits);
        const proto_ulong L = space->liveCellsLastCycle.load(std::memory_order_relaxed);
        const proto_ulong before = static_cast<proto_ulong>(std::max(0, space->softHeapLimit));
        LawInputs in;
        in.softCells = before;
        in.budgetCells = s->hardCells;
        in.liveCells = L;
        in.rate = std::max(s->rate[0], s->rate[1]);
        const double busy = static_cast<double>(s->last.busyNanos) / 1e9;
        in.throughput = busy > 0.0 ? static_cast<double>(measures.freedCells) / busy : 0.0;
        in.waitShare = s->waitShare;
        // Pacing's runway for the next interval, before the ceiling caps it.
        in.runwayCells = static_cast<double>(pacing::runway(
            static_cast<long long>(kMaxCells), 0, in.rate,
            std::max(s->cycleSeconds[0], s->cycleSeconds[1])));
        // The first cycle under the controller is not evidence about S: it
        // starts with no measured runway (pacing needs one cycle of r and C),
        // so its wait is the one every run pays, and its interval includes
        // the program's start-up.
        // The live-set floor applies to it all the same: a program whose
        // start-up live set sits just below S0 would otherwise run its next
        // cycle after a thin slice of allocation.
        const proto_ulong after = s->cycles == 0
            ? static_cast<proto_ulong>(raiseToFloor(static_cast<double>(before), before, L,
                                                    in.runwayCells, s->hardCells))
            : nextSoftLimit(in, s->law);
        // Stored relaxed: the sweep reads it without the lock (sweep::mutatorsShort).
        relaxedStore(space->softHeapLimit, static_cast<int>(std::min<proto_ulong>(after, kMaxCells)));
        s->lastThroughput = in.throughput;

        const long long retained = static_cast<long long>(space->heapSize)
                                 - static_cast<long long>(space->freeCellsCount);
        s->retainedLastCycle = static_cast<proto_ulong>(std::max(0LL, retained));
        setRunway(space, s);
        s->lastCycleEnd = now;
        s->lastPressure = s->waitShare;
        ++s->cycles;
        // The cycle a pending soft-zone wait was for has completed.
        clearPending(s);

        if (s->trace) {
            std::fprintf(stderr,
                "protoCore heap: space=%p cycle=%llu L=%" PROTO_FMT_U " w=%.4f r=%.0f T=%.0f"
                " rho=%.3f S=%" PROTO_FMT_U "->%d H=%" PROTO_FMT_U " heap=%d retained=%" PROTO_FMT_U
                " Tc=%.3fms C=%.3fms runway=%lld probes=%llu stopped=%d stw=%.3fms\n",
                static_cast<void*>(space), static_cast<unsigned long long>(s->cycles),
                L, in.waitShare, in.rate, in.throughput,
                in.throughput > 0.0 ? in.rate / in.throughput : 0.0,
                before, space->softHeapLimit, s->hardCells, space->heapSize, s->retainedLastCycle,
                std::chrono::duration<double, std::milli>(now - s->cycleStart).count(),
                s->cycleSeconds[0] * 1e3, s->runway,
                static_cast<unsigned long long>(s->law.probes), s->law.stopped ? 1 : 0,
                stopTheWorldNanos / 1e6);
            std::fflush(stderr);
        }
    }

    bool liveSetExceedsBudget(const ProtoSpace* space, bool& known,
                              proto_ulong& occupiedCells, proto_ulong& budgetCells) {
        SpaceState* s = findEnabled(space);
        known = s != nullptr;
        if (!s) return false;
        const long long H = static_cast<long long>(processBudgetCells);
        const long long others =
            std::max(0LL, processHeapCells - static_cast<long long>(space->heapSize));
        const long long occupied = static_cast<long long>(s->retainedLastCycle) + others;
        // One refill batch per running thread: the cells threads hold in
        // their private freelists, beyond any collector's reach (the batch
        // cap of getFreeCells: H / (8 x threads), within [512, 65536]).
        const long long threads = std::max(1, space->runningThreads.load());
        long long batch = H / (8 * threads);
        batch = std::max(512LL, std::min(65536LL, batch));
        const long long margin = threads * batch;
        occupiedCells = static_cast<proto_ulong>(occupied);
        budgetCells = static_cast<proto_ulong>(H);
        return occupied > H - margin;
    }

    int environmentMode() {
        const char* v = std::getenv("PROTOCORE_ADAPTIVE_HEAP");
        if (!v) return -1;
        if (std::strcmp(v, "1") == 0) return 1;
        if (std::strcmp(v, "0") == 0) return 0;
        return -1;
    }

    void enable(ProtoSpace* space, const AdaptiveHeapConfig& config) {
        // H: the environment, then the configuration, then automatic.
        int envSoft = 0;
        int envHard = 0;
        const bool fromEnv =
            parseHeapLimitCells(std::getenv("PROTOCORE_HEAP_LIMIT_CELLS"), envSoft, envHard)
            && envHard > 0;
        proto_ulong H;
        if (fromEnv) H = static_cast<proto_ulong>(envHard);
        else if (config.hardCells > 0) H = config.hardCells;
        else H = defaultHardCells(physicalMemoryBytes(), processMemoryLimitBytes());
        if (H > kMaxCells) H = kMaxCells;
        if (H < 1) H = 1;

        proto_ulong S0;
        if (fromEnv && envSoft > 0) S0 = static_cast<proto_ulong>(envSoft);
        else if (config.initialSoftCells > 0) S0 = config.initialSoftCells;
        else S0 = kDefaultInitialSoftCells;
        // An embedder or operator that knows the run fits starts at the
        // budget (no speculative growth otherwise): initialSoftCells at or
        // above H, or PROTOCORE_ADAPTIVE_HEAP_START=budget.
        if (const char* start = std::getenv("PROTOCORE_ADAPTIVE_HEAP_START")) {
            if (std::strcmp(start, "budget") == 0) S0 = H;
        }
        if (S0 > H) S0 = H;

        // PROTOCORE_ADAPTIVE_HEAP=0: diagnosis without the controller -- the
        // same H as a fixed hard limit, no soft watermark.
        if (environmentMode() == 0) {
            space->setHeapLimits(0, static_cast<int>(H));
            return;
        }

        SpaceState* s = ensureState(space);
        if (!s->enabled) {
            s->enabled = true;
            ++enabledCount;
        }
        s->law = LawState();
        s->hardCells = H;
        const char* traceEnv = std::getenv("PROTOCORE_HEAP_TRACE");
        s->trace = traceEnv && std::strcmp(traceEnv, "0") != 0 && *traceEnv;
        s->waitNanos.store(0, std::memory_order_relaxed);
        s->lastCycleEnd = Clock::now();
        s->cycleStart = s->lastCycleEnd;
        s->lastPressure = 0.0;
        s->retainedLastCycle = static_cast<proto_ulong>(std::max(0, space->heapSize));

        processBudgetCells = H;
        for (SpaceState* other : registry())
            if (other->enabled) other->hardCells = H;
        relaxedStore(space->softHeapLimit, static_cast<int>(S0));
        relaxedStore(space->maxHeapSize, static_cast<int>(H));
        recomputeCeilings();
    }

    void disable(ProtoSpace* space) {
        SpaceState* s = find(space);
        if (s && s->enabled) {
            clearPending(s);
            s->enabled = false;
            --enabledCount;
        }
    }

    AdaptiveHeapStats stats(const ProtoSpace* space) {
        AdaptiveHeapStats r;
        r.softCells = static_cast<proto_ulong>(std::max(0, space->softHeapLimit));
        r.heapCells = static_cast<proto_ulong>(std::max(0, space->heapSize));
        r.liveCellsLastCycle = space->liveCellsLastCycle.load(std::memory_order_relaxed);
        if (SpaceState* s = findEnabled(space)) {
            r.enabled = true;
            r.hardCells = s->hardCells;
            r.cycles = s->cycles;
            r.lastPressure = s->lastPressure;
        } else {
            r.hardCells = static_cast<proto_ulong>(std::max(0, space->maxHeapSize));
        }
        return r;
    }

    void forgetSpace(ProtoSpace* space) {
        processHeapCells -= space->heapSize;
        if (processHeapCells < 0) processHeapCells = 0;
        auto& states = registry();
        for (size_t i = 0; i < states.size(); ++i) {
            if (states[i]->space != space) continue;
            clearPending(states[i]);
            if (states[i]->enabled) --enabledCount;
            delete states[i];
            states.erase(states.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        // The budget this space held is free for the others.
        recomputeCeilings();
    }

}  // namespace adaptive
}  // namespace proto
