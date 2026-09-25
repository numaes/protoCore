#include "Cases.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

namespace proto { namespace conformance {

namespace {

// The order matters and is stated here, in exactly one place.
//
// gc.young_submitted runs first because every later GC observation is
// meaningless while the collector reclaims nothing -- protoST's
// MailboxCursor::adopt bug was UNREACHABLE for 848 passing tests purely because
// S15 was still present.  thread.registered runs before stw.quorum_completes
// because rule 11's failure corrupts while rule 2's only hangs.
//                                  rule  ownProc  kernel-only  fn
const CaseEntry kCases[] = {
    { "gc.young_submitted",              1, false, false, &caseYoungSubmitted },
    { "thread.registered",              11, false, false, &caseThreadRegistered },
    // These two fail by deadlocking the space: the thread they wait on parks
    // inside safepoint() for a collection that can never start, so no bound
    // inside the case can rescue it.  They run one-per-process and the external
    // timeout is the verdict.
    { "stw.quorum_completes",            2, true,  false, &caseQuorumCompletes },
    { "join.parks",                      2, true,  false, &caseJoinParks },
    { "gc.transient_reclaimed",          5, false, false, &caseTransientReclaimed },
    { "symbol.fast_path_key_hits",       4, false, false, &caseFastPathKeyHits },
    { "gc.host_stress",                  3, false, false, &caseHostStress },
    // Kernel characterisations: they need no Host capability, and they exist so
    // that a change to documented kernel behaviour cannot land silently under
    // five runtimes at once.
    { "external.finalizer_runs",         7, false, true,  &casePerennialNeverFinalized },
    { "module.root_survives_cycle",      9, false, true,  &caseModuleRootSurvivesCycle },
    { "external.bytes_accounted",        7, false, false, &caseExternalBytesAccounted },
    { "module.alias_rejected",           9, false, false, &caseModuleAliasRejected },
    // Its failure mode is std::abort() inside waitForHeapHeadroom, which would
    // take the whole test binary with it.  Run through the isolate binary.
    { "heap.ceiling_progress",           8, true,  false, &caseCeilingProgress },
};
const unsigned kCaseCount = sizeof(kCases) / sizeof(kCases[0]);

const CaseEntry* find(const char* id)
{
    if (!id) return nullptr;
    for (unsigned i = 0; i < kCaseCount; ++i)
        if (std::strcmp(kCases[i].id, id) == 0) return &kCases[i];
    return nullptr;
}

bool contains(const std::vector<std::string>& v, const char* s)
{
    for (const std::string& e : v) if (e == s) return true;
    return false;
}

}  // namespace

CaseResult unavailable(const char* id, unsigned rule, const char* capability,
                       const char* why)
{
    // NotApplicable is never Pass, and it always names the capability.  A suite
    // that reports NotApplicable for makeGarbage has told the maintainer exactly
    // why its GC cases mean nothing -- which is the thing 833 green protoST
    // tests never said.
    return {id, rule, Status::NotApplicable,
            std::string("capability unavailable: Host::") + capability
            + "() is not implemented by this runtime's adaptor.  " + why
            + ".  This is NOT a pass: the rule is unverified for this runtime"};
}

const char* statusName(Status s)
{
    switch (s) {
        case Status::Pass:          return "PASS";
        case Status::Fail:          return "FAIL";
        case Status::NotApplicable: return "NOTAPPLICABLE";
        case Status::Skipped:       return "SKIPPED";
        case Status::NeedsReview:   return "NEEDSREVIEW";
    }
    return "?";
}

std::vector<const char*> caseIds()
{
    std::vector<const char*> out;
    out.reserve(kCaseCount);
    for (unsigned i = 0; i < kCaseCount; ++i) out.push_back(kCases[i].id);
    return out;
}

unsigned ruleOf(const char* id)
{
    const CaseEntry* e = find(id);
    return e ? e->rule : 0u;
}

bool needsOwnProcess(const char* id)
{
    const CaseEntry* e = find(id);
    return e ? e->ownProcess : false;
}

bool isKernelCase(const char* id)
{
    const CaseEntry* e = find(id);
    return e ? e->kernelOnly : false;
}

CaseResult runOne(Host& host, const char* id)
{
    const CaseEntry* e = find(id);
    if (!e) {
        // Fail, not Skipped: an unknown id means the caller and the library
        // disagree about what exists, and a Skipped there would hide a case that
        // silently stopped being registered.
        return {id ? id : "<null>", 0, Status::Fail,
                std::string("unknown conformance case id '")
                + (id ? id : "<null>")
                + "'.  The caller and this library disagree about which cases "
                  "exist; a case that was removed must not be reported as "
                  "skipped"};
    }
    try {
        return e->fn(host);
    } catch (const std::exception& ex) {
        return {e->id, e->rule, Status::Fail,
                std::string("the case threw std::exception: ") + ex.what()};
    } catch (...) {
        return {e->id, e->rule, Status::Fail,
                "the case threw a non-std exception"};
    }
}

std::vector<CaseResult> runAll(Host& host, const Selector& sel)
{
    std::vector<CaseResult> out;
    for (unsigned i = 0; i < kCaseCount; ++i) {
        const CaseEntry& e = kCases[i];
        if (!sel.ids.empty() && !contains(sel.ids, e.id)) continue;
        if (sel.skipAbortingCases && e.ownProcess) {
            out.push_back({e.id, e.rule, Status::Skipped,
                std::string("this case's failure mode destroys the run rather "
                            "than reporting -- a process abort, or a deadlock of "
                            "the whole space that no in-process bound can escape. "
                            "Run it in its own process, under an external "
                            "timeout, where the timeout is the verdict: "
                            "<isolate-binary> --case=")
                + e.id});
            continue;
        }
        out.push_back(runOne(host, e.id));
    }
    return out;
}

std::string format(const std::vector<CaseResult>& results)
{
    std::string s;
    // Every result carries its numbers, on pass as well as on fail.  A run whose
    // details are all empty or all zeros is a broken harness that looks like a
    // clean suite -- which is the entire subject of this phase.
    for (const CaseResult& r : results) {
        s += statusName(r.status);
        s += "  rule ";  s += std::to_string(r.rule);
        s += "  ";       s += (r.id ? r.id : "<null>");
        s += "  ";       s += r.detail;   // never empty, never omitted
        s += "\n";
    }
    return s;
}

int isolateMain(Host& host, int argc, char** argv)
{
    const char* wanted = nullptr;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) list = true;
        else if (std::strncmp(argv[i], "--case=", 7) == 0) wanted = argv[i] + 7;
    }

    if (list) {
        for (const char* id : caseIds()) std::printf("%s\n", id);
        return 0;
    }
    if (!wanted) {
        std::fprintf(stderr,
            "usage: %s --list | --case=<id>\n"
            "Runs ONE conformance case in this process, so a case whose failure "
            "mode is std::abort() reports a result instead of destroying a whole "
            "run.\n"
            "Exit: 0 Pass, 1 Fail, 2 NotApplicable, 3 Skipped, 4 usage, "
            "5 NeedsReview.\n", argc > 0 ? argv[0] : "conformance-isolate");
        return 4;
    }

    Selector sel;
    sel.ids.push_back(wanted);
    sel.skipAbortingCases = false;   // the whole point of this binary
    const std::vector<CaseResult> results = runAll(host, sel);
    std::printf("%s", format(results).c_str());
    std::fflush(stdout);

    if (results.empty()) return 4;
    switch (results[0].status) {
        case Status::Pass:          return 0;
        case Status::Fail:          return 1;
        case Status::NotApplicable: return 2;
        case Status::Skipped:       return 3;
        case Status::NeedsReview:   return 5;
    }
    return 4;
}

}}  // namespace proto::conformance
