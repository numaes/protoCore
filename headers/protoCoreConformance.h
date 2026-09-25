/*
 * protoCoreConformance.h — the embedder conformance suite (P4).
 *
 * protoCore states its participation obligations once, and this library is
 * that statement in executable form.  Every runtime built on protoCore runs
 * the same cases through an adaptor it implements itself.
 *
 * Two deliberate properties, both load-bearing:
 *
 *  1. FRAMEWORK-FREE.  Cases return results as data; the embedder's own test
 *     framework does the asserting.  protoCore's GoogleTest is a FetchContent
 *     download with INSTALL_GTEST OFF (test/CMakeLists.txt), so it is not
 *     available to a consumer of the installed package -- and protoST and
 *     protoJS use Catch2, not GoogleTest, so imposing one framework was never
 *     an option.
 *
 *  2. protoCore NEVER NAMES A RUNTIME.  Host declares capabilities, not
 *     runtimes.  A case that needs something a runtime cannot supply reports
 *     NotApplicable with the capability's name, NEVER Pass.  A silent pass is
 *     the defect this whole library exists to end.
 *
 * See docs/EMBEDDER-CONFORMANCE.md for the normative rule table.
 */
#ifndef PROTO_CORE_CONFORMANCE_H
#define PROTO_CORE_CONFORMANCE_H

#include "protoCore.h"

#include <string>
#include <vector>

namespace proto { namespace conformance {

/**
 * @brief What a case needs from the runtime under test.
 *
 * `mainContext`, `name` and `makeGarbage` are pure virtual: a runtime that
 * cannot supply them cannot be audited at all, and finding that out at compile
 * time is better than at run time.  Every other method has a default that
 * reports the capability as unavailable.  Implement what you can honestly
 * supply; the runner reports the rest as NotApplicable and says which
 * capability was missing.
 */
class Host
{
public:
    virtual ~Host() = default;

    /** The context the runtime's main thread evaluates in.  REQUIRED. */
    virtual ProtoContext* mainContext() = 0;

    /** A short, stable name for this runtime, used only in reports. */
    virtual const char* name() const = 0;

    /**
     * Make the runtime allocate at least `requestedCells` cells of data that
     * is UNREACHABLE when this call returns, using the runtime's own
     * evaluator rather than direct protoCore calls.  REQUIRED.
     *
     * @return a number the runtime can honestly stand behind: how much it
     *         believes it allocated, or 0 when it cannot tell.  It is a
     *         DECLARATION, cross-checked against what the kernel measured; it
     *         is not the denominator of the verdict.
     *
     * Do NOT compute it as a delta of `ProtoContext::allocatedCellsCount`.
     * `safepoint()` sets that counter to 0 every time it submits the young
     * chain (core/ProtoContext.cpp), so the counter is zeroed by the very
     * mechanism rule 1 audits: a CONFORMING runtime reports a small delta, and
     * an unsigned subtraction underflows into an astronomic one.  Count units
     * of work and multiply by a measured cells-per-unit, or return 0.
     */
    virtual unsigned long makeGarbage(unsigned long requestedCells) = 0;

    /**
     * Same, but built out of the runtime's own SEQUENCE type -- what a user
     * gets from a literal vector, list, array or tuple.  Rule 5's probe.  It
     * deliberately does not mention ProtoTuple: the case measures
     * reclamation, so it is correct whatever representation was chosen.
     */
    virtual unsigned long makeSequenceGarbage(unsigned long requestedCells)
    { (void) requestedCells; return 0; }

    /**
     * Intern `text` the way this runtime interns an ATTRIBUTE KEY, and return
     * the pointer it would actually use as that key.  Rule 4's runtime probe.
     */
    virtual const ProtoObject* internAttributeKey(const char* text)
    { (void) text; return nullptr; }

    /**
     * How a thread kind participates in protoCore.  Rule 11 accepts three
     * conforming shapes, because a runtime can be correct in more than one way
     * and a check that accepted only the first would report a correct runtime
     * as broken (see docs/EMBEDDER-CONFORMANCE.md rule 11).
     */
    enum class ThreadVerdict
    {
        Registered,    ///< created through ProtoSpace::newThread; root-scanned
        HoldsNothing,  ///< declares it touches no ProtoObject* at all
        OwnSpace,      ///< is the main thread of its OWN ProtoSpace
    };

    /** One kind of OS thread the runtime creates. */
    struct ThreadKind
    {
        const char*   name;            ///< e.g. "actor-worker"
        bool          blocksWhenIdle;  ///< true when it waits rather than spins
        ThreadVerdict verdict;         ///< what the runtime DECLARES about it
    };

    /** Called ON the spawned thread, with the context that thread uses. */
    using ThreadBody = void (*)(void* user, const ThreadKind& kind, ProtoContext* ctx);

    /**
     * For each kind of OS thread this runtime creates: start one, run `body`
     * on it, and join it.  Rules 2, 11 and 12's probe.  The case VERIFIES the
     * declared verdict rather than taking it: a kind that declares
     * HoldsNothing and then allocates is a Fail, and that is the finding worth
     * having -- a declaration the code contradicts.
     * @return false when the runtime cannot enumerate its thread kinds.
     */
    virtual bool forEachThreadKind(ThreadBody body, void* user)
    { (void) body; (void) user; return false; }

    /**
     * Run the runtime's own producer/consumer or actor workload for `units`
     * of work.  Rules 3 and 8's probe.
     * @return true when the workload completed.
     */
    virtual bool runProducerConsumer(unsigned long units)
    { (void) units; return false; }

    /**
     * Start a thread through the runtime's own threading facility, have it
     * block until `releaseFlag` becomes true, and JOIN it -- where the join is
     * performed exactly as the runtime performs its own joins.  Rule 2b's
     * probe (`join.parks`).
     *
     * The case sets a hard heap ceiling and makes forward progress depend on a
     * collection completing while this call is blocked.  A join that holds the
     * stop-the-world quorum therefore cannot return, and the case times out
     * instead of hanging the runner.
     *
     * @param releaseFlag a flag the case raises from another thread; the
     *        spawned thread must poll it and then exit.
     * @return true when the runtime supplied a thread and joined it.
     */
    virtual bool joinBlockingThread(volatile bool* releaseFlag)
    { (void) releaseFlag; return false; }

    /**
     * Bytes the runtime allocated OUTSIDE protoCore and is accounting for
     * itself (MemoryModel.md section 5, "Count it").
     * @return (unsigned long) -1 when the runtime keeps no such total.
     */
    virtual unsigned long externalBytesAccounted() { return (unsigned long) -1; }

    /**
     * Load the same logical path from two different providers.  Rule 9c.
     * @return 1 when the runtime kept them distinct, 0 when it aliased them,
     *         -1 when the capability is unavailable.
     */
    virtual int loadSamePathTwoProviders() { return -1; }
};

enum class Status { Pass, Fail, NotApplicable, Skipped, NeedsReview };

/** The textual name of a status, as the report prints it. */
const char* statusName(Status s);

struct CaseResult
{
    const char*  id;        ///< e.g. "gc.young_submitted"
    unsigned     rule;      ///< the rule number in EMBEDDER-CONFORMANCE.md
    Status       status;
    std::string  detail;    ///< ALWAYS carries the numbers measured, even on Pass
};

struct Selector
{
    /// Empty runs every case.  Otherwise, exact case ids.
    std::vector<std::string> ids;
    /// Cases whose failure mode is std::abort() are skipped in-process and
    /// must be run through the isolate binary.  Defaults to true.
    bool skipAbortingCases = true;
};

/** Every case id this build knows, in a stable order. */
std::vector<const char*> caseIds();

/** The rule number a case belongs to, or 0 for an unknown id. */
unsigned ruleOf(const char* id);

/** True when this case's failure mode is a process abort (rule 8). */
bool isAbortingCase(const char* id);

/**
 * True when this case audits the KERNEL rather than the embedder: it needs no
 * Host capability beyond mainContext(), and it therefore passes for any host
 * with a working context.
 *
 * The distinction is load-bearing for the suite's own vacuity check.  "No case
 * may pass for a host that supplies no capability" is the right rule for a case
 * that audits a runtime, and the WRONG rule for a case that characterises
 * protoCore itself -- `external.finalizer_runs` and `module.root_survives_cycle`
 * assert documented kernel behaviour so that a change to it cannot land silently
 * under five runtimes at once, and their passing for a capability-less host is
 * the correct result, not a vacuous one.  Conflating the two would have forced
 * one of two dishonest choices: deleting two real kernel characterisations, or
 * weakening the vacuity check that protects every other case.
 */
bool isKernelCase(const char* id);

/** Run one case.  Never throws; a case that would throw reports Fail. */
CaseResult runOne(Host& host, const char* id);

/** Run every case the selector admits. */
std::vector<CaseResult> runAll(Host& host, const Selector& sel = Selector{});

/** One line per result, for a log.  Always includes `detail`. */
std::string format(const std::vector<CaseResult>& results);

/**
 * The body of a one-case-per-process runner.  A case whose failure mode is
 * std::abort() would destroy an in-process run, so the embedder builds its own
 * isolate binary from its own Host with this macro:
 *
 *     PROTOCORE_CONFORMANCE_ISOLATE_MAIN(MyHost)
 *
 * Exit codes: 0 Pass, 1 Fail, 2 NotApplicable, 3 Skipped, 4 usage error,
 * 5 NeedsReview.  A crash (SIGABRT/SIGSEGV) is a Fail to the caller, and
 * ctest reports the signal -- which for rule 8 is the finding itself.
 */
int isolateMain(Host& host, int argc, char** argv);

}}  // namespace proto::conformance

#define PROTOCORE_CONFORMANCE_ISOLATE_MAIN(HostType)                           \
    int main(int argc, char** argv)                                            \
    {                                                                          \
        HostType host;                                                         \
        return ::proto::conformance::isolateMain(host, argc, argv);            \
    }

#endif  // PROTO_CORE_CONFORMANCE_H
