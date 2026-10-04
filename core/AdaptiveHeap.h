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

    /**
     * The control law (docs/specs/2026-10-03-collector-throughput-design.md,
     * section 4.5; it replaces the 2.10.1 law and its fitted constants
     * k_live, k_cap and p_high).  Objective: minimise the time mutators lose
     * to collection within the budget B (the hard limit H).
     *
     * Inputs, measured at each cycle end: the live set L, the mutators'
     * allocation rate r (cells per second while not waiting), the collector's
     * reclamation throughput T (cells freed per second of collector busy
     * time) and the mutators' wait share w (wait time per thread over the
     * interval).
     *
     *   rho = r / T
     *   rho < 1 (regime 1: the collector keeps up at this size) and the
     *   mutators waited (w > 0; without waits S is kept):
     *       G*     = rho x L / (1 - rho)   the runway a cycle needs to hide
     *       target = min(B, L + G* x (1 + m))
     *       S'     = max(S, target)
     *   rho >= 1 (regime 2): a probe -- S' = min(B, 2 S) -- while the
     *       mutators wait (w > 0) and growth is not stopped.
     *   After a probe, a wait share that did not fall below the one before
     *   the probe counts as a non-improving probe; two in a row stop growth
     *   (S is held: a larger heap only lengthens cycles).  A change of L or
     *   r by a factor of 2 since the stop re-arms growth; the same change
     *   between a probe and its verdict voids the verdict (the workload
     *   moved, not S).
     *
     * S never decreases and never exceeds B.  The constants are structural:
     * m = kCycleSlack, doubling, two probes, a factor of 2.
     */
    constexpr double kCycleSlack = 0.25;
    constexpr int kNonImprovingProbesToStop = 2;
    constexpr double kRearmFactor = 2.0;

    struct LawInputs {
        proto_ulong softCells = 0;     // S
        proto_ulong budgetCells = 0;   // B
        proto_ulong liveCells = 0;     // L
        double rate = 0.0;             // r, cells per second
        double throughput = 0.0;       // T, cells per second of busy time
        double waitShare = 0.0;        // w
        double runwayCells = 0.0;      // pacing's runway before the ceiling caps it (cells)
    };

    /** The law's memory between cycles. */
    struct LawState {
        bool stopped = false;
        int nonImproving = 0;
        bool probePending = false;
        double waitBeforeProbe = 0.0;
        proto_ulong liveAtStop = 0;
        double rateAtStop = 0.0;
        std::uint64_t changes = 0;     // times S changed
        proto_ulong liveAtProbe = 0;
        double rateAtProbe = 0.0;
        std::uint64_t probes = 0;
        std::uint64_t rearms = 0;
        std::uint64_t voidProbes = 0;  // verdicts discarded: the workload moved
    };

    /** The soft limit after a cycle; updates `state`.  Pure: no clock, no
     *  space.  Never below liveSetFloor(L, runway, B). */
    proto_ulong nextSoftLimit(const LawInputs& in, LawState& state);

    /**
     * The live-set floor (since 2.14.2): after every cycle, the first
     * included, with or without waits,
     *
     *     S >= min(B, 2 L + runway)
     *
     * so that a cycle paced to start `runway` cells before S finds at least
     * as much garbage as the live set it marks.  Below it the cycles follow
     * each other after a thin slice of allocation, each marking the whole
     * live set to reclaim less than it: with a program's start-up live set
     * just below S0 (protoST under the controller, 2026-10-04
     * re-measurement), and pacing hides those cycles from the waits, so rule
     * 4.1 (no wait, no growth) alone never raises S.  The factor is
     * structural: the point where a cycle reclaims as many cells as it marks.
     * A soft limit below the floor is raised to max(floor, 2 S), within B:
     * the runway is measured and noisy, and a floor that creeps with it would
     * otherwise change S at every cycle.  So the floor's changes are
     * doublings at least, counted with the probes' in the bound on changes.
     */
    constexpr double kLiveSetFloorFactor = 2.0;
    proto_ulong liveSetFloor(proto_ulong liveCells, double runwayCells, proto_ulong budgetCells);

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
        constexpr double kCycleSlack = adaptive::kCycleSlack;

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

    /**
     * A refill of `space` by `who` (its thread, or its context without one;
     * getFreeCells, globalMutex held): counts whether more than one thread
     * allocated since the last cycle end (since 2.14.2).
     */
    void noteRefill(ProtoSpace* space, const void* who);
    /** More than one thread of `space` allocated since its last cycle end;
     *  true when unknown (globalMutex held).  The sweep walks one chain only
     *  for a single allocating thread (sweep::mutatorsShort). */
    bool severalAllocators(const ProtoSpace* space);

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
    /** Threads of any space in reclaimWaitLocked now (relaxed; the parallel
     *  sweep engages its helpers while this is non-zero). */
    extern std::atomic<int> headroomWaitersTotal;
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
    /** The largest sum of all spaces' heaps seen at a heap growth since the
     *  last reset; `reset` restarts it from the current sum (globalMutex
     *  held).  The budget property P2 of the spec. */
    long long processHeapPeakCells(bool reset);
    /** The control law's state of an enabled `space` (globalMutex held). */
    LawState lawState(const ProtoSpace* space);
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
