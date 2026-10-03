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

    proto_ulong nextSoftLimit(proto_ulong softCells, proto_ulong hardCells,
                              proto_ulong liveCells, double pressure,
                              const LawParams& params) {
        // Doubles hold every value involved exactly enough: cell counts are
        // below 2^31, far inside the 53-bit mantissa.
        const double hard = static_cast<double>(hardCells);
        const double floorCells =
            std::ceil(params.liveHeadroom * static_cast<double>(liveCells));
        double next;
        if (pressure > params.highPressure) {
            const double grown =
                std::ceil(params.growthFactor * static_cast<double>(softCells));
            next = std::max(floorCells, grown);
        } else {
            next = std::max(static_cast<double>(softCells), floorCells);
        }
        next = std::min(hard, next);
        return static_cast<proto_ulong>(next);
    }

    LawParams sanitizedParams(const AdaptiveHeapConfig& config) {
        LawParams p;
        if (config.highPressure > 0.0 && config.highPressure <= 1.0)
            p.highPressure = config.highPressure;
        if (config.growthFactor > 1.0 && config.growthFactor <= 16.0)
            p.growthFactor = config.growthFactor;
        if (config.liveHeadroom >= 1.0 && config.liveHeadroom <= 16.0)
            p.liveHeadroom = config.liveHeadroom;
        return p;
    }

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
            LawParams params;
            proto_ulong hardCells = 0;
            // Mutator waits since the last cycle end, in nanoseconds.
            std::atomic<std::uint64_t> waitNanos{0};
            Clock::time_point lastCycleEnd;
            std::uint64_t cycles = 0;
            double lastPressure = 0.0;
            // A refill went past S without waiting; cleared at cycle end.
            bool pending = false;
            // pace(): a cycle is requested when the cells left before S
            // fall below this (half of S - L after the last cycle).
            long long triggerRunway = 0;
            // Cells the last cycle left unreclaimed: heapSize minus the
            // global freelist, right after the sweep published its cells.
            proto_ulong retainedLastCycle = 0;
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
        void requestCycle(ProtoSpace* space) {
            if (!space->gcStarted) {
                space->gcStarted = true;
                space->gcCV.notify_all();
            }
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
        requestCycle(space);
    }

    bool softWaitPendingFor(const ProtoSpace* space) {
        SpaceState* s = findEnabled(space);
        return s && s->pending;
    }

    /** The fraction of the headroom S - L consumed before the next cycle
     *  is requested; the rest is the mutators' runway during the cycle. */
    constexpr double kTriggerFraction = 0.5;

    namespace {
        long long runwayFor(proto_ulong soft, proto_ulong live) {
            const long long headroom = static_cast<long long>(soft) - static_cast<long long>(live);
            return headroom > 0 ? static_cast<long long>(headroom * (1.0 - kTriggerFraction)) : 0;
        }
    }

    void pace(ProtoSpace* space) {
        if (enabledCount == 0) return;
        SpaceState* s = findEnabled(space);
        if (!s || space->gcStarted) return;
        const long long left = static_cast<long long>(space->freeCellsCount)
            + std::max(0LL, static_cast<long long>(space->softHeapLimit) - space->heapSize);
        if (left < s->triggerRunway) requestCycle(space);
    }

    void afterHeapGrowth(ProtoSpace* space, int cells) {
        processHeapCells += cells;
        if (enabledCount == 0) return;
        recomputeCeilings();
        SpaceState* s = findEnabled(space);
        if (!s) return;
        // A cycle starts when the heap reaches S, whether or not a thread
        // waits for it: a thread inside a critical section, or one that has
        // already waited once in this refill, grows the heap without asking.
        if (space->softHeapLimit > 0 && space->heapSize >= space->softHeapLimit)
            requestCycle(space);
    }

    void recordMutatorWait(const ProtoSpace* space, std::uint64_t nanos) {
        if (SpaceState* s = findEnabled(space))
            s->waitNanos.fetch_add(nanos, std::memory_order_relaxed);
    }

    void onCycleEnd(ProtoSpace* space, std::uint64_t stopTheWorldNanos) {
        SpaceState* s = findEnabled(space);
        if (!s) {
            // Fixed limits: PROTOCORE_HEAP_TRACE prints the cycle too, so a
            // fixed policy can be measured against the controller.
            const char* traceEnv = std::getenv("PROTOCORE_HEAP_TRACE");
            if (traceEnv && *traceEnv && std::strcmp(traceEnv, "0") != 0) {
                std::fprintf(stderr,
                    "protoCore heap: space=%p fixed cycle=%llu L=%" PROTO_FMT_U
                    " soft=%d hard=%d heap=%d stw=%.3fms\n",
                    static_cast<void*>(space),
                    static_cast<unsigned long long>(
                        space->gcCycleCount.load(std::memory_order_relaxed)),
                    static_cast<proto_ulong>(
                        space->liveCellsLastCycle.load(std::memory_order_relaxed)),
                    space->softHeapLimit, relaxedLoad(space->maxHeapSize),
                    relaxedLoad(space->heapSize), stopTheWorldNanos / 1e6);
                std::fflush(stderr);
            }
            return;
        }
        const Clock::time_point now = Clock::now();
        const double T = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - s->lastCycleEnd).count());
        const std::uint64_t waits = s->waitNanos.exchange(0, std::memory_order_relaxed);
        const double P = static_cast<double>(stopTheWorldNanos + waits);
        const double p = T > 0.0 ? P / T : 0.0;
        const proto_ulong L = space->liveCellsLastCycle.load(std::memory_order_relaxed);
        const proto_ulong before = static_cast<proto_ulong>(std::max(0, space->softHeapLimit));
        const proto_ulong after = nextSoftLimit(before, s->hardCells, L, p, s->params);
        space->softHeapLimit = static_cast<int>(std::min<proto_ulong>(after, kMaxCells));

        const long long retained = static_cast<long long>(space->heapSize)
                                 - static_cast<long long>(space->freeCellsCount);
        s->retainedLastCycle = static_cast<proto_ulong>(std::max(0LL, retained));
        s->triggerRunway = runwayFor(static_cast<proto_ulong>(space->softHeapLimit), L);
        s->lastCycleEnd = now;
        s->lastPressure = p;
        ++s->cycles;
        // The cycle a pending soft-zone wait was for has completed.
        clearPending(s);

        if (s->trace) {
            std::fprintf(stderr,
                "protoCore heap: space=%p cycle=%llu L=%" PROTO_FMT_U " T=%.3fms "
                "P=%.3fms p=%.4f S=%" PROTO_FMT_U "->%d H=%" PROTO_FMT_U
                " heap=%d retained=%" PROTO_FMT_U "\n",
                static_cast<void*>(space), static_cast<unsigned long long>(s->cycles),
                L, T / 1e6, P / 1e6, p, before, space->softHeapLimit,
                s->hardCells, space->heapSize, s->retainedLastCycle);
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
        if (S0 > H) S0 = H;

        // PROTOCORE_ADAPTIVE_HEAP=0: diagnosis without the controller -- the
        // same H as a fixed hard limit, no soft watermark.
        if (environmentMode() == 0) {
            space->setHeapLimits(0, static_cast<int>(H));
            return;
        }

        SpaceState* s = find(space);
        if (!s) {
            s = new SpaceState();
            s->space = space;
            registry().push_back(s);
        }
        if (!s->enabled) {
            s->enabled = true;
            ++enabledCount;
        }
        s->params = sanitizedParams(config);
        s->hardCells = H;
        const char* traceEnv = std::getenv("PROTOCORE_HEAP_TRACE");
        s->trace = traceEnv && std::strcmp(traceEnv, "0") != 0 && *traceEnv;
        s->waitNanos.store(0, std::memory_order_relaxed);
        s->lastCycleEnd = Clock::now();
        s->lastPressure = 0.0;
        s->retainedLastCycle = static_cast<proto_ulong>(std::max(0, space->heapSize));

        processBudgetCells = H;
        for (SpaceState* other : registry())
            if (other->enabled) other->hardCells = H;
        space->softHeapLimit = static_cast<int>(S0);
        s->triggerRunway = runwayFor(S0, space->liveCellsLastCycle.load(std::memory_order_relaxed));
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
