// CycleDriver.h -- forcing a cycle is not the same as submitting, and
// "reclaimed something" is not the same as "reclaimed the garbage".
//
// Track Y measured a forced cycle reclaiming 4-7 cells where the workload had
// created 205,120, and the test asserted `reclaimed > 0` and passed.
// protoCore's own GCSurvivorRechainTests.cpp still asserts only
// EXPECT_GT(freeAfter, freeBefore), which a single cell satisfies.  The same
// vacuity has now been found five times in this project.
//
// So this header offers NO "did it reclaim anything" call.  The only
// reclamation question it can answer takes the denominator as an argument.
#ifndef PROTO_CORE_CONFORMANCE_CYCLE_DRIVER_H
#define PROTO_CORE_CONFORMANCE_CYCLE_DRIVER_H

#include "../headers/protoCore.h"

#include <string>

namespace proto { namespace conformance {

/// The fraction of the heap growth a workload caused that a conforming runtime
/// must give back.  A named constant so a future loosening is a visible diff
/// and not a tweak inside an assertion.
///
/// Why 0.50 and not 0.95.  A conforming runtime does NOT return 100% of what it
/// took, for three legitimate reasons, all in docs/GarbageCollector.md:
///   (a) a young chain submitted AFTER Phase 2's dirtySegments exchange belongs
///       to the NEXT cycle, so the last batch always lags;
///   (b) with PROTOCORE_GC_REINCLUDE_SURVIVORS and survivorStagger > 1,
///       survivors re-enter the candidate set only every n-th cycle;
///   (c) the driver's own allocations, the runtime's caches and each thread's
///       unused free-cell batch (up to CELL_CHUNK_SIZE = 8192 cells) are live.
/// A NON-conforming runtime gives back a rounding error: protoST returned 0 of
/// 2,748,398, Track Y's helper 4-7 of 205,120 -- a ratio of 3e-5.  The
/// threshold therefore discriminates anywhere in [0.05, 0.9], and 0.50 is far
/// above every observed failure and far below the lag (a)-(c) can cause for a
/// workload of the size this library demands.
///
/// TUNING IT PER RUNTIME IS FORBIDDEN.  If a runtime cannot reach 0.50, the
/// finding is about the runtime.
inline constexpr double kReclaimFraction = 0.50;

/// The floor on how much heap a measured workload must consume before a verdict
/// is possible.  Below it, the fixed lag of (a)-(c) above is not a small
/// fraction of the denominator and the case reports NotApplicable rather than
/// guessing.  Chosen so that one thread's unused batch (8192) is ~4%.
inline constexpr unsigned long kMinWorkloadCells = 200000;

/// A point-in-time reading of the space's cell accounting.
///
/// `inUse` is the measure every verdict is built on: cells the space has taken
/// from the OS and not got back on its free list.  It is what the kernel itself
/// can see, and it is deliberately NOT ProtoContext::allocatedCellsCount --
/// safepoint() zeroes that counter every time it submits a young chain, so a
/// delta of it is smallest exactly when a runtime is conforming.
struct HeapSample
{
    long          heapSize   = 0;
    long          freeCells  = 0;
    long          inUse      = 0;   ///< heapSize - freeCells
    unsigned long liveLast   = 0;   ///< space.liveCellsLastCycle
    unsigned long reclaimed  = 0;   ///< space.reclaimedLastCycle
    unsigned long long cycles = 0;  ///< space.getGCCycleCount()
};

HeapSample sample(ProtoSpace& space);

struct CycleReport
{
    HeapSample    base;              ///< before the workload
    HeapSample    peak;              ///< after the workload, before cycles
    HeapSample    end;               ///< after cycles converged
    unsigned long reclaimedSum = 0;  ///< sum of reclaimedLastCycle over cycles
    unsigned      cyclesRun    = 0;
    bool          converged    = false;  ///< false when the deadline hit first

    /// Heap the workload took and had not given back when it returned.  THE
    /// DENOMINATOR, measured by the kernel rather than declared by the Host.
    long grownByWorkload() const { return peak.inUse - base.inUse; }
    /// Heap still held after reclamation converged.
    long residual() const { return end.inUse - base.inUse; }
};

/**
 * @brief Drive GC cycles on `space` until the in-use figure stops falling.
 *
 * Two interlocking concerns, both learned the hard way:
 *
 *  * triggerGC() is advisory -- it does nothing unless free cells are below
 *    20% (ProtoSpace::triggerGC).  A case that allocates a bounded amount must
 *    set gcStarted directly, which is what protoCore's own GC tests do
 *    (test/GCSurvivorRechainTests.cpp::waitForGcCycles).
 *
 *  * STW cannot begin until parkedThreads >= runningThreads, and the calling
 *    thread is one of the running ones.  So the wait loop MUST call
 *    ctx->safepoint(), or the collector blocks forever waiting for us.  That is
 *    the same obligation rule 1 audits, which is worth noticing: the harness
 *    has to obey the rule it is measuring.
 */
CycleReport driveCycles(ProtoSpace& space, ProtoContext* ctx,
                        unsigned maxCycles, unsigned deadlineMs);

/// Continue an existing report with more cycles, keeping `base` and `peak`.
void driveMoreCycles(CycleReport& r, ProtoSpace& space, ProtoContext* ctx,
                     unsigned maxCycles, unsigned deadlineMs);

/**
 * @brief The ONLY reclamation verdict this library offers.
 *
 * @param declaredByHost what the Host said it allocated, for cross-checking and
 *        for the report.  Pass 0 when the Host could not measure it: that is
 *        reported, and does not by itself void the verdict, because the
 *        denominator is kernel-measured.
 * @return an empty string when the report is consistent with the heap growth
 *         the workload caused, otherwise the failure message, which always
 *         includes every number it used.
 */
std::string checkProportionalReclaim(const CycleReport& r,
                                     unsigned long declaredByHost);

/**
 * @brief Was the workload big enough to judge at all?
 * @return an empty string when it was, otherwise the NotApplicable reason.
 */
std::string checkWorkloadLargeEnough(const CycleReport& r,
                                     unsigned long declaredByHost);

/// Human-readable, always emitted -- on pass as well as on fail.
std::string describe(const CycleReport& r, unsigned long declaredByHost);

}}  // namespace proto::conformance

#endif  // PROTO_CORE_CONFORMANCE_CYCLE_DRIVER_H
