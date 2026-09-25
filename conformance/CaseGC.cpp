// CaseGC.cpp -- rules 1, 5 and 3's runtime stress probe.
#include "Cases.h"
#include "CycleDriver.h"

#include <string>

namespace proto { namespace conformance {

namespace {

/// Settle the space before a measurement, so that `base` is a quiet reading and
/// not the tail of whatever the runtime's bootstrap left behind.  Without this
/// the bootstrap's own garbage lands in `grownByWorkload` and flatters the
/// runtime by inflating the denominator.
void settle(ProtoSpace& space, ProtoContext* ctx)
{
    CycleReport warmup = driveCycles(space, ctx, /*maxCycles=*/4, /*deadlineMs=*/8000);
    (void) warmup;
}

CaseResult measuredReclaim(Host& host, const char* id, unsigned rule,
                           unsigned long (Host::*workload)(unsigned long),
                           const char* capabilityName,
                           const char* capabilityWhy,
                           bool capabilityIsRequired)
{
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {id, rule, Status::Fail,
                "Host::mainContext() returned no usable context, so nothing "
                "about this runtime can be measured"};

#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    // Say so rather than pass: without the flag there is no threshold
    // submission path to audit, and a green result would be a lie.
    return {id, rule, Status::NotApplicable,
            "protoCore was built without PROTOCORE_GC_REINCLUDE_SURVIVORS, so "
            "ProtoContext::safepoint() has no young-generation submission path "
            "in this build and the obligation this case audits does not exist "
            "here.  Rebuild protoCore with the flag ON (it is the default) "
            "before reading a conformance result"};
#else
    ProtoSpace& space = *ctx->space;

    settle(space, ctx);

    CycleReport r;
    r.base = sample(space);

    const unsigned long declared = (host.*workload)(kMinWorkloadCells);
    if (declared == 0 && !capabilityIsRequired) {
        // A zero from an OPTIONAL capability means "not implemented".  For the
        // required capability a zero means "cannot measure", which is handled
        // below by the workload-size check -- the kernel still measured the
        // heap, so the verdict does not depend on the Host's number.
        return unavailable(id, rule, capabilityName, capabilityWhy);
    }

    r.peak = sample(space);

    const std::string tooSmall = checkWorkloadLargeEnough(r, declared);
    if (!tooSmall.empty()) {
        // Distinguish "the capability is missing" from "the capability ran and
        // did almost nothing".  Both are NotApplicable, and conflating them
        // would hide an adaptor that silently evaluates nothing.
        return {id, rule, Status::NotApplicable,
                std::string(declared == 0 ? "the Host declared 0 cells and " : "")
                + tooSmall};
    }

    driveMoreCycles(r, space, ctx, /*maxCycles=*/16, /*deadlineMs=*/30000);

    const std::string bad = checkProportionalReclaim(r, declared);
    return {id, rule, bad.empty() ? Status::Pass : Status::Fail,
            bad.empty() ? describe(r, declared) : bad};
#endif
}

}  // namespace

// Rule 1 -- the young generation must be submitted.
//
// A context's young chain reaches the collector only through
// ProtoContext::safepoint() or the context's destruction.  GC Phase 2 records a
// young chain as a ROOT HANDLE, not as a candidate: "the cells of a context's
// young chain are not candidates of the cycle and are never marked for it"
// (docs/GarbageCollector.md, Phase 2).  So an unsubmitted chain is live BY
// CONSTRUCTION -- no cycle can consider it.
//
// protoST ran cycles for its entire history, reclaimed 0 of 2,748,398 cells and
// passed 833 tests.  protoClojure's apparent live set was 110x its real one.
// This case is the one that says so.
CaseResult caseYoungSubmitted(Host& host)
{
    return measuredReclaim(host, "gc.young_submitted", 1, &Host::makeGarbage,
                           "makeGarbage",
                           "rule 1 needs the runtime's own evaluator to produce "
                           "garbage; nothing about reclamation can be measured "
                           "without it",
                           /*capabilityIsRequired=*/true);
}

// Rule 5 -- ProtoTuple is never used for transient data.
//
// Measured, not grepped.  Every TupleInterner entry is perennial: entries are
// never removed and the collector records only a published count for them under
// STW, dereferencing nothing (core/ProtoTuple.cpp).  So a runtime whose
// user-visible sequences are ProtoTuple has a heap that grows with every
// sequence ever built and never shrinks -- which is exactly what this
// measurement sees.
//
// This is strictly better than grepping for the type: it catches a runtime that
// reached the same trap through a DIFFERENT perennial structure, and it does NOT
// raise a finding against a runtime that uses ProtoTuple for something
// genuinely perennial.
CaseResult caseTransientReclaimed(Host& host)
{
    return measuredReclaim(host, "gc.transient_reclaimed", 5,
                           &Host::makeSequenceGarbage, "makeSequenceGarbage",
                           "rule 5 is measured on the runtime's own "
                           "user-visible sequence type, which only the runtime "
                           "can build",
                           /*capabilityIsRequired=*/false);
}

// Rule 3 (runtime half) -- no ProtoObject* held across an allocation only in a
// C++ local.  The general rule is NOT mechanisable (see
// docs/EMBEDDER-CONFORMANCE.md checklist C3); what is mechanisable is the
// pressure that opens the window.
//
// All three instances met in this project were found by running the runtime's
// own queue and actor paths with the collector actually reclaiming.  The third
// was UNREACHABLE until the first was fixed, which is why this case runs AFTER
// gc.young_submitted in caseIds() order.
//
// A pass here is not evidence on its own: run it under AddressSanitizer.  A
// surviving use-after-free is silent without it, which is precisely how 848
// passing tests failed to reach protoST's MailboxCursor::adopt bug.
CaseResult caseHostStress(Host& host)
{
    const char* kId = "gc.host_stress";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 3, Status::Fail, "Host::mainContext() returned no usable context"};
    ProtoSpace& space = *ctx->space;

    // A tight ceiling makes every batch trigger real reclamation, so the window
    // between a construction and its rooting is open on every iteration instead
    // of once in a thousand runs.
    const int savedSoft = space.softHeapLimit;
    const int savedHard = space.maxHeapSize;

    const unsigned kRounds = 20;
    unsigned completed = 0;
    unsigned long long cyclesBefore = space.getGCCycleCount();
    unsigned long reclaimedSum = 0;

    space.setHeapLimits(/*softCells=*/150000, /*hardCells=*/400000);
    for (unsigned i = 0; i < kRounds; ++i) {
        if (!host.runProducerConsumer(2000)) break;
        ++completed;
        CycleReport r = driveCycles(space, ctx, /*maxCycles=*/1, /*deadlineMs=*/4000);
        reclaimedSum += r.reclaimedSum;
    }
    space.setHeapLimits(savedSoft, savedHard);

    const unsigned long long cyclesRun = space.getGCCycleCount() - cyclesBefore;

    if (completed == 0)
        return unavailable(kId, 3, "runProducerConsumer",
                           "rule 3's runtime probe needs the runtime's own "
                           "queue or actor path, under pressure, with the "
                           "collector actually reclaiming");

    const std::string detail =
        "completed " + std::to_string(completed) + "/" + std::to_string(kRounds)
        + " stress rounds under a 400,000-cell hard ceiling with a forced cycle "
          "between rounds; cycles=" + std::to_string(cyclesRun)
        + " reclaimedSum=" + std::to_string(reclaimedSum)
        + ".  A pass is evidence only when this case has also been run under "
          "AddressSanitizer: an unrooted local that survives is silent without "
          "it";

    if (completed != kRounds)
        return {kId, 3, Status::Fail,
                "the runtime's own producer/consumer path stopped after "
                + std::to_string(completed) + " of " + std::to_string(kRounds)
                + " rounds under memory pressure.  Read this before concluding "
                  "rule 3: a stall or an out-of-memory abort here has TWO "
                  "possible causes and they need different fixes.  One is rule "
                  "3's subject -- a ProtoObject* held across an allocation in a "
                  "bare C++ local, freed under the runtime, which typically "
                  "shows as a crash or a wrong value.  The other is RETENTION: "
                  "the workload's own structures keeping every item they ever "
                  "handled, which shows as a live set that grows in proportion "
                  "to units processed while reclamation reports zero.  Divide "
                  "liveCellsLastCycle by the units delivered: a small constant "
                  "number of cells per unit is the second, not the first.  "
                + detail};

    // The pressure must have produced cycles, or the case measured nothing.
    if (cyclesRun == 0)
        return {kId, 3, Status::NotApplicable,
                "the workload ran but no collection cycle completed, so the "
                "window this case exists to open was never open.  " + detail};

    return {kId, 3, Status::Pass, detail};
}

}}  // namespace proto::conformance
