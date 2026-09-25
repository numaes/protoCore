// CaseThreads.cpp -- rules 2, 2b and 11.
#include "Cases.h"
#include "CycleDriver.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace proto { namespace conformance {

namespace {

const char* verdictName(Host::ThreadVerdict v)
{
    switch (v) {
        case Host::ThreadVerdict::Registered:   return "Registered";
        case Host::ThreadVerdict::HoldsNothing: return "HoldsNothing";
        case Host::ThreadVerdict::OwnSpace:     return "OwnSpace";
    }
    return "?";
}

/// What one thread kind looked like when the case's body ran on it.
struct KindObservation
{
    std::string          name;
    Host::ThreadVerdict  declared = Host::ThreadVerdict::Registered;
    bool                 hadContext    = false;
    bool                 hadThread     = false;
    bool                 discoverable  = false;
    bool                 sameSpace     = false;
    unsigned long        allocatedCells = 0;
};

struct ThreadAccumulator
{
    std::mutex                   mu;
    std::vector<KindObservation> kinds;
    ProtoSpace*                  mainSpace = nullptr;

    void record(const KindObservation& o)
    {
        std::lock_guard<std::mutex> g(mu);
        kinds.push_back(o);
    }

    std::string kindList()
    {
        std::lock_guard<std::mutex> g(mu);
        std::string s;
        for (size_t i = 0; i < kinds.size(); ++i) {
            if (i) s += ", ";
            s += kinds[i].name;
            s += "(";
            s += verdictName(kinds[i].declared);
            s += ")";
        }
        return s.empty() ? std::string("<none enumerated>") : s;
    }
};

/// Is this context's thread actually in `space->threads`, the sparse list GC
/// Phase 2 walks to find root-scanning candidates?
///
/// This is checked directly against the registry rather than through
/// `ProtoThread::getCurrentThread` or `ProtoSpace::getCurrentThread`, because
/// BOTH of those are declared in headers/protoCore.h and defined nowhere in the
/// library -- an embedder that calls either gets an undefined reference at link
/// time.  Reported as a P4 finding; the registry read below needs no new API.
bool inThreadRegistry(ProtoContext* ctx)
{
    if (!ctx || !ctx->thread || !ctx->space || !ctx->space->threads) return false;
    const unsigned long threadId = reinterpret_cast<uintptr_t>(ctx->thread);
    const ProtoObject* found = ctx->space->threads->getAt(ctx, threadId);
    return found != nullptr && found != PROTO_NONE;
}

void probeThread(void* user, const Host::ThreadKind& kind, ProtoContext* ctx)
{
    auto* acc = static_cast<ThreadAccumulator*>(user);
    KindObservation o;
    o.name           = kind.name ? kind.name : "<unnamed>";
    o.declared       = kind.verdict;
    o.hadContext     = (ctx != nullptr);
    o.hadThread      = o.hadContext && ctx->thread != nullptr;
    o.discoverable   = o.hadThread && inThreadRegistry(ctx);
    o.sameSpace      = o.hadContext && ctx->space == acc->mainSpace;
    o.allocatedCells = o.hadContext ? ctx->allocatedCellsCount : 0;
    acc->record(o);
}

}  // namespace

// Rule 11 -- every OS thread that holds a ProtoObject* must be a
// protoCore-REGISTERED thread, or everything it holds must be pinned in a
// ProtoRootSet.
//
// There is no registerThread() in protoCore.  runningThreads is incremented
// only in thread_main, which runs only for a thread created through
// ProtoSpace::newThread.  A ProtoContext built on a raw std::thread gets
// thread == nullptr unless it happens to run on space->mainThreadId.
//
// Now read GC Phase 2: root collection walks space->threads and calls
// scanContexts on each registered thread's context chain, taking
// automaticLocals, returnValue, pendingRoot and the young-chain head.  A thread
// ABSENT from that list is never root-scanned, so its live objects are swept
// under it.
//
// Compare the failure modes.  Rule 2: the collector never finishes -- a hang,
// nothing corrupted.  Rule 11: the collector finishes and is WRONG -- a
// use-after-free at a distance.  Rule 11 is the more dangerous of the two and it
// is checked first.
//
// The verdict accepts THREE conforming shapes, because a runtime can be right in
// more than one way and a check that accepted only registration would report a
// correct runtime as broken.  What the case does is VERIFY THE DECLARATION
// rather than take it: a kind that declares HoldsNothing and then allocates is a
// Fail, and that is the finding worth having.
//
// Note that protoCore's own test/ConcurrentMarkSafetyTests.cpp builds a
// ProtoContext on a bare std::thread deliberately, with everything pinned.  It
// is safe there and a trap to copy into an embedder.
CaseResult caseThreadRegistered(Host& host)
{
    const char* kId = "thread.registered";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 11, Status::Fail, "Host::mainContext() returned no usable context"};

    ThreadAccumulator acc;
    acc.mainSpace = ctx->space;
    if (!host.forEachThreadKind(&probeThread, &acc))
        return unavailable(kId, 11, "forEachThreadKind",
                           "rule 11 needs the runtime to enumerate the kinds of "
                           "OS thread it creates and run a probe on each");

    if (acc.kinds.empty())
        return {kId, 11, Status::NotApplicable,
                "Host::forEachThreadKind() returned true but enumerated no "
                "thread kind.  A runtime that creates no threads has nothing to "
                "check; a runtime that creates threads and enumerates none has "
                "an incomplete adaptor, and this case cannot tell them apart"};

    std::string detail;
    std::string failures;
    for (const KindObservation& o : acc.kinds) {
        detail += o.name + "[declared=" + verdictName(o.declared)
                + " ctx=" + (o.hadContext ? "yes" : "no")
                + " ctx->thread=" + (o.hadThread ? "set" : "NULL")
                + " inSpaceThreads=" + (o.discoverable ? "yes" : "NO")
                + " sameSpace=" + (o.sameSpace ? "yes" : "no")
                + " allocatedCells=" + std::to_string(o.allocatedCells) + "] ";

        switch (o.declared) {
            case Host::ThreadVerdict::Registered:
                if (!o.hadThread || !o.discoverable) {
                    failures += "kind '" + o.name + "' declares Registered but its "
                        "context has thread == nullptr and/or the thread is "
                        "NOT present in space->threads.  GC Phase 2 finds root-scanning "
                        "candidates by walking space->threads, so this thread's "
                        "automaticLocals, returnValue, pendingRoot and young chain "
                        "are never scanned and anything it holds unpinned is swept "
                        "while live.  Create it through ProtoSpace::newThread, or "
                        "declare HoldsNothing/OwnSpace and make that true.  ";
                }
                break;
            case Host::ThreadVerdict::HoldsNothing:
                if (o.allocatedCells != 0) {
                    failures += "kind '" + o.name + "' declares HoldsNothing but "
                        "allocated " + std::to_string(o.allocatedCells)
                        + " cells on its own context.  A declaration the code "
                          "contradicts is worse than no declaration: the thread is "
                          "unregistered AND holds cells, which is the unscanned-roots "
                          "shape.  ";
                }
                break;
            case Host::ThreadVerdict::OwnSpace:
                if (o.sameSpace) {
                    failures += "kind '" + o.name + "' declares OwnSpace but its "
                        "context's space is the SAME space as mainContext()'s, so it "
                        "is a stray thread of someone else's space rather than the "
                        "main thread of its own.  ";
                }
                break;
        }
    }

    if (!failures.empty())
        return {kId, 11, Status::Fail, failures + " Observed: " + detail};
    return {kId, 11, Status::Pass,
            "every enumerated thread kind matches its declared verdict.  "
            + detail};
}

// Rule 2 -- every registered protoCore thread must park.  A thread that blocks
// does so inside ProtoContext::UnmanagedScope.
//
// STW Phase 1 waits for all application threads to reach a parked state
// (allocation safepoints, explicit synchToGC calls, or threads inside
// UnmanagedScope) (docs/GarbageCollector.md Phase 1).  A registered thread
// blocked on a condition variable outside an UnmanagedScope reaches none of the
// three, so parkedThreads never reaches runningThreads and NO CYCLE EVER
// COMPLETES.
//
// The assertion cannot be "the thread is inside an UnmanagedScope" -- protoCore
// cannot see the embedder's call stack.  It is: with your threads up, does
// gcCycleCount advance?  That is the property that matters and the only one
// observable from here.
CaseResult caseQuorumCompletes(Host& host)
{
    const char* kId = "stw.quorum_completes";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 2, Status::Fail, "Host::mainContext() returned no usable context"};
    ProtoSpace& space = *ctx->space;

    // The whole difficulty of this case is keeping one of the runtime's own
    // registered threads ALIVE AND IDLE while the cycle is demanded.  A case
    // that asks the runtime to start a thread, join it, and only then requests a
    // collection measures a single-threaded process and passes for every
    // runtime, conforming or not -- a vacuous pass of exactly the kind this
    // library exists to end.
    //
    // So the runtime's blocking-thread capability is driven from a helper
    // thread of our own, leaving THIS thread free to demand the cycle while the
    // runtime's thread is still up.  Our helper is a bare std::thread that
    // touches no ProtoObject*, so it is not a thread protoCore must know about.
    volatile bool release = false;
    std::atomic<bool> supplied{false};
    std::atomic<bool> finished{false};
    std::thread driver([&]() {
        supplied.store(host.joinBlockingThread(&release));
        finished.store(true);
    });

    // Wait for the runtime's thread to appear in the running set.
    const auto upDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int runningWithThreadUp = space.runningThreads.load();
    while (std::chrono::steady_clock::now() < upDeadline && !finished.load()) {
        runningWithThreadUp = space.runningThreads.load();
        if (runningWithThreadUp > 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const unsigned long long before = space.getGCCycleCount();
    unsigned long long advanced = 0;
    CycleReport r;
    if (runningWithThreadUp > 1) {
        r = driveCycles(space, ctx, /*maxCycles=*/2, /*deadlineMs=*/15000);
        advanced = space.getGCCycleCount() - before;
    }

    release = true;
    driver.join();

    const std::string common =
        "runningThreads(with the runtime's thread up)="
        + std::to_string(runningWithThreadUp)
        + " parkedThreads=" + std::to_string(space.parkedThreads.load())
        + "; " + describe(r, 0);

    if (!supplied.load())
        return unavailable(kId, 2, "joinBlockingThread",
                           "rule 2 needs one of the runtime's own registered "
                           "threads to be alive and idle while a collection is "
                           "demanded; without that the case would measure a "
                           "single-threaded process and pass for any runtime");

    if (runningWithThreadUp <= 1)
        return {kId, 2, Status::NotApplicable,
                "no registered thread beyond the main thread was ever in the "
                "running set while this case watched, so the stop-the-world "
                "quorum was never under pressure and nothing was observed.  "
                "Either the runtime's thread finished before the case could see "
                "it, or it is not registered with protoCore (which is rule 11's "
                "finding, not this one).  " + common};

    if (advanced == 0)
        return {kId, 2, Status::Fail,
                "gcCycleCount did not advance within 15 s while one of this "
                "runtime's registered threads was idle.  STW Phase 1 is waiting "
                "for a thread that is blocked outside an UnmanagedScope and "
                "reaches no safepoint: parkedThreads never reaches "
                "runningThreads, so no collection completes and the symptom is a "
                "hang under load rather than a failure.  " + common};

    return {kId, 2, Status::Pass,
            "gcCycleCount advanced by " + std::to_string(advanced)
            + " while one of this runtime's registered threads was idle, so the "
              "idle thread parks.  " + common};
}

// Rule 2b -- a blocking join must not hold the stop-the-world quorum.
//
// This is rule 2's sharpest instance and it deserves its own case, because for
// protoCore's OWN join the obligation now belongs to the kernel.
//
// `runningThreads` starts at 1 -- the main thread is counted from ProtoSpace
// construction -- and every managed thread adds one.  A thread blocked in a bare
// std::thread::join reaches no safepoint, so it still counts as running: the
// quorum can never be met, no cycle can start, and every thread that then needs
// memory waits in waitForHeapHeadroom for a cycle that cannot begin, usually
// including the thread being joined.  That is a deadlock, not slow shutdown.
//
// As of 2026-09-25 ProtoThread::join brackets itself in an UnmanagedScope, so a
// runtime that joins through the kernel API passes without doing anything.  A
// runtime that calls std::thread::join directly on a thread it registered is
// still broken, and this case still catches it -- which is why the case tests
// the OBSERVABLE property (a cycle completes while the join is blocked) rather
// than which API was called.
//
// The case is bounded: a helper releases the joined thread after the deadline
// whatever happens, so a non-conforming runtime reports a Fail instead of
// hanging the runner.  A case that hangs is indistinguishable from a
// conformance failure that hangs, and this rule's failure mode IS a hang.
CaseResult caseJoinParks(Host& host)
{
    const char* kId = "join.parks";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 2, Status::Fail, "Host::mainContext() returned no usable context"};
    ProtoSpace& space = *ctx->space;

    volatile bool release = false;
    std::atomic<bool> joinReturned{false};
    std::atomic<unsigned long long> cyclesDuringJoin{0};
    std::atomic<bool> cycleCompleted{false};

    const unsigned long long cyclesBefore = space.getGCCycleCount();

    // The helper is a raw std::thread that never touches a ProtoObject*, so it
    // is not a thread protoCore has to know about (rule 11's HoldsNothing
    // shape).  It requests a collection WHILE the main thread is inside the
    // runtime's join, then releases the joined thread whatever the outcome.
    std::thread helper([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            space.gcStarted = true;
            space.gcCV.notify_all();
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline) {
            if (space.getGCCycleCount() > cyclesBefore) {
                cycleCompleted.store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        cyclesDuringJoin.store(space.getGCCycleCount() - cyclesBefore);
        // Always release, so the case reports rather than hangs.
        release = true;
    });

    const bool supplied = host.joinBlockingThread(&release);
    joinReturned.store(true);
    helper.join();

    if (!supplied)
        return unavailable(kId, 2, "joinBlockingThread",
                           "rule 2b needs the runtime to start a thread through "
                           "its own facility and join it the way it joins its "
                           "own threads");

    const std::string common =
        "cyclesCompletedWhileJoinBlocked=" + std::to_string(cyclesDuringJoin.load())
        + " gcCycleCount " + std::to_string(cyclesBefore) + "->"
        + std::to_string(space.getGCCycleCount())
        + " runningThreads=" + std::to_string(space.runningThreads.load())
        + " reclaimedLastCycle="
        + std::to_string(space.reclaimedLastCycle.load(std::memory_order_relaxed));

    if (!cycleCompleted.load())
        return {kId, 2, Status::Fail,
                "no collection cycle could complete during the 8 s this runtime "
                "spent blocked in its own join.  The joining thread is still "
                "counted in runningThreads, so parkedThreads can never reach it "
                "and stop-the-world cannot begin; a thread that then needs "
                "memory waits for a cycle that cannot start.  Either the join "
                "is a direct std::thread::join on a registered thread (bracket "
                "it in ProtoContext::UnmanagedScope), or protoCore's own "
                "ProtoThread::join is not bracketing itself in this build.  "
                + common};

    return {kId, 2, Status::Pass,
            "a collection completed while this runtime was blocked in its own "
            "join, so the join did not hold the stop-the-world quorum.  "
            + common};
}

}}  // namespace proto::conformance
