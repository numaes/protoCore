// CaseHeap.cpp -- rule 8, in its own process.
//
// Rule 8 -- no heap-ceiling wait may be reachable only by the thread that can
// free.  The P2 finding: with the producers and the single consumer all parked
// in ProtoSpace::waitForHeapHeadroom and the GC idle, the only exit is the
// out-of-memory abort.
//
// The failure mode is std::abort() inside waitForHeapHeadroom, reached after two
// consecutive zero-reclaim cycles.  An abort takes the whole test binary with
// it, so this case MUST run one-per-process, which is what the isolate binary is
// for.  isAbortingCase("heap.ceiling_progress") returns true and runAll() skips
// it in-process with that instruction in `detail` -- a Skipped that says how to
// run it, never a silent pass.
//
// Note what this case has to do that no runtime does: SET A CEILING.  No runtime
// in the family calls setHeapLimits, so maxHeapSize is 0, waitForHeapHeadroom
// returns immediately, and the P2 finding is LATENT rather than fixed.  The case
// configures the ceiling itself, which is the only way to reach the code at all.
#include "Cases.h"
#include "CycleDriver.h"

#include <atomic>
#include <string>

namespace proto { namespace conformance {

namespace {

std::atomic<int> g_oomFired{0};

/// The one recovery hook that exists before the abort.  It must not allocate;
/// it only records, so that a runtime which would have aborted instead reports
/// a distinguishable outcome and the case can say WHICH of the three happened.
ProtoObject* recordOomAndReturnNone(ProtoContext* /*ctx*/)
{
    g_oomFired.fetch_add(1);
    return const_cast<ProtoObject*>(PROTO_NONE);
}

}  // namespace

CaseResult caseCeilingProgress(Host& host)
{
    const char* kId = "heap.ceiling_progress";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 8, Status::Fail, "Host::mainContext() returned no usable context"};
    ProtoSpace& space = *ctx->space;

    const int savedSoft = space.softHeapLimit;
    const int savedHard = space.maxHeapSize;
    ProtoObject* (*savedOom)(ProtoContext*) = space.outOfMemoryCallback;

    g_oomFired.store(0);
    space.outOfMemoryCallback = &recordOomAndReturnNone;
        // The ceiling must be near the heap the space ALREADY holds, or the
    // workload finishes below it and the case observes nothing.  protoCore only
    // starts a cycle when a thread needs cells the heap cannot supply below the
    // ceiling, so a ceiling above the current heap is not a ceiling at all.
    const int ceiling = space.heapSize + 32768;
    space.setHeapLimits(/*softCells=*/space.heapSize, /*hardCells=*/ceiling);

    const HeapSample before = sample(space);
    const bool completed = host.runProducerConsumer(200000);
    const HeapSample after = sample(space);

    space.setHeapLimits(savedSoft, savedHard);
    space.outOfMemoryCallback = savedOom;

    const std::string common =
        "heapSize " + std::to_string(before.heapSize) + "->"
        + std::to_string(after.heapSize)
        + " (hard ceiling " + std::to_string(ceiling) + ") inUse " + std::to_string(before.inUse) + "->"
        + std::to_string(after.inUse)
        + " gcCycleCount " + std::to_string(before.cycles) + "->"
        + std::to_string(after.cycles)
        + " reclaimedLastCycle=" + std::to_string(after.reclaimed)
        + " liveCellsLastCycle=" + std::to_string(after.liveLast)
        + " oomCallbackFired=" + std::to_string(g_oomFired.load());

    if (!completed)
        return unavailable(kId, 8, "runProducerConsumer",
                           "rule 8 needs the runtime's own producer/consumer "
                           "topology: the finding is about every thread that "
                           "could free being parked waiting for headroom");

    if (g_oomFired.load() > 0)
        return {kId, 8, Status::Fail,
                "the workload reached the heap ceiling with zero reclamation and "
                "the outOfMemoryCallback fired.  Note what this does and does "
                "not establish.  It establishes that the ceiling was reached "
                "with two consecutive zero-reclaim cycles.  It does NOT by "
                "itself establish the P2 topology (every producer and the "
                "consumer parked in waitForHeapHeadroom with the collector "
                "idle): the same abort is reached when the workload's GENUINE "
                "live set simply does not fit under the ceiling, which is a real "
                "finding of a different kind -- something the runtime anchors "
                "for the whole session and never releases.  Read "
                "liveCellsLastCycle below against the ceiling to tell them "
                "apart: a live set near the ceiling is the second, a small live "
                "set with no progress is the first.  " + common};

    // A workload that completed without ever reaching the ceiling proves
    // nothing about the ceiling.  Say so rather than pass.
    if (after.cycles == before.cycles)
        return {kId, 8, Status::NotApplicable,
                "the workload completed under the hard ceiling "
                "without a single collection cycle, so it never came near the "
                "ceiling and this case observed nothing.  Raise the workload "
                "size before reading a result.  " + common};

    return {kId, 8, Status::Pass,
            "the workload made progress under a hard heap ceiling, with "
            "collection cycles completing while it ran.  " + common};
}

}}  // namespace proto::conformance
