// ThreadStartQuorumTests.cpp - a starting thread joins the stop-the-world
// quorum under globalMutex.
//
// THE DEFECT
// ----------
// A thread created by ProtoSpace::newThread is in `space->threads` before its
// OS thread runs, so every cycle's Phase 2 scans its context.  thread_main
// incremented `runningThreads` without the global mutex, then polled for a
// stop-the-world with relaxed loads.  The collector evaluates its quorum
// (parkedThreads >= runningThreads) and scans every thread's context without
// releasing that mutex; an increment that landed just after the quorum check
// left the thread out of a stop-the-world whose scan was still running, and
// the relaxed polls synchronise with nothing.  The thread's first context
// construction then rewrote ProtoThreadImplementation::context while the
// collector read it (ThreadSanitizer, core/Thread.cpp implSetCurrentContext
// vs. the Phase 2 thread scan in gcThreadLoop; seen once in CI in
// ProtoLazyObject.EightThreadsRealiseOneSharedChainUnderCollection).  On a
// weakly ordered CPU the relaxed poll could even read a stale stwFlag and let
// the thread run managed code during the scan.
//
// THE FIX
// -------
// thread_main joins `runningThreads` under globalMutex, which the collector
// holds from raising stwFlag through the end of its root scan.
//
// THE TESTS
// ---------
// NewThreadWaitsForAStopTheWorldInProgress is deterministic: the test holds
// globalMutex, as a collector does for its whole stop-the-world, while a
// thread starts.  Before the fix the thread's method ran inside that window
// (it was in the running set unsynchronised); with it, the thread waits for
// the mutex before joining the running set, so its method runs only after the
// window closes.
//
// ThreadsStartingDuringStopTheWorldsRunAndJoin is the stress form: threads start back to back while a plain OS thread requests cycles without
// pause, so thread starts keep landing inside stop-the-worlds.  Each thread's
// first act is to construct a context (the write the collector raced with)
// and allocate.  Without a sanitizer this only checks that every thread ran;
// under ThreadSanitizer (the CI job) the unfixed start reports the race.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace proto;

namespace {

std::atomic<int> gRan{0};

const ProtoObject* startingBody(ProtoContext* c, const ProtoObject*, const ParentLink*,
                                const ProtoArguments*, const ProtoKeywordArguments*) {
    ProtoContext first(c->space, c, nullptr, nullptr, nullptr, nullptr);
    for (int i = 0; i < 16; ++i) (void) first.newList()->appendLast(&first, first.fromInteger(i));
    gRan.fetch_add(1);
    return PROTO_NONE;
}

}  // namespace

TEST(ThreadStartQuorum, ThreadsStartingDuringStopTheWorldsRunAndJoin) {
    constexpr int kRounds = 200;
    constexpr int kThreadsPerRound = 6;
    gRan = 0;

    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    const uint64_t c0 = space.getGCCycleCount();

    std::atomic<bool> stop{false};
    std::thread kicker([&space, &stop] {
        while (!stop.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
                space.gcStarted = true;
                space.gcCV.notify_all();
            }
            std::this_thread::yield();
        }
    });

    for (int r = 0; r < kRounds; ++r) {
        std::vector<const ProtoThread*> threads;
        for (int t = 0; t < kThreadsPerRound; ++t)
            threads.push_back(space.newThread(
                root, ProtoString::createSymbol(root, "thread-start-quorum"),
                startingBody, nullptr, nullptr));
        ProtoContext::UnmanagedScope parked(root);
        for (const ProtoThread* th : threads) const_cast<ProtoThread*>(th)->join(root);
    }
    stop.store(true);
    kicker.join();

    EXPECT_EQ(gRan.load(), kRounds * kThreadsPerRound);
    EXPECT_GT(space.getGCCycleCount() - c0, 0u)
        << "no cycle ran while threads started: the test exercised nothing";
}

namespace {

std::atomic<bool> gStartedBodyRan{false};

const ProtoObject* markingBody(ProtoContext* c, const ProtoObject*, const ParentLink*,
                               const ProtoArguments*, const ProtoKeywordArguments*) {
    gStartedBodyRan.store(true);
    ProtoContext first(c->space, c, nullptr, nullptr, nullptr, nullptr);
    (void) first.newList()->appendLast(&first, first.fromInteger(1));
    return PROTO_NONE;
}

}  // namespace

TEST(ThreadStartQuorum, NewThreadWaitsForAStopTheWorldInProgress) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    gStartedBodyRan = false;

    const ProtoThread* thread = nullptr;
    bool ranInsideTheWindow = false;
    {
        // What a collector holds from raising stwFlag to the end of its root
        // scan.  The window is generous: the thread's method sets a flag as
        // its very first act.
        std::lock_guard<std::recursive_mutex> collectorWindow(ProtoSpace::globalMutex);
        thread = space.newThread(root, ProtoString::createSymbol(root, "thread-start-window"),
                                 markingBody, nullptr, nullptr);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (!gStartedBodyRan.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ranInsideTheWindow = gStartedBodyRan.load();
    }
    {
        ProtoContext::UnmanagedScope parked(root);
        const_cast<ProtoThread*>(thread)->join(root);
    }

    EXPECT_FALSE(ranInsideTheWindow)
        << "a starting thread ran managed code while globalMutex was held: it joined the "
           "running set outside the stop-the-world protocol";
    EXPECT_TRUE(gStartedBodyRan.load()) << "the thread never ran once the window closed";
}
