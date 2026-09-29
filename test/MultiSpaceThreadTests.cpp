// MultiSpaceThreadTests.cpp -- one OS thread counted in several ProtoSpaces.
//
// Every ProtoSpace adopts the thread that constructs it as its main thread and
// counts it in its stop-the-world quorum (runningThreads starts at 1).  When one
// OS thread builds two spaces it is a member of both.  Before the multi-space
// presence rules (docs/GLOBAL_MUTABLE_TABLE.md, "Threads in several spaces"),
// such a thread only answered the stop-the-world of the space whose code it was
// running, and only left the quorum of that space when it blocked: a collection
// in the other space waited for it indefinitely.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace proto;

namespace {

struct Space {
    ProtoSpace   space;
    ProtoContext ctx{&space, space.rootContext, nullptr, nullptr, nullptr, nullptr};
};

struct CollectorShared {
    ProtoSpace* space = nullptr;
    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    uint64_t cycles = 0;
};
CollectorShared* gCollector = nullptr;

// A registered thread of `space` that allocates garbage under a low ceiling
// until the space has completed three collections.
const ProtoObject* collectorMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                 const ProtoList*, const ProtoSparseList*) {
    CollectorShared& s = *gCollector;
    ProtoSpace& space = *s.space;
    s.started = true;
    const uint64_t start = space.getGCCycleCount();
    for (int batch = 0; batch < 400 && space.getGCCycleCount() - start < 3; ++batch) {
        ProtoContext garbage(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 5000; ++i) {
            (void) garbage.newObject(false);
            if ((i & 1023) == 0) garbage.safepoint();
        }
        garbage.safepoint();
    }
    s.cycles = space.getGCCycleCount() - start;
    s.done = true;
    return PROTO_NONE;
}

const ProtoThread* startCollector(Space& target, CollectorShared& shared) {
    shared.space = &target.space;
    gCollector = &shared;
    target.space.setHeapLimits(0, target.space.heapSize + 40000);
    return target.space.newThread(target.space.rootContext,
                                  ProtoString::createSymbol(target.space.rootContext, "collector"),
                                  collectorMain, nullptr, nullptr);
}

void joinCollector(Space& target, const ProtoThread* t) {
    const_cast<ProtoThread*>(t)->join(target.space.rootContext);
    target.space.setHeapLimits(0, 0);
    gCollector = nullptr;
}

constexpr auto kDeadline = std::chrono::seconds(20);

}  // namespace

// The main thread runs only space A's code, reaching A's safepoints, while a
// thread of space B collects.  B's stop-the-world needs the main thread too.
TEST(MultiSpaceThread, AThreadRunningOneSpaceAnswersTheOtherSpacesStopTheWorld) {
    Space a, b;
    CollectorShared shared;
    const ProtoThread* t = startCollector(b, shared);
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    while (!shared.done.load() && std::chrono::steady_clock::now() < deadline) {
        ProtoContext work(&a.space, &a.ctx, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 1000; ++i) (void) work.newObject(false);
        work.safepoint();
    }
    EXPECT_TRUE(shared.done.load()) << "space B could not collect while the shared thread ran space A";
    EXPECT_GE(shared.cycles, 3u);
    joinCollector(b, t);
}

// The main thread blocks in an unmanaged region opened through space A.  It
// must be out of the quorum of every space it belongs to, not only A's.
TEST(MultiSpaceThread, AnUnmanagedRegionLeavesTheQuorumOfEverySpace) {
    Space a, b;
    CollectorShared shared;
    const ProtoThread* t = startCollector(b, shared);
    {
        ProtoContext::UnmanagedScope out(&a.ctx);
        const auto deadline = std::chrono::steady_clock::now() + kDeadline;
        while (!shared.done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(shared.done.load()) << "space B could not collect while the shared thread was unmanaged in A";
    EXPECT_GE(shared.cycles, 3u);
    joinCollector(b, t);
}
