// GCShutdownTests.cpp — a space that ends while a collection is pending or
// in flight.
//
// ~ProtoSpace marks the space ENDING and joins the GC thread.  Nothing a
// cycle reclaims at that point is ever reused, so the collector must not
// spend the teardown marking and sweeping, and it must not leave the
// stop-the-world flag raised for a mutator that is still running.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

using namespace proto;

namespace {

std::atomic<int> g_finalized{0};
void countFinalizer(void*) { g_finalized.fetch_add(1); }

// A cell that marks the space ENDING the first time the collector scans it
// during the stop-the-world root collection, i.e. while ~ProtoSpace would be
// blocked on globalMutex, and before mark and sweep start.
ProtoSpace* g_endingSpace = nullptr;
std::atomic<bool> g_endingArmed{false};

class EndSpaceDuringRootScanCell final : public Cell {
public:
    explicit EndSpaceDuringRootScanCell(ProtoContext* context) : Cell(context) {}
    void processReferences(ProtoContext*, void*,
                           void (*)(ProtoContext*, void*, const Cell*)) const override {
        // Runs on the GC thread, which holds globalMutex in Phase 2.
        if (g_endingArmed.exchange(false) && g_endingSpace) {
            g_endingSpace->state = SPACE_STATE_ENDING;
        }
    }
    const ProtoObject* implAsObject(ProtoContext*) const override { return PROTO_NONE; }
};

}  // namespace

// Once the space is ENDING, a cycle that has captured its roots must skip mark
// and sweep.  Garbage external pointers make sweep observable: sweep runs
// their finalizers.  Before the fix the cycle ran to completion and finalized
// every one of them during teardown.
TEST(GCShutdown, CycleStartedWhileEndingSkipsMarkAndSweep) {
    constexpr int kGarbage = 20000;
    g_finalized.store(0);
    auto* space = new ProtoSpace();
    ProtoContext* root = space->rootContext;
    {
        ProtoContext garbage(space, root, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kGarbage; ++i) {
            (void) garbage.fromExternalPointer(nullptr, countFinalizer);
        }
    }   // submitted: every external pointer is a sweep candidate

    {
        ProtoContext keeper(space, root, nullptr, nullptr, nullptr, nullptr);
        (void) new (&keeper) EndSpaceDuringRootScanCell(&keeper);
        g_endingSpace = space;
        g_endingArmed.store(true);
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            space->gcStarted = true;
            space->gcCV.notify_all();
        }
        // Park at safepoints until the GC thread has scanned the roots.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (g_endingArmed.load() && std::chrono::steady_clock::now() < deadline) {
            keeper.safepoint();
            std::this_thread::yield();
        }
        ASSERT_FALSE(g_endingArmed.load()) << "the collector never scanned the roots";
        // The GC loop ends by itself once the space is ENDING.
        space->gcThread->join();
    }

    EXPECT_FALSE(space->stwFlag.load());
    EXPECT_EQ(g_finalized.load(), 0)
        << "sweep finalized " << g_finalized.load() << " garbage cells after the space was ENDING";
    g_endingSpace = nullptr;
    delete space;
}

// ~ProtoSpace can end the space while the collector waits in Phase 1 for a
// thread that has not parked.  The collector must lower the stop-the-world
// flag when it gives up, or a mutator that is still running parks forever at
// its next safepoint.  Before the fix the flag stayed raised.
TEST(GCShutdown, EndingDuringStopTheWorldHandshakeLowersTheFlag) {
    auto* space = new ProtoSpace();
    // A running thread that never parks holds the collector in Phase 1.
    space->runningThreads.fetch_add(1);
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space->gcStarted = true;
        space->gcCV.notify_all();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!space->stwFlag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(space->stwFlag.load()) << "the collector did not start the handshake";

    // The first step of ~ProtoSpace: mark the space ENDING and wake the GC.
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space->state = SPACE_STATE_ENDING;
        space->gcCV.notify_all();
        space->stopTheWorldCV.notify_all();
    }
    space->gcThread->join();

    EXPECT_FALSE(space->stwFlag.load())
        << "the stop-the-world flag stayed raised after the collector left Phase 1";
    // A mutator that reaches a safepoint now must not block.
    space->runningThreads.fetch_sub(1);
    space->rootContext->safepoint();
    delete space;
}
