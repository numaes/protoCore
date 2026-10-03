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

    /** Initial soft limit when none is configured: 32 MiB worth of cells. */
    constexpr proto_ulong kDefaultInitialSoftCells = 524288;
    /** Largest hard limit: cell counts are `int` in ABI 3 (128 GiB). */
    constexpr proto_ulong kMaxCells = 2147483647UL;

    struct LawParams {
        double highPressure = 0.05;  // p_high
        double growthFactor = 1.5;   // g
        double liveHeadroom = 1.5;   // k_live
    };

    /**
     * The soft limit after a cycle (spec section 3.3):
     *
     *     floor = ceil(k_live * L)
     *     p > p_high:  S' = min(H, max(floor, ceil(S * g)))
     *     otherwise:   S' = min(H, max(S, floor))
     *
     * Never below S while S <= H, never above H.
     */
    proto_ulong nextSoftLimit(proto_ulong softCells, proto_ulong hardCells,
                              proto_ulong liveCells, double pressure,
                              const LawParams& params);

    /** The parameters of a configuration, invalid values replaced by the
     *  defaults (p_high in (0, 1], g > 1, k_live >= 1). */
    LawParams sanitizedParams(const AdaptiveHeapConfig& config);

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
     * the cells left before S -- the global freelist plus the room between
     * heapSize and S -- fall below half of the headroom S - L the last cycle
     * left.  The mutators then consume the other half while the collector
     * runs concurrently; requesting only at S itself left no runway, and the
     * stall grew with S (the sweep is proportional to the garbage).
     */
    void pace(ProtoSpace* space);

    /** Called after `space` grew its heap by `cells` (getFreeCells).  Keeps
     *  the process heap total, recomputes the per-space ceilings of enabled
     *  spaces under the process budget, and requests a cycle when an
     *  enabled space's heap has reached its soft limit. */
    void afterHeapGrowth(ProtoSpace* space, int cells);
    /** Mutator stall: time a thread of `space` waited for a cycle. */
    void recordMutatorWait(const ProtoSpace* space, std::uint64_t nanos);
    /** End of a cycle of `space`: apply the control law. */
    void onCycleEnd(ProtoSpace* space, std::uint64_t stopTheWorldNanos);
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

    // Used by ProtoSpace::enableAdaptiveHeap / setHeapLimits / stats.
    void enable(ProtoSpace* space, const AdaptiveHeapConfig& config);
    void disable(ProtoSpace* space);
    AdaptiveHeapStats stats(const ProtoSpace* space);

}  // namespace adaptive
}  // namespace proto

#endif
