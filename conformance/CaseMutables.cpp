// CaseMutables.cpp -- rule 13: a cycle among mutable objects is never collected.
//
// The only rule in this document whose verdict is not "the invariant held or it
// did not", and the reason is worth stating where the code is.  A cycle among
// mutables is permanent retention, always; whether it is a DEFECT depends on what
// the program meant:
//
//   * protoPython's `fn -> __closure_frames__ -> frame -> co_name -> fn` was a
//     diagnostic back-pointer.  Removable, and removed.
//   * protoScala's captured `var` -- `var f = null; f = () => f()` -- compiles the
//     cell to a protoCore mutable because sharing a `var` between closures is
//     exactly what it is for.  A snapshot would freeze the cell at null.  The
//     cycle is what the program means.
//
// protoCore cannot tell those apart, and a case that called the second one a Fail
// would be switched off, taking the first one with it.  So the rule turns on a
// DECLARATION VERIFIED AGAINST A MEASUREMENT -- the same shape as rule 11's
// ThreadVerdict -- and the Fail is reserved for cycles the runtime did not
// declare.
//
// See docs/MemoryModel.md section 7 for the property and the rule, and
// ProtoSpace::findMutableCycles for the detector.
#include "Cases.h"
#include "CycleDriver.h"

#include <string>

namespace proto { namespace conformance {

namespace {

std::string cycleLines(const MutableGraphReport& r, unsigned limit)
{
    std::string s;
    unsigned shown = 0;
    for (const MutableCycle& c : r.cycles) {
        if (shown++ >= limit) {
            s += "; ... and " + std::to_string(r.cycles.size() - limit)
               + " more (call ProtoSpace::findMutableCycles for the rest)";
            break;
        }
        s += "; {";
        for (std::size_t i = 0; i < c.refs.size(); ++i) {
            if (i) s += ",";
            s += std::to_string(c.refs[i]);
        }
        s += "} " + c.path;
    }
    return s;
}

}  // namespace

CaseResult caseMutableGraphCycles(Host& host)
{
    const char* kId = "mutable.graph_cycles";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 13, Status::Fail, "Host::mainContext() returned no usable context"};
    ProtoSpace& space = *ctx->space;

    // Baseline first: whatever the runtime built before this case ran is part of
    // what rule 13 governs, and the growth across the probe is what establishes
    // that the probe did anything at all.
    const MutableGraphReport before = space.findMutableCycles(ctx);
    const unsigned long declaredBuilt = host.makeMutableGraph();
    const MutableGraphReport after = space.findMutableCycles(ctx);

    const std::string counts =
        "mutables table " + std::to_string(before.handles) + "->"
        + std::to_string(after.handles) + " entries, host declared "
        + std::to_string(declaredBuilt) + " mutables built, "
        + std::to_string(after.handleReferences)
        + " references to a mutable handle, "
        + std::to_string(after.cellsVisited) + " cells walked, "
        + std::to_string(after.cycles.size()) + " cycle(s)";

    if (after.truncated)
        return {kId, 13, Status::Fail,
                "the scan ran out of cell budget, so the ABSENCE of cycles is "
                "not established and this result cannot be read as clean.  "
                "Raise the budget argument of "
                "ProtoSpace::findMutableCycles.  " + counts
                + cycleLines(after, 4)};

    // The non-vacuity gate.  A scan over a table the runtime never wrote to is a
    // statement about protoCore's bootstrap, not about this runtime -- and
    // protoCore's bootstrap writes to nothing, so the table would be empty.
    // NotApplicable, never Pass: that is the distinction this whole library
    // exists to keep.
    if (after.handles == before.handles) {
        const std::string why =
            "the mutables table did not grow while the probe ran (" + counts
            + "), so the scan covered no mutable this runtime built and its "
              "silence is a statement about protoCore's bootstrap rather than "
              "about this runtime";
        return unavailable(kId, 13, "makeMutableGraph", why.c_str());
    }

    const long declaredCycles = host.declaredMutableCycles();
    const unsigned long found = static_cast<unsigned long>(after.cycles.size());

    if (found == 0) {
        if (declaredCycles > 0)
            return {kId, 13, Status::Pass,
                    "the mutable graph is acyclic, so every mutable's table "
                    "entry is released once its handle dies.  The runtime "
                    "declares " + std::to_string(declaredCycles)
                    + " structural cycle(s) and none were found, which is "
                      "better than declared and worth re-reading the "
                      "declaration for.  " + counts};
        return {kId, 13, Status::Pass,
                "the mutable graph is acyclic and the scan was complete, so "
                "every mutable's table entry is released once its handle dies "
                "and nothing is retained for the life of the space.  " + counts};
    }

    if (declaredCycles < 0)
        return {kId, 13, Status::NeedsReview,
                "the scan found " + std::to_string(found)
                + " cycle(s) in the mutable graph and this runtime declares "
                  "none.  EVERY ONE OF THEM IS PERMANENT RETENTION: the table "
                  "is a GC root unconditionally and an entry is released only "
                  "once its handle has been finalized, so a value that reaches "
                  "its own handle keeps it marked for ever, and a later "
                  "collection does not fix it.  The amount is bounded by the "
                  "cycles themselves.  Decide for each whether it is incidental "
                  "-- store the current value instead of the mutable -- or "
                  "structural, like a captured variable that refers to itself, "
                  "and record the count in docs/CONFORMANCE.md via "
                  "Host::declaredMutableCycles().  " + counts
                + cycleLines(after, 8)};

    if (found > static_cast<unsigned long>(declaredCycles))
        return {kId, 13, Status::Fail,
                "this runtime declares " + std::to_string(declaredCycles)
                + " structural cycle(s) in its mutable graph and the scan found "
                + std::to_string(found)
                + ".  The extra ones are unaccounted permanent retention: the "
                  "table is a GC root unconditionally and their entries can "
                  "never be released.  " + counts + cycleLines(after, 8)};

    return {kId, 13, Status::Pass,
            "the scan found " + std::to_string(found)
            + " cycle(s), within the " + std::to_string(declaredCycles)
            + " this runtime declares as structural.  Each is permanent, "
              "bounded retention that the runtime has accounted for in its own "
              "docs/CONFORMANCE.md.  " + counts + cycleLines(after, 8)};
}

}}  // namespace proto::conformance
