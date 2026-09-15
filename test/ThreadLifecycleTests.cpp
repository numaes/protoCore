// ThreadLifecycleTests.cpp — stop-the-world accounting of ProtoThreads across
// their exit.
//
// The stop-the-world quorum is `parkedThreads >= runningThreads`.  A thread
// must stay counted in `runningThreads` for as long as it can still execute
// protoCore code — including the allocations its exit path makes while it
// removes itself from `space->threads` — or a park on that path counts it as
// parked without counting it as running, and the quorum is met while another
// mutator still runs.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace proto;

namespace {

std::atomic<bool> g_started{false};
std::atomic<bool> g_release{false};

// Thread body: announce the start, then wait (without touching protoCore)
// until the test releases it, and return.
const ProtoObject* waitForRelease(ProtoContext*, const ProtoObject*, const ParentLink*,
                                  const ProtoList*, const ProtoSparseList*) {
    g_started.store(true);
    while (!g_release.load()) std::this_thread::yield();
    return PROTO_NONE;
}

void joinUnmanaged(ProtoContext* ctx, const ProtoThread* thread) {
    // join() blocks; leave the stop-the-world quorum while it does.
    ProtoContext::UnmanagedScope unmanaged(ctx);
    const_cast<ProtoThread*>(thread)->join(ctx);
}

}  // namespace

// The test plays the collector: it raises stwFlag itself (the GC thread stays
// asleep, no cycle was requested) and samples the quorum counters under
// globalMutex, as Phase 1 does.  The test thread keeps running and never
// parks, so the quorum must never be met.  Before the fix, thread_main
// decremented runningThreads before rebuilding space->threads; that rebuild
// allocates, the allocation parked on the raised flag, and the counters read
// parked 1 >= running 1 while this thread was still running.  The park
// happens in builds with -DPROTOCORE_GC_REINCLUDE_SURVIVORS=OFF, where the
// allocation poll ignores the critical section that removeAt opens; with the
// re-chain enabled the old exit path did not park there, and the test
// guards the ordering.
TEST(ThreadLifecycle, ExitingThreadStaysCountedUntilUnregistered) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    ASSERT_EQ(space.runningThreads.load(), 1);
    ASSERT_EQ(space.parkedThreads.load(), 0);

    g_started.store(false);
    g_release.store(false);
    const ProtoThread* worker = space.newThread(root, nullptr, waitForRelease, nullptr, nullptr);
    while (!g_started.load()) std::this_thread::yield();

    space.stwFlag.store(true);
    g_release.store(true);

    bool quorumMetWhileRunning = false;
    int parkedSeen = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        int parked, running;
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            parked = space.parkedThreads.load();
            running = space.runningThreads.load();
        }
        if (parked > parkedSeen) parkedSeen = parked;
        if (parked >= running) {
            quorumMetWhileRunning = true;
            break;
        }
        std::this_thread::yield();
    }

    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space.stwFlag.store(false);
        space.stopTheWorldCV.notify_all();
    }
    joinUnmanaged(root, worker);

    EXPECT_FALSE(quorumMetWhileRunning)
        << "the stop-the-world quorum was met (parked " << parkedSeen
        << ") while the test thread was still running: the exiting thread was "
           "counted as parked but no longer as running";
    EXPECT_EQ(space.runningThreads.load(), 1);
    EXPECT_EQ(space.parkedThreads.load(), 0);
}
