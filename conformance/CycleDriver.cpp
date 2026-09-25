#include "CycleDriver.h"

#include <chrono>
#include <mutex>
#include <thread>

namespace proto { namespace conformance {

namespace {

std::string num(long v) { return std::to_string(v); }
std::string num(unsigned long v) { return std::to_string(v); }
std::string num(unsigned long long v) { return std::to_string(v); }

/// Request one cycle and wait for it, cooperating with STW while we wait.
///
/// The request is made by setting gcStarted directly rather than by calling
/// triggerGC(), because triggerGC is advisory: it starts nothing unless free
/// cells are already below 20% of the heap (ProtoSpace::triggerGC).  A
/// conformance case allocates a bounded amount, so it would frequently sit
/// above that threshold and measure no cycle at all -- which would look exactly
/// like a runtime that cannot reclaim.
bool requestOneCycle(ProtoSpace& space, ProtoContext* ctx, unsigned deadlineMs)
{
    // Bounded, not blocking.  Requesting a cycle means taking globalMutex, and
    // the collector holds that mutex while it waits for the stop-the-world
    // quorum -- so on a non-conforming runtime an unbounded lock_guard here
    // blocks forever and the CASE hangs instead of reporting.  That is not
    // hypothetical: it is what the first draft did when it was run against a
    // kernel built without the ProtoThread::join fix.  Every wait in this
    // library is bounded, including the ones that look like bookkeeping.
    const auto lockDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);
    bool requested = false;
    while (std::chrono::steady_clock::now() < lockDeadline) {
        if (ProtoSpace::globalMutex.try_lock()) {
            space.gcStarted = true;
            space.gcCV.notify_all();
            ProtoSpace::globalMutex.unlock();
            requested = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!requested) return false;   // could not even ask: report, do not hang
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);
    {
        // Leave the running set while waiting, rather than calling safepoint()
        // in a poll loop.  Both meet the stop-the-world quorum, but safepoint()
        // ALSO submits this context's young generation -- so a driver that
        // polled with safepoint() would submit the garbage of the very context
        // it is measuring, and rule 1 could never fail.  That is not a
        // hypothetical: the first draft of this driver polled with safepoint(),
        // and the self-check's NoSafepoint mutant PASSED gc.young_submitted
        // with 378,016 cells reclaimed, because the harness had submitted them
        // on the host's behalf.  Nothing inside this region touches a
        // ProtoObject*; it reads atomics only.
        ProtoContext::UnmanagedScope parked(ctx);
        while (space.gcStarted.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (space.gcStarted.load()) return false;   // deadline hit
    // The GC thread clears gcStarted before sweep finishes (sweep runs without
    // the global lock), so the reclaimed figure is not yet final here.  Give
    // the sweep room before the next sample.
    {
        ProtoContext::UnmanagedScope parked(ctx);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    return true;
}

}  // namespace

HeapSample sample(ProtoSpace& space)
{
    HeapSample s;
    s.heapSize  = space.heapSize;
    s.freeCells = space.freeCellsCount;
    s.inUse     = s.heapSize - s.freeCells;
    s.liveLast  = space.liveCellsLastCycle.load(std::memory_order_relaxed);
    s.reclaimed = space.reclaimedLastCycle.load(std::memory_order_relaxed);
    s.cycles    = space.getGCCycleCount();
    return s;
}

void driveMoreCycles(CycleReport& r, ProtoSpace& space, ProtoContext* ctx,
                     unsigned maxCycles, unsigned deadlineMs)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);
    long previousInUse = sample(space).inUse;
    unsigned stable = 0;

    for (unsigned i = 0; i < maxCycles; ++i) {
        if (std::chrono::steady_clock::now() >= deadline) {
            r.converged = false;
            r.end = sample(space);
            return;
        }
        const unsigned remainingMs = (unsigned) std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (!requestOneCycle(space, ctx, remainingMs)) {
            r.converged = false;
            r.end = sample(space);
            return;
        }
        ++r.cyclesRun;
        const HeapSample after = sample(space);
        r.reclaimedSum += after.reclaimed;

        // Convergence: two consecutive cycles that no longer lower the in-use
        // figure.  Two rather than one because a submitted-late young chain
        // belongs to the next cycle (GarbageCollector.md Phase 2 item 5), so a
        // single flat cycle is not yet evidence that nothing more is coming.
        if (after.inUse >= previousInUse) {
            if (++stable >= 2) { r.converged = true; r.end = after; return; }
        } else {
            stable = 0;
        }
        previousInUse = after.inUse;
    }
    // Ran the full cycle budget without two flat cycles in a row.  That is
    // still a converged measurement for our purposes only if the in-use figure
    // came down; the caller's proportional check decides.  Report it honestly.
    r.converged = true;
    r.end = sample(space);
}

CycleReport driveCycles(ProtoSpace& space, ProtoContext* ctx,
                        unsigned maxCycles, unsigned deadlineMs)
{
    CycleReport r;
    r.base = sample(space);
    r.peak = r.base;
    driveMoreCycles(r, space, ctx, maxCycles, deadlineMs);
    return r;
}

std::string describe(const CycleReport& r, unsigned long declaredByHost)
{
    std::string s;
    s += "inUse base=" + num(r.base.inUse)
       + " peak=" + num(r.peak.inUse)
       + " end="  + num(r.end.inUse)
       + "; grownByWorkload=" + num(r.grownByWorkload())
       + " residual=" + num(r.residual())
       + "; heapSize " + num(r.base.heapSize) + "->" + num(r.end.heapSize)
       + "; freeCells " + num(r.base.freeCells) + "->" + num(r.end.freeCells)
       + "; cyclesRun=" + std::to_string(r.cyclesRun)
       + " gcCycleCount " + num(r.base.cycles) + "->" + num(r.end.cycles)
       + "; reclaimedSum=" + num(r.reclaimedSum)
       + " liveCellsLastCycle=" + num(r.end.liveLast)
       + "; hostDeclared=" + num(declaredByHost)
       + "; converged=" + (r.converged ? "yes" : "no");
    return s;
}

std::string checkWorkloadLargeEnough(const CycleReport& r,
                                     unsigned long declaredByHost)
{
    if (r.grownByWorkload() < (long) kMinWorkloadCells) {
        return "the workload did not consume enough heap to judge: the space's "
               "in-use cell count grew by only "
             + num(r.grownByWorkload()) + " cells, and at least "
             + num(kMinWorkloadCells) + " is required before the fixed lag of "
               "late submission, survivor stagger and per-thread free-cell "
               "batches is a small fraction of the denominator.  Either the "
               "Host's workload is too small, or it is not reaching protoCore "
               "at all.  " + describe(r, declaredByHost);
    }
    return std::string();
}

std::string checkProportionalReclaim(const CycleReport& r,
                                     unsigned long declaredByHost)
{
    if (!r.converged) {
        return "reclamation did not converge within the deadline.  A cycle that "
               "cannot complete is rule 2's signature (a registered thread "
               "blocked outside an UnmanagedScope holds the stop-the-world "
               "quorum), not rule 1's.  " + describe(r, declaredByHost);
    }

    const long grown = r.grownByWorkload();
    const long allowedResidual =
        (long) ((1.0 - kReclaimFraction) * (double) grown);

    if (r.residual() > allowedResidual) {
        return "the workload grew the space's in-use cell count by "
             + num(grown) + " cells and " + num(r.residual())
             + " of them were still held after " + std::to_string(r.cyclesRun)
             + " collection cycles; at most " + num(allowedResidual)
             + " was allowed (kReclaimFraction=0.50).  A cycle that runs and "
               "returns a handful of cells is what an UNSUBMITTED YOUNG "
               "GENERATION looks like: a context's young chain is recorded by "
               "GC Phase 2 as a root and never as a candidate, so a chain that "
               "is never submitted is live by construction and no cycle can "
               "ever consider it -- however many cycles run.  "
               "ProtoContext::safepoint() is the only submission point (rule "
               "1).  Note gcCycleCount advanced normally, which is why "
               "instrumentation that counts cycles reports a healthy collector "
               "while nothing is reclaimed.  " + describe(r, declaredByHost);
    }
    return std::string();
}

}}  // namespace proto::conformance
