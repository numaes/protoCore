// UnmanagedScopeWakeupTests.cpp -- the collector handshake of unmanaged
// regions once entering one no longer wakes the collector unconditionally.
//
// Up to 3.3.0 every outermost UnmanagedScope entry took the global mutex and
// notified the collector's condition variable, waking an idle collector once
// per entry.  The entry now notifies only when a stop-the-world is raised,
// which is the only time the collector waits for the quorum
// (core/MultiSpace.cpp, "Leaving and rejoining the quorum").  These tests pin
// what that must not break:
//
//   * no lost wakeup: a collector already waiting for the quorum is woken by
//     the thread that completes it by entering an unmanaged region;
//   * a cycle completes while a thread sits inside an unmanaged region;
//   * many threads entering and leaving regions while cycles run under a low
//     heap ceiling: no hang, no cell of a live structure freed (a thread
//     running managed code during a stop-the-world would expose it), and the
//     quorum counters back to their starting value.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace proto;

namespace {

// Aborts the process if a test hangs: a lost wakeup is a hang, and a hang
// detector two orders of magnitude above the expected time keeps the
// failure readable instead of a ctest timeout.
class Watchdog {
public:
    Watchdog(const char* what, int seconds) : what_(what) {
        thread_ = std::thread([this, seconds] {
            for (int i = 0; i < seconds * 20 && !finished_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!finished_.load()) {
                std::fprintf(stderr, "UnmanagedScopeWakeup: %s hung\n", what_);
                std::abort();
            }
        });
    }
    ~Watchdog() {
        finished_ = true;
        thread_.join();
    }
private:
    const char* what_;
    std::atomic<bool> finished_{false};
    std::thread thread_;
};

// Fills the heap with garbage until fewer than 20 % of its cells are free,
// so that triggerGC starts a cycle (it ignores a request otherwise), and lets
// any cycle the filling started finish first: a request made during a cycle
// is folded into it.
void fillWithGarbage(ProtoSpace& space) {
    for (int tries = 0; tries < 64; ++tries) {
        {
            ProtoContext garbage(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
            while (static_cast<long long>(relaxedLoad(space.freeCellsCount)) * 5
                   >= relaxedLoad(space.heapSize))
                (void) garbage.newObject(false);
        }
        while (space.gcStarted.load()) {
            space.rootContext->safepoint();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        if (static_cast<long long>(relaxedLoad(space.freeCellsCount)) * 5
            < relaxedLoad(space.heapSize))
            return;
    }
}

// ---- 1. The thread completing the quorum by entering a region wakes the GC --

struct LateEntry {
    std::atomic<bool> go{false};
    std::atomic<bool> ready{false};   // past its last safepoint
    std::atomic<bool> stwSeen{false};
    std::atomic<bool> inside{false};
    std::atomic<bool> release{false};
    int delayUs = 0;
};
LateEntry* gLate = nullptr;

// Once told to go, runs managed code with no safepoint until the
// stop-the-world is raised, so the collector waits for this thread, then
// enters an unmanaged region: the entry is what completes the quorum.
const ProtoObject* lateEntryMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                 const ProtoArguments*, const ProtoKeywordArguments*) {
    LateEntry& s = *gLate;
    while (!s.go.load()) {
        ctx->safepoint();   // answers the cycles the main thread's filling starts
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    s.ready = true;
    while (!ctx->space->stwFlag.load(std::memory_order_seq_cst)) std::this_thread::yield();
    s.stwSeen = true;
    if (s.delayUs) std::this_thread::sleep_for(std::chrono::microseconds(s.delayUs));
    {
        ProtoContext::UnmanagedScope out(ctx);
        s.inside = true;
        while (!s.release.load()) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return PROTO_NONE;
}

}  // namespace

TEST(UnmanagedScopeWakeup, EnteringARegionWakesACollectorWaitingForTheQuorum) {
    ProtoSpace space;
    Watchdog watchdog("the late-entry rounds", 120);
    for (int round = 0; round < 40; ++round) {
        LateEntry s;
        // Even rounds enter at once (the entry races the collector's wait),
        // odd rounds after the collector has certainly started waiting.
        s.delayUs = (round % 2) ? 2000 : 0;
        gLate = &s;
        const ProtoThread* t = space.newThread(space.rootContext,
                                               ProtoString::createSymbol(space.rootContext, "late"),
                                               lateEntryMain, nullptr, nullptr);
        fillWithGarbage(space);
        const uint64_t before = space.getGCCycleCount();
        {
            // The main thread leaves the quorum too, so the late thread is the
            // last one the collector waits for.
            ProtoContext::UnmanagedScope out(space.rootContext);
            s.go = true;
            while (!s.ready.load()) std::this_thread::yield();
            space.triggerGC();
            while (space.getGCCycleCount() == before)
                std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        // The late thread saw the stop-the-world raised while it ran managed
        // code: the cycle could only start once its entry completed the
        // quorum.  (`inside` is stored after the entry, so it may lag.)
        EXPECT_TRUE(s.stwSeen.load());
        while (!s.inside.load()) std::this_thread::sleep_for(std::chrono::microseconds(50));
        s.release = true;
        const_cast<ProtoThread*>(t)->join(space.rootContext);
        gLate = nullptr;
    }
    EXPECT_EQ(space.parkedThreads.load(), 0);
}

// ---- 2. A cycle completes while a thread sits inside a region --------------

namespace {
struct Sitter {
    std::atomic<bool> inside{false};
    std::atomic<bool> release{false};
};
Sitter* gSitter = nullptr;

const ProtoObject* sitterMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoArguments*, const ProtoKeywordArguments*) {
    ProtoContext::UnmanagedScope out(ctx);
    gSitter->inside = true;
    while (!gSitter->release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return PROTO_NONE;
}
}  // namespace

TEST(UnmanagedScopeWakeup, ACycleCompletesWhileAThreadSitsInARegion) {
    ProtoSpace space;
    Watchdog watchdog("a cycle with a thread inside a region", 60);
    Sitter s;
    gSitter = &s;
    const ProtoThread* t = space.newThread(space.rootContext,
                                           ProtoString::createSymbol(space.rootContext, "sitter"),
                                           sitterMain, nullptr, nullptr);
    while (!s.inside.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    for (int i = 0; i < 5; ++i) {
        fillWithGarbage(space);
        const uint64_t before = space.getGCCycleCount();
        space.triggerGC();
        while (space.getGCCycleCount() == before) space.rootContext->safepoint();
    }
    s.release = true;
    const_cast<ProtoThread*>(t)->join(space.rootContext);
    gSitter = nullptr;
    EXPECT_EQ(space.parkedThreads.load(), 0);
}

// ---- 3. Many threads in and out of regions while cycles run ----------------

namespace {
struct Stress {
    ProtoSpace* space = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<int> corrupted{0};
    std::atomic<long> scopes{0};
};
Stress* gStress = nullptr;

constexpr int kCanary = 64;

// Holds a list in its context across thousands of regions and cycles and
// checks it after every return: a thread counted as parked while it runs
// managed code, or a stop-the-world it did not wait out, shows up as a freed
// or overwritten cell.
const ProtoObject* scopeChurnMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                  const ProtoArguments*, const ProtoKeywordArguments*) {
    Stress& st = *gStress;
    const ProtoList* canary = ctx->newList();
    for (int i = 0; i < kCanary; ++i) canary = canary->appendLast(ctx, ctx->fromInteger(i * 7));
    for (long i = 0; !st.stop.load(std::memory_order_relaxed); ++i) {
        {
            ProtoContext::UnmanagedScope out(ctx);
            if ((i & 63) == 0) std::this_thread::yield();
        }
        if ((i & 15) == 0) {
            // Managed work between regions, in a child context so the
            // canary's context never hands its young cells to the collector.
            ProtoContext work(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
            for (int k = 0; k < 200; ++k) (void) work.newObject(false);
        }
        if ((i & 255) == 0) {
            bool ok = canary->getSize(ctx) == static_cast<unsigned long>(kCanary);
            for (int k = 0; ok && k < kCanary; ++k)
                ok = canary->getAt(ctx, k)->asLong(ctx) == k * 7;
            if (!ok) st.corrupted.fetch_add(1);
            st.scopes.fetch_add(256, std::memory_order_relaxed);
        }
    }
    return PROTO_NONE;
}

const ProtoObject* garbageMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                               const ProtoArguments*, const ProtoKeywordArguments*) {
    Stress& st = *gStress;
    while (!st.stop.load(std::memory_order_relaxed)) {
        ProtoContext garbage(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 4000; ++i) {
            (void) garbage.newObject(false);
            if ((i & 511) == 0) garbage.safepoint();
        }
    }
    return PROTO_NONE;
}
}  // namespace

TEST(UnmanagedScopeWakeup, ManyThreadsInAndOutOfRegionsWhileCyclesRun) {
    ProtoSpace space;
    Watchdog watchdog("the region stress", 240);
    Stress st;
    st.space = &space;
    gStress = &st;
    space.setHeapLimits(0, space.heapSize + 40000);
    const int before = space.parkedThreads.load();
    std::vector<const ProtoThread*> threads;
    for (int i = 0; i < 6; ++i)
        threads.push_back(space.newThread(space.rootContext,
                                          ProtoString::createSymbol(space.rootContext, "churn"),
                                          scopeChurnMain, nullptr, nullptr));
    for (int i = 0; i < 2; ++i)
        threads.push_back(space.newThread(space.rootContext,
                                          ProtoString::createSymbol(space.rootContext, "garbage"),
                                          garbageMain, nullptr, nullptr));
    const uint64_t start = space.getGCCycleCount();
    {
        ProtoContext::UnmanagedScope out(space.rootContext);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        // Both conditions: on a fast machine 30 cycles take a fraction of a
        // second, before the region threads have done much.
        while ((space.getGCCycleCount() - start < 30 || st.scopes.load() < 50000)
               && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        st.stop = true;
    }
    for (const ProtoThread* t : threads) const_cast<ProtoThread*>(t)->join(space.rootContext);
    space.setHeapLimits(0, 0);
    gStress = nullptr;
    EXPECT_GE(space.getGCCycleCount() - start, 30u) << "the collector did not keep cycling";
    EXPECT_GE(st.scopes.load(), 50000);
    EXPECT_EQ(st.corrupted.load(), 0);
    EXPECT_EQ(space.parkedThreads.load(), before);
}
