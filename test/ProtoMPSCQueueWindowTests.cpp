// ProtoMPSCQueueWindowTests.cpp - the nodes a producer prepends INSIDE
// takeAll's publish window.
//
// WHY THIS FILE EXISTS, AND WHY IT IS NOT A STRESS TEST
// ----------------------------------------------------
// `takeAll` publishes its retain cell with the chain it LOADED, then detaches
// a possibly longer chain, then walks that chain parking for stop-the-world
// every 64 nodes.  Until the widening store in core/ProtoMPSCQueue.cpp, the
// nodes a producer prepended between the load and the detach hung off nothing
// but a C++ local for the length of the walk, so any cycle that began at one
// of those polls freed them and the items they were the only reference to.
//
// In the field that showed up as: of a 182-message batch, exactly one element
// lost every own attribute, and it was always the LAST - the chain is LIFO, so
// the newest node, the one prepended inside the window, comes out last.
//
// Reproducing it by racing a producer against a consumer gave 4 failures in 40
// runs before the fix and 0 in 40 after.  That is p ~ 0.12 under the null
// hypothesis: it does not distinguish the fix from luck, and a flaky
// reproduction is not a test.  The window is a few instructions wide and its
// location is known exactly, so instead of racing for it these tests ENTER it,
// through `proto::pmqTakeAllWindowHook` (headers/proto_internal.h).  Both
// tests below fail on every run when the widening store is removed, and pass
// on every run with it.
//
// Two tests, because they falsify different things:
//
//   1. RetainCellCoversEveryDetachedNode - the invariant, structurally, with
//      no collector involved: after a takeAll, every node it detached is
//      reachable from `retained`.  Deterministic by construction.
//
//   2. AWindowPrependSurvivesACollectionDuringTheDrain - the loss itself.  A
//      canary Cell is the only thing the window node points at, a cycle is
//      forced to run while `takeAll` is still inside itself, and the canary's
//      own `finalize` override says whether the sweep took it.  That is a
//      direct observation of the collection, not an inference from corrupted
//      contents, so it needs no reuse of the freed cell to become visible.
//
// Both hooks stand in for a producer and do only what a producer does: push.
// Neither blocks, waits on another protoCore thread, or touches
// ProtoSpace::globalMutex - the consumer is at criticalSectionDepth > 0 inside
// the window and cannot park, so a collector holding that mutex while waiting
// for the stop-the-world quorum would deadlock against it.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace proto;

namespace {

//--------------------------------------------------------------------------
// What the hook pushes, and where it pushes from.
//--------------------------------------------------------------------------
// A node prepended inside the window only becomes a sweep CANDIDATE once the
// context that allocated it hands its young generation to the space - a live
// context's young chain is never a candidate, which is exactly why the field
// failures needed producers that end a turn.  `submitYoungGeneration` is the
// lock-free primitive behind that hand-off (core/ProtoSpace.cpp), so the hook
// calls it directly on a context the test owns: same effect as a producer
// ending its turn, without destroying a ProtoContext inside the window (that
// destructor takes ProtoSpace::globalMutex, which the window must not).
struct WindowHookState {
    const ProtoMPSCQueue* queue{nullptr};
    ProtoContext* producer{nullptr};      // the context the prepend is charged to
    const ProtoObject* item{nullptr};     // pre-built, so the hook allocates only the node
    PmqWindowPhase at{PmqWindowPhase::BeforeDetach};
    bool submitYoung{false};
    bool armCycle{false};
    std::atomic<int> fired{0};
    std::atomic<bool> armed{false};
    std::atomic<bool> stwSeen{false};
};

WindowHookState* g_hook = nullptr;

// Asks the collector for a cycle and returns once it has ANNOUNCED the pause,
// so that the caller knows the next stop-the-world poll it reaches is the one
// that completes that pause.
//
// Both halves are deadlock-free from inside the window, and the split matters:
//   * `gcStarted` is set under globalMutex, because `gcCV.wait` re-reads it
//     under that mutex and a lock-free store races a lost wakeup.  The mutex
//     is free here: the test waits for an idle collector before the takeAll,
//     nothing sets a heap ceiling, so no cycle is in flight and nobody else
//     holds it.  The acquisition is still bounded by try_lock attempts, so
//     even a surprise holder produces a reported failure and not a hang.
//   * the wait for `stwFlag` holds NO lock.  The collector raises that flag at
//     the top of the pause and only THEN waits for the quorum while holding
//     globalMutex; waiting for the flag with the mutex held would deadlock
//     against exactly that.
bool armPauseFromWindow(ProtoSpace* space) {
    bool requested = false;
    for (int attempt = 0; attempt < 1000 && !requested; ++attempt) {
        if (ProtoSpace::globalMutex.try_lock()) {
            space->gcStarted = true;
            space->gcCV.notify_all();
            ProtoSpace::globalMutex.unlock();
            requested = true;
        } else {
            std::this_thread::yield();
        }
    }
    if (!requested) return false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!space->stwFlag.load(std::memory_order_relaxed)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

void windowHook(ProtoContext*, const ProtoMPSCQueue* queue, PmqWindowPhase phase) {
    WindowHookState* s = g_hook;
    if (!s || phase != s->at || queue != s->queue) return;
    if (s->fired.fetch_add(1, std::memory_order_relaxed) != 0) return;   // once

    // The only allocation is the queue node, and `push` makes it inside its
    // own CriticalSection, so it cannot park - just as a real producer's
    // cannot be interrupted mid-publish.
    s->queue->push(s->producer, s->item);

    if (s->submitYoung && s->producer->lastAllocatedCell) {
        s->producer->space->submitYoungGeneration(s->producer->lastAllocatedCell);
        s->producer->lastAllocatedCell = nullptr;
    }

    if (s->armCycle) {
        s->armed.store(true, std::memory_order_relaxed);
        s->stwSeen.store(armPauseFromWindow(s->producer->space), std::memory_order_relaxed);
    }
}

struct HookInstaller {
    explicit HookInstaller(WindowHookState* s) {
        g_hook = s;
        pmqTakeAllWindowHook.store(&windowHook, std::memory_order_relaxed);
    }
    ~HookInstaller() {
        pmqTakeAllWindowHook.store(nullptr, std::memory_order_relaxed);
        g_hook = nullptr;
    }
};

//--------------------------------------------------------------------------
// Helpers
//--------------------------------------------------------------------------
using NodeCell   = ProtoMPSCQueueNodeImplementation;
using RetainCell = ProtoMPSCQueueRetainImplementation;

const ProtoMPSCQueueImplementation* implOf(const ProtoMPSCQueue* q) {
    return toImpl<const ProtoMPSCQueueImplementation>(q);
}

// How many nodes the collector can reach through `retained`.  `retained` is a
// stack of retain cells and each carries one chain; only the newest matters
// here because each test does exactly one takeAll.
unsigned long nodesReachableFromRetained(const ProtoMPSCQueue* q) {
    const RetainCell* r = implOf(q)->retained.load(std::memory_order_acquire);
    if (!r) return 0;
    unsigned long n = 0;
    for (const NodeCell* c = r->chain.load(std::memory_order_acquire); c;
         c = c->next.load(std::memory_order_acquire))
        ++n;
    return n;
}

bool waitForIdleCollector(ProtoSpace& space, ProtoContext* ctx) {
    ProtoContext::UnmanagedScope parked(ctx);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (space.gcStarted.load()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

//--------------------------------------------------------------------------
// The canary
//--------------------------------------------------------------------------
// A Cell with no references whose finalize override records that the sweep
// reclaimed it.  finalize is called from exactly one place - the sweep's inner
// loop, on a cell found unreachable (core/ProtoSpace.cpp) - so a non-zero
// `finalized` is a direct statement that the collector freed a cell that was
// still an element of the list `takeAll` returned.  Recording a number in
// collector bookkeeping is what the finalizer contract
// (docs/GarbageCollector.md section 7) permits and nothing more happens here.
class CanaryCell final : public Cell {
public:
    static std::atomic<unsigned long> finalized;
    static std::atomic<unsigned long> traced;

    explicit CanaryCell(ProtoContext* context) : Cell(context) {}

    void finalize(ProtoContext*) const override {
        finalized.fetch_add(1, std::memory_order_relaxed);
    }
    void processReferences(ProtoContext*, void*,
                           void (*)(ProtoContext*, void*, const Cell*)) const override {
        traced.fetch_add(1, std::memory_order_relaxed);
    }
    const ProtoObject* implAsObject(ProtoContext*) const override {
        return reinterpret_cast<const ProtoObject*>(this);
    }
};
std::atomic<unsigned long> CanaryCell::finalized{0};
std::atomic<unsigned long> CanaryCell::traced{0};

}  // namespace

//===========================================================================
// 1. The invariant: `retained` covers every node the detach took.
//===========================================================================
// No collector, no timing, nothing to tune.  The hook prepends one node inside
// the window; the batch therefore has kFill + 1 items, and every one of those
// nodes must be reachable from `retained`, because that is the only GC root
// the chain has once `head` has been emptied.
//
// Removing `retain->chain.store(chain, ...)` from takeAll makes this report
// kFill against kFill + 1, on every run, for both phases.
class MPSCQueueWindowPhase : public ::testing::TestWithParam<PmqWindowPhase> {};

TEST_P(MPSCQueueWindowPhase, RetainCellCoversEveryDetachedNode) {
    constexpr long kFill = 100;

    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoRootSet* rs = space.createRootSet("mpsc-window-invariant");
    ASSERT_NE(rs, nullptr);

    const ProtoMPSCQueue* q = live.newMPSCQueue();
    const ProtoRootSet::Handle pinned = rs->add(q->asObject(&live));
    ASSERT_NE(pinned, ProtoRootSet::kNullHandle);

    for (long i = 0; i < kFill; ++i) q->push(&live, live.fromInteger(i));

    ProtoContext producer(&space, &live, nullptr, nullptr, nullptr, nullptr);
    WindowHookState state;
    state.queue = q;
    state.producer = &producer;
    state.item = producer.fromInteger(kFill);
    state.at = GetParam();
    state.submitYoung = false;

    const ProtoList* batch = nullptr;
    {
        HookInstaller installed(&state);
        batch = q->takeAll(&live);
    }

    ASSERT_EQ(state.fired.load(), 1) << "the hook never entered the publish window";
    ASSERT_EQ(batch->getSize(&live), static_cast<unsigned long>(kFill + 1));
    EXPECT_EQ(batch->getAt(&live, static_cast<int>(kFill))->asLong(&live), kFill)
        << "the node prepended inside the window must be the last item out";

    const unsigned long covered = nodesReachableFromRetained(q);
    std::printf("[ WINDOW   ] phase=%s batch=%lu reachable from retained=%lu\n",
                GetParam() == PmqWindowPhase::AfterHeadLoad ? "after-head-load"
                                                           : "before-detach",
                batch->getSize(&live), covered);
    std::fflush(stdout);

    EXPECT_EQ(covered, static_cast<unsigned long>(kFill + 1))
        << "takeAll detached " << (kFill + 1) << " nodes but only " << covered
        << " of them are reachable from `retained`.  The uncovered ones hang off "
           "nothing but a C++ local for the length of the O(batch) walk, which "
           "parks for stop-the-world every 64 nodes";

    rs->remove(pinned);
}

INSTANTIATE_TEST_SUITE_P(BothWindowPhases, MPSCQueueWindowPhase,
                         ::testing::Values(PmqWindowPhase::AfterHeadLoad,
                                           PmqWindowPhase::BeforeDetach),
                         [](const ::testing::TestParamInfo<PmqWindowPhase>& i) {
                             return i.param == PmqWindowPhase::AfterHeadLoad
                                        ? "AfterHeadLoad" : "BeforeDetach";
                         });

//===========================================================================
// 2. The loss: a cycle that runs during the drain must not take the item.
//===========================================================================
// Shape of the run, with nothing left to timing:
//   * fill the queue with kFill embedded integers (no cells of their own, so
//     the only candidates in play are the nodes);
//   * build the canary in a context the test owns, so it is not yet a
//     candidate and no earlier cycle can touch it;
//   * takeAll.  Inside the window the hook prepends a node carrying the
//     canary, hands the producer context's young generation to the space -
//     which is what a real producer does when it ends a turn, and what makes
//     node and canary ordinary candidates - and then asks the collector for a
//     cycle and waits until it has RAISED `stwFlag`.  The pause therefore
//     cannot begin before the window closes (this thread is at
//     criticalSectionDepth > 0 and refuses to park) and cannot begin later
//     than the walk's very first poll, 64 nodes in.  So the candidate set is
//     always fixed with the walk in progress and no `newList` cell yet
//     allocated, on every run.
//   * the canary's finalize says whether that cycle freed it.
//
// The canary is the LAST element of the list takeAll returns, so
// `finalized != 0` means the collector reclaimed a live element of a live
// list: the shipped defect, observed directly rather than inferred from
// corrupted contents, so no reuse of the freed cell is needed to see it.
TEST(MPSCQueueWindow, AWindowPrependSurvivesACollectionDuringTheDrain) {
    constexpr long kFill = 20000;

    CanaryCell::finalized = 0;
    CanaryCell::traced = 0;

    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoRootSet* rs = space.createRootSet("mpsc-window-loss");
    ASSERT_NE(rs, nullptr);

    const ProtoMPSCQueue* q = live.newMPSCQueue();
    const ProtoRootSet::Handle pinned = rs->add(q->asObject(&live));
    ASSERT_NE(pinned, ProtoRootSet::kNullHandle);

    {
        ProtoContext filler(&space, &live, nullptr, nullptr, nullptr, nullptr);
        for (long i = 0; i < kFill; ++i) q->push(&filler, filler.fromInteger(i));
    }

    ProtoContext producer(&space, &live, nullptr, nullptr, nullptr, nullptr);
    const Cell* canary = new (&producer) CanaryCell(&producer);
    ASSERT_NE(canary, nullptr);

    // No heap ceiling anywhere in this test: the pause is requested, not
    // provoked, so the collector runs exactly once and only when asked.
    ASSERT_TRUE(waitForIdleCollector(space, &live))
        << "a collection was still in flight before the drain";

    WindowHookState state;
    state.queue = q;
    state.producer = &producer;
    state.item = reinterpret_cast<const ProtoObject*>(canary);
    state.at = PmqWindowPhase::BeforeDetach;
    state.submitYoung = true;
    state.armCycle = true;

    const uint64_t cyclesBefore = space.getGCCycleCount();
    const ProtoList* batch = nullptr;
    {
        HookInstaller installed(&state);
        batch = q->takeAll(&live);
    }
    const uint64_t cyclesDuringDrain = space.getGCCycleCount() - cyclesBefore;

    ASSERT_TRUE(waitForIdleCollector(space, &live));

    ASSERT_EQ(state.fired.load(), 1) << "the hook never entered the publish window";
    ASSERT_TRUE(state.armed.load()) << "the hook did not reach its arming step";
    ASSERT_TRUE(state.stwSeen.load())
        << "the collector never announced a pause while takeAll held the window, "
           "so the cycle this test needs was not armed";
    ASSERT_EQ(batch->getSize(&live), static_cast<unsigned long>(kFill + 1));
    EXPECT_EQ(batch->getAt(&live, static_cast<int>(kFill)),
              reinterpret_cast<const ProtoObject*>(canary))
        << "the node prepended inside the window must be the last item out";

    std::printf("[ WINDOW   ] drain of %ld items spanned %llu gc cycles; "
                "canary traced=%lu finalized=%lu\n",
                kFill + 1, (unsigned long long) cyclesDuringDrain,
                CanaryCell::traced.load(), CanaryCell::finalized.load());
    std::fflush(stdout);

    // Anti-vacuity: without a cycle inside the call there is nothing to survive.
    ASSERT_GE(cyclesDuringDrain, 1u)
        << "no collection ran while takeAll was in progress";

    EXPECT_EQ(CanaryCell::finalized.load(), 0u)
        << "the collector finalized and freed the item carried by the node that "
           "was prepended inside the publish window, while that item was still "
           "the last element of the list takeAll returned";
    EXPECT_GT(CanaryCell::traced.load(), 0u)
        << "the collector never reached the item through `retained`, so this "
           "run says nothing about whether it would have freed it";

    rs->remove(pinned);
}
