// AdaptiveHeapThreadsTests.cpp -- the adaptive heap controller's
// out-of-memory rule with several allocating threads and a small budget.
//
// Design: docs/specs/2026-10-02-adaptive-heap-controller-design.md, section
// 3.4: out of memory is declared only when the LIVE data cannot fit.  Until
// 2.14.3 the rule compared the cells a cycle left unreclaimed (heapSize minus
// the freelist) with the budget.  Those cells include, besides the live set,
// the refill batches threads hold, the young generations of calls still in
// progress and the cells allocated while the cycle ran; with a 64 MB budget
// and a few allocating threads they reached 75-90 % of the budget while the
// live set was about 25 % (protoJS bounded-memory test on Windows,
// 2026-10-10), and the rule came close to a false out-of-memory.
//
// The cases:
//   * the decision as a pure function (adaptive::outOfMemoryStrike), on
//     inputs that reproduce that measurement exactly;
//   * end to end, a small budget, several threads, a low live set, calls in
//     progress holding young cells, and heavy garbage: the run completes;
//   * end to end, the true out-of-memory still fires with several threads:
//     a live set larger than the budget, and the young-only retained set of
//     the single-threaded death test (AdaptiveHeapTests.cpp).
//
// The end-to-end cases run in a child process (EXPECT_EXIT): a false
// out-of-memory aborts, and the parent reports it as this test's failure
// with the child's message instead of losing the whole suite.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../core/AdaptiveHeap.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
static int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
static int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

using namespace proto;

namespace {

// The decision inputs of a 64 MB budget (1,048,576 cells) with two running
// threads, the configuration of protoJS's bounded-memory test.
adaptive::OutOfMemoryInputs smallBudget() {
    adaptive::OutOfMemoryInputs in;
    in.budgetCells = 1048576;
    in.runningThreads = 2;
    in.otherSpacesCells = 0;
    in.liveCells = 262144;          // 25 % of the budget
    in.retainedCells = 943718;      // 90 %: live + batches + young + cycle allocations
    in.reclaimedCells = 300000;     // the cycle freed garbage: progress
    return in;
}

}  // namespace

// --- The decision, as a pure function -----------------------------------------

// The measurement that came close to a false out-of-memory: 90 % of the
// budget unreclaimed after a cycle, 25 % live, the cycle reclaimed garbage.
// The live data fits, so it is no strike.
TEST(AdaptiveHeapOutOfMemoryRule, UnreclaimedReservationsAreNotLiveData) {
    const adaptive::OutOfMemoryInputs in = smallBudget();
    EXPECT_FALSE(adaptive::outOfMemoryStrike(in))
        << "a live set of 25 % of the budget was taken for out of memory";
}

// The live set itself does not fit under the budget less one refill batch
// per running thread: a strike, whatever the cycle reclaimed.
TEST(AdaptiveHeapOutOfMemoryRule, ALiveSetThatDoesNotFitIsAStrike) {
    adaptive::OutOfMemoryInputs in = smallBudget();
    in.liveCells = in.budgetCells - 1000;
    in.retainedCells = in.budgetCells;
    EXPECT_TRUE(adaptive::outOfMemoryStrike(in));
    // One refill batch per running thread is the margin (the batch cap of a
    // refill: budget / (8 x threads), within [512, 65536]).
    in.runningThreads = 2;
    const proto_ulong batch = in.budgetCells / 16;   // 65,536
    in.liveCells = in.budgetCells - 2 * batch;       // exactly at the margin: fits
    EXPECT_FALSE(adaptive::outOfMemoryStrike(in));
    in.liveCells += 1;
    EXPECT_TRUE(adaptive::outOfMemoryStrike(in));
}

// Other spaces count with their whole heap: memory never moves between
// spaces, so another space's free cells are of no use here.
TEST(AdaptiveHeapOutOfMemoryRule, OtherSpacesCountWithTheirWholeHeap) {
    adaptive::OutOfMemoryInputs in = smallBudget();
    in.otherSpacesCells = in.budgetCells - in.liveCells;   // live + others == budget
    EXPECT_TRUE(adaptive::outOfMemoryStrike(in));
}

// A cycle that reclaimed nothing while a thread waits at the ceiling is a
// strike even though the marked live set is small: the cells are held by
// calls in progress (young generations are pinned, never marked), and no
// cycle can do better until one of them returns -- which the waiting threads
// cannot.  Two in a row are the "one more cycle" of section 3.4, condition 2.
TEST(AdaptiveHeapOutOfMemoryRule, ACycleThatReclaimedNothingIsAStrike) {
    adaptive::OutOfMemoryInputs in = smallBudget();
    in.reclaimedCells = 0;
    EXPECT_TRUE(adaptive::outOfMemoryStrike(in));
}

// --- End to end ------------------------------------------------------------------

namespace {

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            hadValue_ = true;
            oldValue_ = old;
        }
        if (value) ::setenv(name, value, 1);
        else ::unsetenv(name);
    }
    ~ScopedEnv() {
        if (hadValue_) ::setenv(name_, oldValue_.c_str(), 1);
        else ::unsetenv(name_);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    bool hadValue_ = false;
    std::string oldValue_;
};

struct CleanEnv {
    ScopedEnv limit{"PROTOCORE_HEAP_LIMIT_CELLS", nullptr};
    ScopedEnv off{"PROTOCORE_ADAPTIVE_HEAP", nullptr};
    ScopedEnv trace{"PROTOCORE_HEAP_TRACE", nullptr};
};

// What every worker does, set by the case before the threads start.
struct Workload {
    // Cells each worker keeps in the young generation of a call that stays
    // in progress for the whole run (an interpreter frame that has not
    // returned): pinned by its context, never marked, never reclaimable.
    proto_ulong pinnedCells = 0;
    // Short calls of 100 objects each, all garbage once the call returns.
    int garbageCalls = 0;
    // A live list in a root set that grows without bound (true out of memory).
    bool growLiveForever = false;

    std::atomic<int> started{0};
    std::atomic<bool> go{false};
    std::atomic<int> done{0};
};
Workload* gWork = nullptr;

const ProtoObject* workerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoList*, const ProtoSparseList*) {
    Workload& w = *gWork;
    ProtoThread* self = const_cast<ProtoThread*>(ctx->thread);
    ProtoSpace* space = ctx->space;
    w.started.fetch_add(1);
    {
        ProtoContext::UnmanagedScope parked(ctx);
        while (!w.go.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (w.growLiveForever) {
        ProtoRootSet* rs = space->createRootSet("adaptive-heap-threads");
        ProtoRootSet::Handle h = rs->add(ctx->newList()->asObject(ctx));
        for (;;) {
            ProtoContext sub(space, ctx, nullptr, nullptr, nullptr, nullptr);
            const ProtoList* list = rs->resolve(h)->asList(&sub);
            for (int i = 0; i < 1000; ++i) list = list->appendLast(&sub, sub.newObject(false));
            rs->remove(h);
            h = rs->add(list->asObject(&sub));
            self->synchToGC();
        }
    }
    // The call in progress.  Only synchToGC between the short calls: a
    // safepoint() on this context would submit its young generation once
    // past the context threshold, and the pinned cells would become garbage.
    ProtoContext inProgress(space, ctx, nullptr, nullptr, nullptr, nullptr);
    while (inProgress.allocatedCellsCount < w.pinnedCells) (void) inProgress.newObject(false);
    for (int call = 0; call < w.garbageCalls; ++call) {
        {
            ProtoContext garbage(space, &inProgress, nullptr, nullptr, nullptr, nullptr);
            for (int i = 0; i < 100; ++i) (void) garbage.newObject(false);
        }
        self->synchToGC();
    }
    w.done.fetch_add(1);
    return PROTO_NONE;
}

// Starts `threads` workers on `space`, releases them together and joins them.
void runWorkers(ProtoSpace& space, int threads, Workload& w) {
    ProtoContext* root = space.rootContext;
    gWork = &w;
    std::vector<const ProtoThread*> handles;
    for (int t = 0; t < threads; ++t)
        handles.push_back(space.newThread(root, ProtoString::createSymbol(root, "oom-worker"),
                                          workerMain, nullptr, nullptr));
    {
        ProtoContext::UnmanagedScope parked(root);
        while (w.started.load() < threads) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        w.go = true;
    }
    for (const ProtoThread* h : handles) const_cast<ProtoThread*>(h)->join(root);
    gWork = nullptr;
}

// A live set reachable from a root set, built in chunks (about three cells
// per element stay live: the object and the list's nodes).
void buildRootedList(ProtoSpace& space, int elements) {
    ProtoRootSet* rs = space.createRootSet("adaptive-heap-threads-live");
    ProtoRootSet::Handle h = rs->add(space.rootContext->newList()->asObject(space.rootContext));
    int done = 0;
    while (done < elements) {
        ProtoContext sub(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* list = rs->resolve(h)->asList(&sub);
        const int chunk = std::min(10000, elements - done);
        for (int i = 0; i < chunk; ++i) list = list->appendLast(&sub, sub.newObject(false));
        rs->remove(h);
        h = rs->add(list->asObject(&sub));
        done += chunk;
        space.rootContext->safepoint();
    }
}

}  // namespace

// The shape of the protoJS false out-of-memory: a 64 MB budget, four
// allocating threads, a live set of about a quarter of the budget, and
// heavy garbage (about 20 times the budget).  Each thread also keeps a call in
// progress holding 150,000 young cells, so that the cells no cycle can
// reclaim -- live set, pinned young cells, the threads' refill batches and
// what they allocate while a cycle runs -- sit at 85-95 % of the budget at
// every cycle end (PROTOCORE_HEAP_TRACE on Linux x86-64), around the old
// rule's threshold, while the marked live set stays near 40 %.  The run must
// complete, with collections.  On Linux the 2.14.3 rule did not abort here
// in local runs (a thread woken by published cells rarely reaches the
// check); the deterministic fail-first is the pure-function case above.
TEST(AdaptiveHeapThreadsDeathTest, LowLiveSetWithHeavyGarbageDoesNotRunOutOfMemory) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT({
        CleanEnv env;
        bool completed = false;
        {
            ProtoSpace space;
            AdaptiveHeapConfig c;
            c.hardCells = 1048576;   // 64 MB
            space.enableAdaptiveHeap(c);
            buildRootedList(space, 85000);   // ~255,000 live cells, ~25 %
            Workload w;
            w.pinnedCells = 150000;          // 4 x 150,000 = 57 % of the budget
            w.garbageCalls = 25000;          // 4 x 2.5 M objects of garbage
            runWorkers(space, 4, w);
            const AdaptiveHeapStats s = space.adaptiveHeapStats();
            std::fprintf(stderr, "[ threads ] done=%d cycles=%llu L=%lu heap=%lu H=%lu\n",
                         w.done.load(), (unsigned long long) s.cycles,
                         (unsigned long) s.liveCellsLastCycle, (unsigned long) s.heapCells,
                         (unsigned long) s.hardCells);
            completed = w.done.load() == 4 && s.cycles > 0;
        }
        // After the space is gone: a process exit with a live collector is
        // ProcessExitTests.cpp's subject, not this case's.
        std::exit(completed ? 0 : 3);
    }, ::testing::ExitedWithCode(0), "");
}

// True out of memory with several threads: four threads grow live lists in
// root sets without bound.  The live set passes the budget; the callback
// fires once, then the controlled abort.
static ProtoObject* threadsOomMarker(ProtoContext*) {
    std::fprintf(stderr, "OOM_CALLBACK_FIRED ");
    std::fflush(stderr);
    return nullptr;
}

TEST(AdaptiveHeapThreadsDeathTest, LiveSetLargerThanTheBudgetIsOutOfMemory) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_DEATH({
        CleanEnv env;
        ProtoSpace space;
        space.outOfMemoryCallback = threadsOomMarker;
        AdaptiveHeapConfig c;
        c.hardCells = 1048576;   // 64 MB
        space.enableAdaptiveHeap(c);
        Workload w;
        w.growLiveForever = true;
        runWorkers(space, 4, w);
    }, "OOM_CALLBACK_FIRED protoCore: adaptive heap hard limit");
}

// True out of memory with several threads where nothing is ever marked: each
// thread's call in progress holds young cells without bound.  No cycle
// reclaims anything; the threads wait at the ceiling; the abort follows.
TEST(AdaptiveHeapThreadsDeathTest, YoungCellsOfCallsInProgressThatDoNotFitAreOutOfMemory) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_DEATH({
        CleanEnv env;
        ProtoSpace space;
        space.outOfMemoryCallback = threadsOomMarker;
        AdaptiveHeapConfig c;
        c.hardCells = 1048576;   // 64 MB
        space.enableAdaptiveHeap(c);
        Workload w;
        w.pinnedCells = 2000000;   // each thread alone exceeds the budget
        runWorkers(space, 4, w);
    }, "OOM_CALLBACK_FIRED protoCore: adaptive heap hard limit");
}
