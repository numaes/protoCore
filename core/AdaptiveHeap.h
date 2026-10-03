/*
 * AdaptiveHeap.h -- the adaptive heap controller (internal).
 *
 * Three parts, each testable on its own:
 *
 *   1. The control law (nextSoftLimit): a pure function of the soft limit S,
 *      the hard limit H, the live set L and the pressure p.
 *   2. Memory-limit detection: physical memory and the process memory limit
 *      (cgroup v1/v2 on Linux, job object on Windows), and the default hard
 *      limit derived from them.  The cgroup parsers read from an injectable
 *      root directory so the tests can feed them a fake /proc and /sys.
 *   3. The per-space controller state, kept OUTSIDE ProtoSpace's layout (ABI
 *      3 is unchanged) in a registry owned by this file, and the hooks the
 *      allocator and the collector call.  Every hook is called with
 *      ProtoSpace::globalMutex held.
 *
 * Design: docs/specs/2026-10-02-adaptive-heap-controller-design.md.
 */

#ifndef PROTOCORE_ADAPTIVE_HEAP_H
#define PROTOCORE_ADAPTIVE_HEAP_H

#include "../headers/protoCore.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace proto {
namespace adaptive {

    // --- 1. Control law ------------------------------------------------------

    /** Initial soft limit when none is configured: 128 MiB worth of cells. */
    constexpr proto_ulong kDefaultInitialSoftCells = 2097152;
    /** Largest hard limit: cell counts are `int` in ABI 3 (128 GiB). */
    constexpr proto_ulong kMaxCells = 2147483647UL;
    /** k_live when none is configured: S never falls below 3 x L. */
    constexpr double kDefaultLiveHeadroom = 3.0;
    /**
     * Pressure grows S only up to this multiple of the live set (or S0, or
     * the floor, whichever is larger).  Past it, a stall is the collector's
     * throughput falling short of the allocation rate, which no soft limit
     * removes: measured, S then ran to 20 x L and beyond while the stall
     * reappeared at every size (docs/reports/2026-10-03-adaptive-heap-
     * calibration.md).
     */
    constexpr double kLiveCapFactor = 8.0;

    struct LawParams {
        double highPressure = 0.05;                    // p_high
        double growthFactor = 1.5;                     // g
        double liveHeadroom = kDefaultLiveHeadroom;    // k_live
        double liveCap = kLiveCapFactor;               // k_cap >= k_live
        proto_ulong initialSoft = kDefaultInitialSoftCells;  // S0
    };

    /**
     * The soft limit after a cycle (spec section 3.3, as calibrated in 2.10.1):
     *
     *     floor = ceil(k_live * L)
     *     cap   = max(S0, ceil(k_cap * L), floor)
     *     p > p_high:  S' = min(H, max(S, floor, min(ceil(S * g), cap)))
     *     otherwise:   S' = min(H, max(S, floor))
     *
     * Never below S while S <= H, never above H.
     */
    proto_ulong nextSoftLimit(proto_ulong softCells, proto_ulong hardCells,
                              proto_ulong liveCells, double pressure,
                              const LawParams& params);

    /** The parameters of a configuration, invalid values replaced by the
     *  defaults (p_high in (0, 1], g > 1, k_live >= 1); k_cap is
     *  max(kLiveCapFactor, k_live) and S0 is left at its default (the
     *  caller sets it). */
    LawParams sanitizedParams(const AdaptiveHeapConfig& config);

    // --- 1b. Pacing: when a cycle starts -------------------------------------
    //
    // docs/specs/2026-10-03-collector-throughput-design.md, section 4.3.  One
    // rule for fixed limits and for the controller: a cycle is requested when
    // the cells left before the ceiling fall below the runway the mutators
    // need while a cycle runs, r x C, so the cycle runs behind them instead
    // of starting when they are already out of cells.

    namespace pacing {
        /** m: a quarter of a cycle of measurement error.  Structural, not
         *  fitted to a workload. */
        constexpr double kCycleSlack = 0.25;

        /**
         * The runway, in cells:
         *
         *     min(max(0, ceiling - retained), ceil(rate x cycleSeconds x (1 + slack)))
         *
         * `ceiling` is S under the controller and maxHeapSize under a fixed
         * limit; `retained` is what the last cycle left unreclaimed (R);
         * `rate` is the mutators' allocation rate while not waiting (cells per
         * second) and `cycleSeconds` the cycle duration from request to
         * completion.  0 when either measurement is missing (before the first
         * cycle), which is the pre-pacing behaviour.  Never above the headroom
         * `ceiling - retained`; monotone in `rate` and `cycleSeconds`.  A
         * runway equal to the whole headroom means the collector cannot keep
         * up at this ceiling: cycles then run back to back.
         */
        long long runway(long long ceiling, long long retained, double rate,
                         double cycleSeconds, double slack = kCycleSlack);
    }  // namespace pacing

    // --- 2. Memory limits ----------------------------------------------------

    /** A cgroup v2 `memory.max` value in bytes; 0 for "max" or unparsable. */
    std::uint64_t parseCgroupV2Max(const std::string& content);
    /** A cgroup v1 `memory.limit_in_bytes` value; 0 for "unlimited" (any
     *  value of 2^60 or more, the page-rounded LONG_MAX) or unparsable. */
    std::uint64_t parseCgroupV1Limit(const std::string& content);
    /**
     * The cgroup path of this process from the contents of /proc/self/cgroup.
     * `v2` selects the unified-hierarchy line ("0::<path>"); otherwise the v1
     * line whose controller list contains "memory".  Empty when absent.
     */
    std::string cgroupPathFromProcSelf(const std::string& procSelfCgroup, bool v2);
    /**
     * The memory limit of this process's cgroup, in bytes, or 0 when there is
     * none.  `root` prefixes every path read ("" for the real file system):
     * <root>/proc/self/cgroup, then <root>/sys/fs/cgroup<path>/memory.max
     * (v2) or <root>/sys/fs/cgroup/memory<path>/memory.limit_in_bytes (v1).
     * The cgroup and every ancestor up to the mount point are consulted and
     * the smallest limit wins, because a parent's limit binds its children;
     * walking up also finds the limit in a container whose cgroup namespace
     * hides the host path.
     */
    std::uint64_t cgroupMemoryLimit(const std::string& root);

    /** Physical memory in bytes, 0 if unknown. */
    std::uint64_t physicalMemoryBytes();
    /** The process memory limit in bytes (cgroup on Linux, job object on
     *  Windows), 0 when none applies (always 0 on macOS). */
    std::uint64_t processMemoryLimitBytes();
    /**
     * The automatic hard limit, in cells: 75 % of the smaller of `physical`
     * and `limit` (0 = no limit), divided by the 64-byte cell, clamped to
     * [1, kMaxCells].  With nothing known (both 0), 8 GiB worth of cells.
     */
    proto_ulong defaultHardCells(std::uint64_t physicalBytes, std::uint64_t limitBytes);

    /**
     * Parse PROTOCORE_HEAP_LIMIT_CELLS: "<hard>" or "<soft>,<hard>", decimal
     * digits, each at most INT_MAX.  Returns false for anything else.  A
     * missing soft part yields soft = 0.
     */
    bool parseHeapLimitCells(const char* text, int& softCells, int& hardCells);

    // --- 3. Per-space state and hooks (globalMutex held) ---------------------

    /** What a refill that must grow the heap at or above S does. */
    enum class SoftZone {
        Fixed,  // the controller is not enabled: the fixed-limit soft zone
        Wait,   // a cycle is requested or running: wait for it
        Grow    // no cycle pending (one was waited for, or none is due): grow
    };
    SoftZone softZoneDecision(const ProtoSpace* space);
    /** The controller is enabled for `space` (globalMutex held). */
    bool isEnabled(const ProtoSpace* space);
    /** Cycles of `space` completed while enabled (globalMutex held). */
    std::uint64_t completedCycles(const ProtoSpace* space);
    /** A refill went past S without waiting: request a cycle and make the
     *  thread's next critical-section checkpoint wait for it. */
    void markSoftWaitPending(ProtoSpace* space);
    /** True when `space` has a soft-zone wait pending (globalMutex held). */
    bool softWaitPendingFor(const ProtoSpace* space);
    /** Spaces with a soft-zone wait pending, process-wide.  Read relaxed by
     *  ProtoContext::heapLimitCheckpoint on every outermost critical-section
     *  entry of a space with a hard limit; written under globalMutex. */
    extern std::atomic<int> softWaitPending;
    /** The checkpoint's wait (defined in ProtoSpace.cpp, which owns the
     *  reclaim wait).  Takes globalMutex; the caller holds no lock and is at
     *  criticalSectionDepth 0. */
    void softZoneCheckpoint(ProtoSpace* space, ProtoContext* ctx);

    /**
     * Pacing (called by getFreeCells at every refill): request a cycle once
     * the cells left before the ceiling -- the global freelist plus the room
     * between heapSize and the ceiling (S under the controller, maxHeapSize
     * under a fixed limit) -- fall below the runway pacing::runway computed
     * at the last cycle end.  Requesting only at the ceiling left the
     * mutators no runway: they stopped for the whole cycle.
     */
    void pace(ProtoSpace* space);

    /** Every space has a side state (created by its constructor), whatever
     *  its limits: pacing and the wait accounting apply to fixed limits too. */
    void registerSpace(ProtoSpace* space);
    /** A cycle of `space` was requested (gcStarted set); the cycle duration C
     *  runs from the first request to the completion. */
    void noteCycleRequested(ProtoSpace* space);
    /** Cycles completed by this space's collector, enabled or not. */
    std::uint64_t cyclesCompleted(const ProtoSpace* space);

    /**
     * Waits for headroom that end when cells arrive (section 4.4).  A thread
     * in reclaimWaitLocked is counted from waitBegin to waitEnd, both under
     * globalMutex.  Every publication of free cells (cellsPublished, with
     * globalMutex held) wakes one waiter while the count is non-zero, and the
     * wait predicate accepts a non-empty freelist, so a waiter no longer
     * polls the freelist on the 50 ms watchdog while the sweep publishes.
     */
    bool earlyWake(const ProtoSpace* space);
    void waitBegin(ProtoSpace* space);
    /** Why a headroom wait ended. */
    enum class WakeReason {
        Watchdog,  // the 50 ms timeout, with the predicate still false
        Cells,     // cells were published to the freelist (early wake)
        Cycle,     // a cycle started (fixed limits) or completed
        Ending     // the space is being destroyed
    };
    void waitEnd(ProtoSpace* space, WakeReason reason);
    /** Free cells were published to `space`'s freelist (globalMutex held). */
    void cellsPublished(ProtoSpace* space);
    /** A thread or a context returned `cells` unused cells to the freelist
     *  (globalMutex held): counted for the allocation-rate estimate.  The
     *  caller also calls cellsPublished. */
    void cellsReturned(ProtoSpace* space, proto_ulong cells);

    /** Wait and pacing counters of a space, for tests and the trace. */
    struct WaitStats {
        std::uint64_t waits = 0;            // reclaimWaitLocked calls
        std::uint64_t watchdogWakes = 0;    // ended on the 50 ms watchdog
        std::uint64_t cellWakes = 0;        // ended on a publication of cells
        std::uint64_t cycleWakes = 0;       // ended on a cycle start or end
        std::uint64_t pacedRequests = 0;    // cycles requested by pace()
        long long runway = 0;               // the current runway, cells
        std::uint64_t cyclesCompleted = 0;
        double rate = 0.0;                  // last r, cells per second
        double cycleSeconds = 0.0;          // last C
    };
    WaitStats waitStats(const ProtoSpace* space);

    /** Called after `space` grew its heap by `cells` (getFreeCells).  Keeps
     *  the process heap total, recomputes the per-space ceilings of enabled
     *  spaces under the process budget, and requests a cycle when an
     *  enabled space's heap has reached its soft limit. */
    void afterHeapGrowth(ProtoSpace* space, int cells);
    /** Start of a cycle of `space` (the collector holds the cycle token);
     *  the trace reports the cycle's duration.  globalMutex held. */
    void onCycleStart(ProtoSpace* space);
    /** Mutator stall: time a thread of `space` waited for a cycle. */
    void recordMutatorWait(const ProtoSpace* space, std::uint64_t nanos);
    /** What the collector measured in one cycle: three clock reads and a
     *  per-cell counter in the sweep loop, always on (section 4.2). */
    struct CycleMeasures {
        std::uint64_t stwNanos = 0;     // the pause (Phase 1 quorum reached -> resume)
        std::uint64_t markNanos = 0;    // resume -> sweep start (young walk + trace)
        std::uint64_t sweepNanos = 0;   // Phase 5, the trailing chunk included
        std::uint64_t busyNanos = 0;    // token taken -> cycle end
        proto_ulong sweptCells = 0;     // candidates examined, survivors included
        proto_ulong freedCells = 0;     // returned to the freelist
        proto_ulong sweptSegments = 0;
    };
    /** End of a cycle of `space`: the pacing signals, and the control law
     *  when the controller is enabled. */
    void onCycleEnd(ProtoSpace* space, const CycleMeasures& measures);
    /** The measures of `space`'s last completed cycle (globalMutex held). */
    CycleMeasures lastCycleMeasures(const ProtoSpace* space);
    /**
     * Section 3.4, condition 1, for an enabled space: after a completed
     * cycle, the cells that cycle left unreclaimed in `space`, plus every
     * other space's whole heap (memory is never moved between spaces), exceed
     * H minus one refill batch per running thread.  Sets `known` to false
     * when the controller is not enabled for `space` (the caller then keeps
     * the fixed-limit rule).
     */
    bool liveSetExceedsBudget(const ProtoSpace* space, bool& known,
                              proto_ulong& occupiedCells, proto_ulong& budgetCells);
    /** Remove `space` (its destructor, after the collector has joined). */
    void forgetSpace(ProtoSpace* space);

    /**
     * PROTOCORE_ADAPTIVE_HEAP, a diagnostic switch: 1 when it is "1" (every
     * space enables the controller when it is created, and setHeapLimits
     * leaves it enabled), 0 when it is "0" (enableAdaptiveHeap applies H as
     * a fixed limit), -1 when unset or any other value.
     */
    int environmentMode();

    // Used by ProtoSpace::enableAdaptiveHeap / setHeapLimits / stats.
    void enable(ProtoSpace* space, const AdaptiveHeapConfig& config);
    void disable(ProtoSpace* space);
    AdaptiveHeapStats stats(const ProtoSpace* space);

}  // namespace adaptive
}  // namespace proto

#endif
