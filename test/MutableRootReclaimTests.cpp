// MutableRootReclaimTests.cpp — release of mutables-tree entries.
//
// A mutable object's state lives in ProtoSpace::mutableRoot, in the shard
// selected by its handle's mutable_ref.  When the handle is collected, its
// finalizer only records the ref; after sweep the collector removes the
// recorded entries, one compare-and-swap per shard, so the state and the
// objects it references become collectable in the following cycle.
//
// These tests check that:
//   * the entries of dropped mutables are removed (both survivor
//     configurations; the objects are dropped before their first cycle);
//   * the objects those states referenced are collected (survivor
//     re-inclusion only: without it a cell that survives one cycle, as the
//     state does through the cycle's mutable snapshot, is never a candidate
//     again);
//   * live mutables keep their state while entries of their shards are
//     released;
//   * writers updating the same shards while releases run lose no update.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

using namespace proto;

namespace {

void requestGcCycle(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    space.gcStarted = true;
    space.gcCV.notify_all();
}

// Runs one collection cycle that starts after the call, cooperating with its
// stop-the-world, and returns when that cycle has finished.
//
// The cycle counter rises at a cycle's stop-the-world, and gcStarted is
// cleared when a cycle ends (after sweep and after the release of
// mutables-tree entries).  A cycle already running at the call clears the
// request when it ends without raising the counter past `before`; the
// request is repeated in that case.
bool runGcCycle(ProtoSpace& space, ProtoContext* ctx) {
    const uint64_t before = space.getGCCycleCount();
    requestGcCycle(space);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (space.gcStarted.load() || space.getGCCycleCount() == before) {
        if (std::chrono::steady_clock::now() > deadline) {
            ADD_FAILURE() << "GC cycle did not finish: stwFlag=" << space.stwFlag.load()
                          << " gcStarted=" << space.gcStarted.load()
                          << " parkedThreads=" << space.parkedThreads.load()
                          << " runningThreads=" << space.runningThreads.load()
                          << " cycles=" << space.getGCCycleCount() << " (before " << before << ")";
            return false;
        }
        if (!space.gcStarted.load() && space.getGCCycleCount() == before) requestGcCycle(space);
        ctx->safepoint();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

unsigned long mutableRefOf(const ProtoObject* object) {
    return toImpl<const ProtoObjectCell>(object)->mutable_ref;
}

bool hasEntry(ProtoSpace& space, ProtoContext* ctx, unsigned long ref) {
    const ProtoSparseList* root =
        space.mutableRoot[ref % ProtoSpace::MUTABLE_ROOT_SHARDS].root.load();
    return sparseListGetRaw(ctx, root, ref) != nullptr;
}

std::size_t countEntries(ProtoSpace& space, ProtoContext* ctx, const std::vector<unsigned long>& refs) {
    std::size_t n = 0;
    for (unsigned long ref : refs) {
        if (hasEntry(space, ctx, ref)) ++n;
    }
    return n;
}

// Payload finalizer: completes an action on an external structure (a
// counter) and nothing else, as the finalizer contract requires.
std::atomic<unsigned long> gPayloadsFinalized{0};
void countPayloadFinalized(void*) {
    gPayloadsFinalized.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

// Mutables written and dropped before the first cycle lose their entries in
// that cycle.  Holds in both survivor configurations.
TEST(MutableRootReclaim, DroppedMutablesReleaseTheirEntries) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const ProtoString* key = ProtoString::createSymbol(ctx, "payload");

    constexpr int kObjects = 4000;
    std::vector<unsigned long> refs;
    refs.reserve(kObjects);
    {
        ProtoContext sub(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kObjects; ++i) {
            const ProtoObject* object = sub.newObject(true);
            object->setAttribute(&sub, key, sub.fromInteger(i));
            refs.push_back(mutableRefOf(object));
        }
    }
    ASSERT_EQ(countEntries(space, ctx, refs), static_cast<std::size_t>(kObjects))
        << "every written mutable must have an entry before collection";

    ASSERT_TRUE(runGcCycle(space, ctx));
    EXPECT_EQ(countEntries(space, ctx, refs), 0u)
        << "the cycle that collects a mutable's handle must release its entry";

    ASSERT_TRUE(runGcCycle(space, ctx));
    EXPECT_EQ(countEntries(space, ctx, refs), 0u);
}

// Once the entries are released, the states and the objects they reference
// are collected on a following cycle.
TEST(MutableRootReclaim, ObjectsReferencedByDroppedMutablesAreCollected) {
#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    GTEST_SKIP() << "requires PROTOCORE_GC_REINCLUDE_SURVIVORS: a state survives the cycle "
                    "that releases its entry, and without re-inclusion a survivor is never "
                    "a candidate again";
#else
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const ProtoString* key = ProtoString::createSymbol(ctx, "payload");
    gPayloadsFinalized = 0;

    constexpr int kObjects = 4000;
    static int token = 0;
    std::vector<unsigned long> refs;
    refs.reserve(kObjects);
    {
        ProtoContext sub(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kObjects; ++i) {
            const ProtoObject* object = sub.newObject(true);
            const ProtoObject* payload = sub.fromExternalPointer(&token, countPayloadFinalized);
            object->setAttribute(&sub, key, payload);
            refs.push_back(mutableRefOf(object));
        }
    }

    // Cycle 1: the handles are collected and their entries released.  The
    // states were reached through this cycle's mutable snapshot, so they and
    // their payloads survive it.
    ASSERT_TRUE(runGcCycle(space, ctx));
    EXPECT_EQ(countEntries(space, ctx, refs), 0u);

    // The writes above filled this thread's mutable value cache with entries
    // naming pre-release shard roots and states.  The caches are not GC roots
    // and this thread cleared them when it resumed from the cycle above, so
    // they keep nothing alive: no step is needed here.

    // Cycle 2 collects the states and payloads; cycle 3 absorbs timing.
    ASSERT_TRUE(runGcCycle(space, ctx));
    ASSERT_TRUE(runGcCycle(space, ctx));
    EXPECT_EQ(gPayloadsFinalized.load(), static_cast<unsigned long>(kObjects))
        << "objects referenced only by dropped mutables must be collected once "
           "their entries are released";
#endif
}

// Live mutables interleaved with dropped ones, so they share shards, keep
// their state across the cycles that release their neighbours' entries, and
// remain writable afterwards.
TEST(MutableRootReclaim, LiveMutablesKeepTheirState) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const ProtoString* key = ProtoString::createSymbol(ctx, "value");
    ProtoRootSet* rs = space.createRootSet("mutable-root-reclaim-live");
    ASSERT_NE(rs, nullptr);

    constexpr int kPairs = 2000;
    std::vector<ProtoRootSet::Handle> live;
    std::vector<unsigned long> droppedRefs;
    live.reserve(kPairs);
    droppedRefs.reserve(kPairs);
    {
        ProtoContext sub(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 2 * kPairs; ++i) {
            const ProtoObject* object = sub.newObject(true);
            object->setAttribute(&sub, key, sub.fromInteger(i));
            if (i % 2 == 0) {
                live.push_back(rs->add(object));
            } else {
                droppedRefs.push_back(mutableRefOf(object));
            }
        }
    }

    for (int cycle = 0; cycle < 3; ++cycle) ASSERT_TRUE(runGcCycle(space, ctx));
    EXPECT_EQ(countEntries(space, ctx, droppedRefs), 0u);

    for (int p = 0; p < kPairs; ++p) {
        const ProtoObject* object = rs->resolve(live[p]);
        ASSERT_NE(object, nullptr);
        const ProtoObject* value = object->getAttribute(ctx, key);
        ASSERT_NE(value, nullptr) << "live mutable " << p << " lost its state";
        EXPECT_EQ(value->asLong(ctx), 2 * p) << "live mutable " << p;
    }

    {
        ProtoContext sub(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (int p = 0; p < kPairs; ++p) {
            rs->resolve(live[p])->setAttribute(&sub, key, sub.fromInteger(100000 + p));
        }
    }
    for (int cycle = 0; cycle < 2; ++cycle) ASSERT_TRUE(runGcCycle(space, ctx));
    for (int p = 0; p < kPairs; ++p) {
        const ProtoObject* value = rs->resolve(live[p])->getAttribute(ctx, key);
        ASSERT_NE(value, nullptr);
        EXPECT_EQ(value->asLong(ctx), 100000 + p) << "live mutable " << p;
    }

    for (ProtoRootSet::Handle h : live) rs->remove(h);
    space.destroyRootSet(rs);
}

namespace {

constexpr int kWriters = 4;
constexpr int kOwnedPerWriter = 32;
constexpr int kDroppedPerIteration = 8;
// Writers run at least kMinWriterIterations and then until the collector
// has completed kCyclesDuringWrites cycles, so the releases of those cycles
// race the writers regardless of machine speed.  kMaxWriterIterations bounds
// the run if cycles do not progress; the cycle assertion then fails.
constexpr int kMinWriterIterations = 500;
constexpr int kMaxWriterIterations = 200000;
constexpr uint64_t kCyclesDuringWrites = 5;

struct WriterShared {
    ProtoRootSet* roots = nullptr;
    const ProtoObject* counter = nullptr;
    const ProtoString* valueKey = nullptr;
    const ProtoString* countKey = nullptr;
    uint64_t stopAtCycle = 0;
    std::atomic<int> done{0};
    std::atomic<bool> release{false};
    std::atomic<unsigned long> errors{0};
    std::atomic<unsigned long> iterations{0};
    std::mutex droppedMutex;
    std::vector<unsigned long> droppedRefs;
};
WriterShared* gWriters = nullptr;

// Each writer owns pinned mutables that it rewrites every iteration, bumps a
// shared counter with setAttributeIfEqual, and drops freshly written mutables
// so that every cycle finalizes handles and releases entries in the shards
// the writers are updating.  When done, it checks its own objects and waits,
// parked, for the release.
const ProtoObject* writerThreadMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                    const ProtoList*, const ProtoSparseList*) {
    WriterShared& shared = *gWriters;
    std::vector<ProtoRootSet::Handle> owned;
    {
        ProtoContext sub(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (int j = 0; j < kOwnedPerWriter; ++j) {
            owned.push_back(shared.roots->add(sub.newObject(true)));
        }
    }
    std::vector<unsigned long> dropped;

    ProtoThread* self = const_cast<ProtoThread*>(ctx->thread);
    int it = 0;
    for (; it < kMaxWriterIterations; ++it) {
        if (it >= kMinWriterIterations && ctx->space->getGCCycleCount() >= shared.stopAtCycle) break;
        {
            ProtoContext sub(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
            for (int j = 0; j < kOwnedPerWriter; ++j) {
                shared.roots->resolve(owned[j])->setAttribute(&sub, shared.valueKey, sub.fromInteger(it));
            }
            for (;;) {
                const ProtoObject* current = shared.counter->getAttribute(&sub, shared.countKey);
                const ProtoObject* next = sub.fromInteger(current->asLong(&sub) + 1);
                if (shared.counter->setAttributeIfEqual(&sub, shared.countKey, current, next)) break;
            }
            for (int k = 0; k < kDroppedPerIteration; ++k) {
                const ProtoObject* object = sub.newObject(true);
                object->setAttribute(&sub, shared.valueKey, sub.fromInteger(it));
                dropped.push_back(mutableRefOf(object));
            }
        }
        // Nearly every allocation above runs inside a critical section, where
        // a thread never parks, so a loop like this must offer a safepoint of
        // its own, as an embedder's dispatch loop does.  synchToGC parks
        // without submitting the young generation.
        self->synchToGC();
    }

    for (int j = 0; j < kOwnedPerWriter; ++j) {
        const ProtoObject* value = shared.roots->resolve(owned[j])->getAttribute(ctx, shared.valueKey);
        if (!value || value->asLong(ctx) != it - 1) {
            shared.errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
    shared.iterations.fetch_add(static_cast<unsigned long>(it));
    {
        std::lock_guard<std::mutex> lock(shared.droppedMutex);
        shared.droppedRefs.insert(shared.droppedRefs.end(), dropped.begin(), dropped.end());
    }
    shared.done.fetch_add(1);
    ProtoContext::UnmanagedScope parked(ctx);
    while (!shared.release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return PROTO_NONE;
}

}  // namespace

// Writers keep updating mutables in every shard while cycles release the
// entries of dropped mutables in those same shards.  The release and the
// writers both publish shard roots with compare-and-swap; no update may be
// lost on either side.
TEST(MutableRootReclaim, ConcurrentWritersLoseNoUpdateDuringRelease) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    WriterShared shared;
    gWriters = &shared;
    shared.roots = space.createRootSet("mutable-root-reclaim-writers");
    shared.valueKey = ProtoString::createSymbol(ctx, "value");
    shared.countKey = ProtoString::createSymbol(ctx, "count");
    const ProtoObject* counter = ctx->newObject(true);
    counter->setAttribute(ctx, shared.countKey, ctx->fromInteger(0));
    const ProtoRootSet::Handle counterHandle = shared.roots->add(counter);
    shared.counter = counter;

    const uint64_t cyclesStart = space.getGCCycleCount();
    shared.stopAtCycle = cyclesStart + kCyclesDuringWrites;

    std::vector<const ProtoThread*> threads;
    for (int t = 0; t < kWriters; ++t) {
        threads.push_back(space.newThread(ctx, ProtoString::createSymbol(ctx, "mutable-root-writer"),
                                          writerThreadMain, nullptr, nullptr));
    }

    {
        // Parked for the stop-the-world quorum while it requests cycles.
        ProtoContext::UnmanagedScope parked(ctx);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
        while (shared.done.load() < kWriters && std::chrono::steady_clock::now() < deadline) {
            requestGcCycle(space);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    const uint64_t cyclesDuringWrites = space.getGCCycleCount() - cyclesStart;
    const int done = shared.done.load();

    // The writers are parked; two more cycles release the entries of the
    // mutables dropped at the end of their runs.
    for (int cycle = 0; cycle < 2; ++cycle) ASSERT_TRUE(runGcCycle(space, ctx));

    std::size_t remaining = 0;
    std::size_t droppedCount = 0;
    {
        std::lock_guard<std::mutex> lock(shared.droppedMutex);
        remaining = countEntries(space, ctx, shared.droppedRefs);
        droppedCount = shared.droppedRefs.size();
    }
    const ProtoObject* finalCount = shared.roots->resolve(counterHandle)->getAttribute(ctx, shared.countKey);
    const unsigned long iterations = shared.iterations.load();

    shared.release = true;
    {
        ProtoContext::UnmanagedScope parked(ctx);
        for (const ProtoThread* thread : threads) const_cast<ProtoThread*>(thread)->join(ctx);
    }

    ASSERT_EQ(done, kWriters) << "writer threads did not finish";
    EXPECT_GE(cyclesDuringWrites, 3u) << "too few cycles ran while the writers were active";
    EXPECT_EQ(shared.errors.load(), 0u) << "a writer's last update to its own mutable was lost";
    ASSERT_NE(finalCount, nullptr);
    EXPECT_EQ(static_cast<unsigned long>(finalCount->asLong(ctx)), iterations)
        << "a compare-and-swap increment of the shared counter was lost";
    EXPECT_EQ(droppedCount, static_cast<std::size_t>(iterations) * kDroppedPerIteration);
    EXPECT_EQ(remaining, 0u) << "entries of dropped mutables were not released";

    space.destroyRootSet(shared.roots);
    gWriters = nullptr;
}
