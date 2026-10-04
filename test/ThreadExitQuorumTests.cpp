// ThreadExitQuorumTests.cpp - an exiting thread stays in the stop-the-world
// quorum for as long as it is in `space->threads`.
//
// THE DEFECT (GitHub issue #3)
// ----------------------------
// thread_main used to decrement `runningThreads` as soon as the thread's body
// returned, and only then rebuild the threads list without itself - a rebuild
// that allocates cells in the thread's root context.  For that window the
// thread was still in `space->threads`, so every cycle's Phase 2 scanned its
// context and captured its young-chain head, but it no longer counted in the
// quorum `parkedThreads >= runningThreads`, so the collector could stop the
// world without it.  Two consequences:
//   * the collector read a running thread's context, and the concurrent
//     young-chain walk read cells that thread was still constructing
//     (ThreadSanitizer: Phase 4 young walk vs. the ProtoSparseListSmall
//     constructor reached from removeAt, about 3 runs in 40 of
//     GCRootScope.CandidateReachableOnlyFromAYoungCellSurvivesACycleForcedAtOnce);
//   * an allocation in that window could still park (parkIn counts the thread
//     into parkedThreads), so one uncounted parked thread could stand in for a
//     counted one that was still running.
//
// THE TEST
// --------
// ProtoSpace's test-only threadExitHook runs on the exiting thread exactly in
// that window: body returned, thread still in the list.  The hook requests a
// cycle, waits for the collector to raise stwFlag, and then watches it for a
// while without parking.  A thread in the list that has not parked must hold
// the world: stwFlag may not come down again until the thread parks or leaves
// the list.  Before the fix the collector reached its quorum without the
// thread and completed the stop-the-world under the hook's eyes, every time.

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

std::atomic<int> gHeld{0};          // the collector waited for the thread
std::atomic<int> gStoppedWithout{0}; // the collector stopped the world without it
std::atomic<int> gNoCycle{0};       // no stop-the-world was raised (inconclusive)

constexpr auto kRaiseWait = std::chrono::seconds(5);
constexpr auto kHoldWatch = std::chrono::milliseconds(300);

void watchTheQuorum(ProtoContext* context) {
    ProtoSpace* space = context->space;
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space->gcStarted = true;
        space->gcCV.notify_all();
    }
    const auto raiseDeadline = std::chrono::steady_clock::now() + kRaiseWait;
    while (!space->stwFlag.load() && std::chrono::steady_clock::now() < raiseDeadline) {
        std::this_thread::yield();
    }
    if (!space->stwFlag.load()) {
        gNoCycle.fetch_add(1);
        return;
    }
    const auto watchDeadline = std::chrono::steady_clock::now() + kHoldWatch;
    while (std::chrono::steady_clock::now() < watchDeadline) {
        if (!space->stwFlag.load()) {
            gStoppedWithout.fetch_add(1);
            return;
        }
        std::this_thread::yield();
    }
    gHeld.fetch_add(1);
}

const ProtoObject* allocatingBody(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                  const ProtoList*, const ProtoSparseList*) {
    for (int i = 0; i < 64; ++i) (void) ctx->newList()->appendLast(ctx, ctx->fromInteger(i));
    return PROTO_NONE;
}

struct HookGuard {
    HookGuard() { threadExitHook.store(&watchTheQuorum); }
    ~HookGuard() { threadExitHook.store(nullptr); }
};

}  // namespace

TEST(ThreadExitQuorum, AThreadStillInTheThreadsListHoldsTheStopTheWorld) {
    constexpr int kThreads = 3;
    gHeld = 0;
    gStoppedWithout = 0;
    gNoCycle = 0;

    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    {
        HookGuard hook;
        for (int t = 0; t < kThreads; ++t) {
            // One exiting thread at a time, with the main thread out of the
            // quorum while it joins, so the exiting thread is the only one
            // the collector has to wait for.
            const ProtoThread* thread = space.newThread(
                root, ProtoString::createSymbol(root, "thread-exit-quorum"),
                allocatingBody, nullptr, nullptr);
            ProtoContext::UnmanagedScope parked(root);
            const_cast<ProtoThread*>(thread)->join(root);
        }
    }

    EXPECT_EQ(gStoppedWithout.load(), 0)
        << "the collector stopped the world while an exiting thread that is still in "
           "space->threads was running";
    EXPECT_EQ(gHeld.load() + gStoppedWithout.load() + gNoCycle.load(), kThreads);
    EXPECT_GE(gHeld.load() + gStoppedWithout.load(), 1)
        << "no stop-the-world was raised while a thread exited: the test exercised nothing";
}
