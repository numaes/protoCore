// ConformanceSelfCheckTests.cpp -- rule 10, applied to the conformance suite
// itself.  This is the file that makes every other conformance result
// believable.
//
// Why it is not optional.  protoST's suite looked green for its entire history
// while the collector reclaimed exactly 0 cells of 2,748,398.  protoScala
// shipped six map fixtures that could not detect a broken key classification.
// protoClojure passed 391 tests with an apparent live set 110x its real one.
// A GC or concurrency test that cannot fail is worse than no test, because it
// converts absence of evidence into evidence of absence.
//
// Every test below runs ONE conformance case against a host built to violate
// exactly that rule, and REQUIRES a Fail.  If one of these ever passes, the
// corresponding case has stopped measuring anything and every green conformance
// report that relied on it is void.
#include <gtest/gtest.h>

#include "SelfHost.h"

using proto::conformance::CaseResult;
using proto::conformance::Status;
using proto::conformance::runAll;
using proto::conformance::runOne;

namespace {

/// A failure must be DIAGNOSTIC, not merely negative: a message that does not
/// carry the numbers it used sends the reader back to guessing.
void expectDiagnostic(const CaseResult& r, const char* mustMention)
{
    EXPECT_NE(r.detail.find(mustMention), std::string::npos)
        << "the failure message does not mention '" << mustMention
        << "', so it cannot be acted on: " << r.detail;
}

}  // namespace

//===========================================================================
// The mutation matrix: one deliberately broken host per case.
//===========================================================================

// Rule 1.  protoST's S15 exactly.
TEST(ConformanceSelfCheck, YoungSubmittedCatchesMissingSafepoint)
{
    proto::conformance::NonConformingHost_NoSafepoint host;
    const CaseResult r = runOne(host, "gc.young_submitted");
    EXPECT_EQ(r.status, Status::Fail) << r.detail;
    expectDiagnostic(r, "grownByWorkload");
    expectDiagnostic(r, "safepoint");
}

// Rule 5.  protoClojure's pre-Track-C vectors: sequences out of perennial
// interned tuple nodes.
TEST(ConformanceSelfCheck, TransientReclaimedCatchesPerennialSequence)
{
    proto::conformance::NonConformingHost_PerennialSequence host;
    const CaseResult r = runOne(host, "gc.transient_reclaimed");
    EXPECT_EQ(r.status, Status::Fail) << r.detail;
    expectDiagnostic(r, "grownByWorkload");
}

// Rule 11.  A ProtoContext on a bare std::thread, declared Registered.
TEST(ConformanceSelfCheck, ThreadRegisteredCatchesRawStdThread)
{
    proto::conformance::NonConformingHost_RawStdThread host;
    const CaseResult r = runOne(host, "thread.registered");
    EXPECT_EQ(r.status, Status::Fail) << r.detail;
    expectDiagnostic(r, "raw-std-thread");
    expectDiagnostic(r, "space->threads");
}

// Rule 11's other half.  A declaration the code contradicts.
TEST(ConformanceSelfCheck, ThreadRegisteredCatchesLyingHoldsNothing)
{
    proto::conformance::NonConformingHost_LyingHoldsNothing host;
    const CaseResult r = runOne(host, "thread.registered");
    EXPECT_EQ(r.status, Status::Fail) << r.detail;
    expectDiagnostic(r, "HoldsNothing");
    expectDiagnostic(r, "allocated");
}

// Rule 2b.  A registered thread joined with a bare std::thread::join, which
// reaches no safepoint and holds the stop-the-world quorum.  This is the shape
// the kernel's own ProtoThread::join no longer has and that a runtime can still
// write for itself -- so the case must still catch it.
TEST(ConformanceSelfCheck, JoinParksCatchesBareJoin)
{
    proto::conformance::NonConformingHost_BareJoin host;
    const CaseResult r = runOne(host, "join.parks");
    EXPECT_EQ(r.status, Status::Fail) << r.detail;
    expectDiagnostic(r, "runningThreads");
}

// Rule 4.  An uninterned attribute key: correct through getAttribute's content
// fallback, silently absent through the getOwnAttributeDirect fast path.
TEST(ConformanceSelfCheck, FastPathKeyCatchesUninternedKey)
{
    proto::conformance::NonConformingHost_UninternedKey host;
    const CaseResult r = runOne(host, "symbol.fast_path_key_hits");
    EXPECT_EQ(r.status, Status::Fail) << r.detail;
    expectDiagnostic(r, "getOwnAttributeDirect");
}

//===========================================================================
// The harness's own vacuity checks.
//===========================================================================

// The single most important test in the phase: the assertion that the suite
// cannot repeat protoST's mistake in its own voice.
TEST(ConformanceSelfCheck, NotApplicableIsNeverReportedAsPass)
{
    proto::conformance::EmptyHost host;   // only the three required capabilities
    const std::vector<CaseResult> results = runAll(host);
    ASSERT_FALSE(results.empty());
    unsigned kernelCases = 0;
    for (const CaseResult& r : results) {
        EXPECT_FALSE(r.detail.empty()) << r.id << " reported no detail";
        if (proto::conformance::isKernelCase(r.id)) {
            // A kernel characterisation needs no runtime capability, so it MUST
            // pass here: it is asserting protoCore's own documented behaviour.
            ++kernelCases;
            EXPECT_EQ(r.status, Status::Pass)
                << r.id << " is a kernel characterisation and must hold "
                           "independently of any runtime: " << r.detail;
            continue;
        }
        EXPECT_NE(r.status, Status::Pass)
            << r.id << " audits the runtime and yet passed for a host that "
                       "supplies no capability: " << r.detail;
    }
    EXPECT_GT(kernelCases, 0u)
        << "no case is marked kernel-only, so this test is not exercising the "
           "distinction it exists to police";
}

TEST(ConformanceSelfCheck, UnknownCaseIdFailsRatherThanSkips)
{
    proto::conformance::SelfHost host;
    const CaseResult r = runOne(host, "gc.no_such_case");
    // Fail, not Skipped: a case that silently stopped being registered must not
    // look like a case that was deliberately not run.
    EXPECT_EQ(r.status, Status::Fail) << r.detail;
    expectDiagnostic(r, "unknown conformance case id");
}

TEST(ConformanceSelfCheck, EveryResultCarriesItsNumbers)
{
    // A run whose details are all empty is a broken harness that looks like a
    // clean suite -- the entire subject of this phase.
    proto::conformance::SelfHost host;
    for (const CaseResult& r : runAll(host)) {
        EXPECT_FALSE(r.detail.empty()) << r.id;
        EXPECT_NE(r.rule, 0u) << r.id << " reports no rule number";
    }
}

TEST(ConformanceSelfCheck, CasesThatDestroyTheRunAreSkippedWithInstructions)
{
    proto::conformance::SelfHost host;
    unsigned seen = 0;
    for (const CaseResult& r : runAll(host)) {
        if (!proto::conformance::needsOwnProcess(r.id)) continue;
        ++seen;
        EXPECT_EQ(r.status, Status::Skipped) << r.id << ": " << r.detail;
        expectDiagnostic(r, "--case=");
        expectDiagnostic(r, "own process");
    }
    // Three today: one aborts, two deadlock the space.  A drop to zero would
    // mean a case that cannot report its own failure is running in-process.
    EXPECT_EQ(seen, 3u) << "the set of cases needing their own process changed";
}

// protoCore names capabilities, never runtimes.  Executed rather than intended:
// a per-embedder special case inside the library is the failure mode the Host
// adaptor exists to prevent.
TEST(ConformanceSelfCheck, ProtoCoreNamesNoRuntimeInAnyCaseDetail)
{
    static const char* kRuntimes[] = {"protoST", "protoJS", "protoPython",
                                      "protoClojure", "protoScala"};
    proto::conformance::SelfHost host;
    for (const CaseResult& r : runAll(host)) {
        for (const char* rt : kRuntimes) {
            EXPECT_EQ(r.detail.find(rt), std::string::npos)
                << r.id << " names the runtime '" << rt << "': " << r.detail;
        }
    }
}

//===========================================================================
// The control: every case must PASS for the reference host, and none may be
// NotApplicable.  SelfHost can supply every capability by construction, so a
// NotApplicable there means the capability is unreachable and the case would be
// NotApplicable for every runtime -- i.e. the case is not finished.
//===========================================================================
TEST(ConformanceSelfCheck, ReferenceHostPassesEveryNonAbortingCase)
{
    proto::conformance::SelfHost host;
    for (const CaseResult& r : runAll(host)) {
        if (r.status == Status::Skipped) continue;   // the aborting case
        EXPECT_EQ(r.status, Status::Pass)
            << r.id << ": " << r.detail;
        EXPECT_NE(r.status, Status::NotApplicable)
            << r.id << " is NotApplicable for the reference host, which supplies "
                       "every capability -- so this case is unfinished and would "
                       "be NotApplicable for every runtime: " << r.detail;
    }
}

//===========================================================================
// The kernel fix of 2026-09-25, pinned here because it changes behaviour for
// five runtimes at once.
//===========================================================================

// ProtoThread::join must leave the running set while it blocks.  Without this,
// a registered thread blocked in a join still counts in runningThreads, the
// stop-the-world quorum can never be met, and a thread that needs memory waits
// for a cycle that cannot begin -- usually the thread being joined.
TEST(ConformanceSelfCheck, ProtoThreadJoinLeavesTheRunningSetWhileBlocked)
{
    // Run directly rather than through runAll: this case is skipped there
    // because its FAILURE deadlocks, but against a conforming kernel it returns
    // in well under a second, and that is exactly what is being pinned.
    proto::conformance::SelfHost host;
    const CaseResult r = runOne(host, "join.parks");
    EXPECT_EQ(r.status, Status::Pass) << r.detail;
    expectDiagnostic(r, "cyclesCompletedWhileJoinBlocked");
}

// The criticalSectionDepth exception, pinned.  Inside a CriticalSection the
// calling thread holds cells reachable only from C++ locals, so leaving the
// running set would let a root scan miss them and the sweep free them: that
// trades a deadlock for memory corruption, which is the worse trade.  join
// therefore does NOT leave the running set there, and it must still return.
TEST(ConformanceSelfCheck, JoinInsideCriticalSectionStillJoinsAndDoesNotPark)
{
    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;

    static volatile bool release = false;
    release = false;
    proto::conformance::ThreadBridge::release() = &release;

    struct Entry {
        static const proto::ProtoObject* run(proto::ProtoContext* c,
                                             const proto::ProtoObject*,
                                             const proto::ParentLink*,
                                             const proto::ProtoList*,
                                             const proto::ProtoSparseList*)
        {
            for (int i = 0; i < 50 && !release; ++i) {
                c->safepoint();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return PROTO_NONE;
        }
    };

    const proto::ProtoThread* t = space.newThread(
        ctx, proto::ProtoString::createSymbol(ctx, "critsec-join-target"),
        &Entry::run, ctx->newList(), nullptr);
    ASSERT_NE(t, nullptr);

    release = true;
    {
        proto::ProtoContext::CriticalSection cs(ctx);
        const int parkedBefore = space.parkedThreads.load();
        const_cast<proto::ProtoThread*>(t)->join(ctx);
        // The join returned, and it did not announce this thread as parked --
        // which is the whole point: a parked announcement here would expose
        // half-built cells held only in C++ locals to the sweep.
        EXPECT_EQ(space.parkedThreads.load(), parkedBefore)
            << "join left the running set while the caller was inside a "
               "CriticalSection, which trades a deadlock for memory corruption";
    }
}
