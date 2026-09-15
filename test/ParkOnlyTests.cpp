// ParkOnlyTests.cpp — ProtoContext::parkIfStopRequested().
//
// safepoint() does two things: it submits a context's young generation once
// the context has allocated more than maxAllocatedCellsPerContext cells, and
// it parks while a stop-the-world phase is requested.  Submission is only
// safe where every live young cell is reachable from a GC root.
// parkIfStopRequested() only parks: while parked, the young generation is
// scanned as roots and is not a sweep candidate, so cells held only in C++
// locals survive.

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

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) { hadOld_ = true; old_ = old; }
        if (value) setenv(name, value, 1); else unsetenv(name);
    }
    ~ScopedEnv() { if (hadOld_) setenv(name_, old_.c_str(), 1); else unsetenv(name_); }
private:
    const char* name_;
    bool hadOld_ = false;
    std::string old_;
};

// Request a cycle and wait for it with parkIfStopRequested() as the only park
// point.  Returns false when no cycle completed in time.
bool runCycleParkingOnly(ProtoSpace& space, ProtoContext* ctx) {
    const uint64_t start = space.getGCCycleCount();
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space.gcStarted = true;
        space.gcCV.notify_all();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        ctx->parkIfStopRequested();
        if (space.getGCCycleCount() > start && !space.gcStarted.load()) return true;
        std::this_thread::yield();
    }
    return false;
}

}  // namespace

TEST(ParkOnly, IsStopRequestedFollowsTheFlag) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    EXPECT_FALSE(ctx->isStopRequested());
    space.stwFlag.store(true);
    EXPECT_TRUE(ctx->isStopRequested());
    space.stwFlag.store(false);
    EXPECT_FALSE(ctx->isStopRequested());
    // Without a stop request the call returns at once.
    ctx->parkIfStopRequested();
}

// Young cells held only in C++ locals stay valid across cycles that stop the
// world while the thread parks through parkIfStopRequested(), even when the
// context is far past the submission threshold.  Garbage churn between the
// cycles recycles every freed cell, so a wrongly freed cell would be
// overwritten.
TEST(ParkOnly, YoungCellsInLocalsSurviveCyclesAcrossParkIfStopRequested) {
    ScopedEnv threshold("PROTOCORE_GC_CONTEXT_THRESHOLD", "16");
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    ProtoContext ctx(&space, root, nullptr, nullptr, nullptr, nullptr);

    constexpr int kLocals = 256;
    std::vector<const ProtoList*> locals;
    locals.reserve(kLocals);
    for (int i = 0; i < kLocals; ++i) {
        locals.push_back(ctx.newList()->appendLast(&ctx, ctx.fromInteger(i)));
    }
    const Cell* chainBefore = ctx.lastAllocatedCell;
    const unsigned long countBefore = ctx.allocatedCellsCount;
    ASSERT_GT(countBefore, space.maxAllocatedCellsPerContext);

    for (int round = 0; round < 3; ++round) {
        ASSERT_TRUE(runCycleParkingOnly(space, &ctx)) << "cycle " << round << " did not complete";
        // Garbage from another context, submitted and swept by the next cycle.
        ProtoContext garbage(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        for (int j = 0; j < 20000; ++j) (void) garbage.newObject(false);
    }
    ASSERT_TRUE(runCycleParkingOnly(space, &ctx));

    EXPECT_EQ(ctx.lastAllocatedCell, chainBefore)
        << "parkIfStopRequested() submitted the young generation";
    EXPECT_EQ(ctx.allocatedCellsCount, countBefore);
    for (int i = 0; i < kLocals; ++i) {
        ASSERT_EQ(locals[i]->getSize(&ctx), 1u) << "local " << i;
        EXPECT_EQ(locals[i]->getAt(&ctx, 0)->asLong(&ctx), i) << "local " << i;
    }
}

// The same scenario through safepoint() submits the young generation: the
// counter is reset and the chain handed to the collector.  This pins the
// difference between the two calls.
TEST(ParkOnly, SafepointSubmitsWhereParkIfStopRequestedDoesNot) {
#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    GTEST_SKIP() << "threshold submission requires PROTOCORE_GC_REINCLUDE_SURVIVORS";
#endif
    ScopedEnv threshold("PROTOCORE_GC_CONTEXT_THRESHOLD", "16");
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    for (int i = 0; i < 64; ++i) (void) ctx.newObject(false);
    const unsigned long count = ctx.allocatedCellsCount;
    ASSERT_GT(count, space.maxAllocatedCellsPerContext);
    ctx.parkIfStopRequested();
    EXPECT_EQ(ctx.allocatedCellsCount, count);
    ctx.safepoint();
    EXPECT_EQ(ctx.allocatedCellsCount, 0u);
}
