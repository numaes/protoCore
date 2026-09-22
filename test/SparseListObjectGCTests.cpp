// SparseListObjectGCTests.cpp — keys referenced only by a
// ProtoSparseListObject must survive collection cycles, both forms.
// Cycles are forced with a small hard heap limit (ProtoSpace::setHeapLimits),
// the pattern of test/GCRootScopeTests.cpp.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace proto;

namespace {

constexpr int kHeadroomCells = 40000;
constexpr int kGarbagePerBatch = 5000;

uint64_t forceCycles(ProtoSpace& space, ProtoContext* parent, uint64_t minCycles) {
    const uint64_t start = space.getGCCycleCount();
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + kHeadroomCells);
    for (int batch = 0; batch < 400 && space.getGCCycleCount() - start < minCycles; ++batch) {
        ProtoContext garbage(&space, parent, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kGarbagePerBatch; ++i) (void) garbage.newObject(false);
    }
    space.setHeapLimits(0, 0);
    return space.getGCCycleCount() - start;
}

bool waitForIdleCollector(ProtoSpace& space, ProtoContext* ctx) {
    ProtoContext::UnmanagedScope parked(ctx);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (space.gcStarted.load()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// A key cell that counts every traversal by the collector.
class KeyProbeCell final : public Cell {
public:
    static std::atomic<unsigned long> traversals;
    explicit KeyProbeCell(ProtoContext* context) : Cell(context) {}
    void processReferences(ProtoContext*, void*, void (*)(ProtoContext*, void*, const Cell*)) const override {
        traversals.fetch_add(1, std::memory_order_relaxed);
    }
    const ProtoObject* implAsObject(ProtoContext*) const override { return PROTO_NONE; }
};
std::atomic<unsigned long> KeyProbeCell::traversals{0};

struct ContentCheck {
    const ProtoSparseListObject* map;
    int n;
    int visited;
    int bad;
    std::vector<bool> seen;
};

void checkPair(ProtoContext* c, void* self, const ProtoObject* key, const ProtoObject* value) {
    auto* chk = static_cast<ContentCheck*>(self);
    chk->visited++;
    const ProtoList* list = key->asList(c);
    if (!list || list->getSize(c) != 1) { chk->bad++; return; }
    const long i = list->getAt(c, 0)->asLong(c);
    if (i < 0 || i >= chk->n || chk->seen[i] || value->asLong(c) != i * 10 ||
        chk->map->getAt(c, key) != value) { chk->bad++; return; }
    chk->seen[i] = true;
}

}  // namespace

class SparseListObjectGC : public ::testing::TestWithParam<int> {};

// The key is referenced only by the collection (the map is pinned by a root
// set; the probe's allocating context has exited).  The collector must reach
// the probe through the map's processReferences.
TEST_P(SparseListObjectGC, KeyReferencedOnlyByTheCollectionIsTraced) {
    const int n = GetParam();
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoRootSet* rs = space.createRootSet("pslo-key-probe");
    ASSERT_NE(rs, nullptr);
    ProtoRootSet::Handle pinned = ProtoRootSet::kNullHandle;
    {
        ProtoContext sub(&space, &live, nullptr, nullptr, nullptr, nullptr);
        const ProtoSparseListObject* m = sub.newSparseListObject();
        for (int i = 0; i < n; ++i) {
            const ProtoObject* probe = reinterpret_cast<const ProtoObject*>(new (&sub) KeyProbeCell(&sub));
            m = m->setAt(&sub, probe, sub.fromInteger(i));
        }
        pinned = rs->add(m->asObject(&sub));
    }  // the probes are now reachable only as keys of the pinned map
    KeyProbeCell::traversals = 0;

    const uint64_t cycles = forceCycles(space, &live, 5);
    ASSERT_TRUE(waitForIdleCollector(space, &live));
    EXPECT_GE(cycles, 5u);
    EXPECT_GE(KeyProbeCell::traversals.load(), static_cast<unsigned long>(n))
        << "a key referenced only by the collection was not traced";

    const ProtoSparseListObject* m = rs->resolve(pinned)->asSparseListObject(&live);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->getSize(&live), static_cast<unsigned long>(n));
    rs->remove(pinned);
    space.destroyRootSet(rs);
}

// Real objects as keys, referenced only by the collection: after forced
// cycles every key is recovered from the map and its contents are intact.
TEST_P(SparseListObjectGC, KeysAndContentsSurviveForcedCollections) {
    const int n = GetParam();
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoRootSet* rs = space.createRootSet("pslo-contents");
    ASSERT_NE(rs, nullptr);
    ProtoRootSet::Handle pinned = ProtoRootSet::kNullHandle;
    {
        ProtoContext sub(&space, &live, nullptr, nullptr, nullptr, nullptr);
        const ProtoSparseListObject* m = sub.newSparseListObject();
        for (int i = 0; i < n; ++i) {
            const ProtoObject* key = sub.newList()->appendLast(&sub, sub.fromInteger(i))->asObject(&sub);
            m = m->setAt(&sub, key, sub.fromInteger(i * 10));
        }
        pinned = rs->add(m->asObject(&sub));
    }

    const uint64_t cycles = forceCycles(space, &live, 5);
    ASSERT_TRUE(waitForIdleCollector(space, &live));
    EXPECT_GE(cycles, 5u);

    const ProtoSparseListObject* m = rs->resolve(pinned)->asSparseListObject(&live);
    ASSERT_NE(m, nullptr);
    ContentCheck chk{m, n, 0, 0, std::vector<bool>(n, false)};
    m->processElements(&live, &chk, checkPair);
    EXPECT_EQ(chk.visited, n);
    EXPECT_EQ(chk.bad, 0);
    rs->remove(pinned);
    space.destroyRootSet(rs);
}

// 1-3 keys: Small form; 4 is the promotion boundary; 64 and 2000: AVL.
INSTANTIATE_TEST_SUITE_P(BothForms, SparseListObjectGC, ::testing::Values(1, 3, 4, 64, 2000));

namespace {

constexpr int kConcBase = 32;
constexpr int kConcThreads = 4;
constexpr int kConcPerThread = 3000;

std::atomic<unsigned long> gConcErrors{0};
const ProtoSparseListObject* gConcBase = nullptr;
std::vector<const ProtoObject*>* gConcBaseKeys = nullptr;

// Each worker derives its own version chain from the shared base. After
// every write it checks its own key round-trips and, every 64 iterations,
// that the shared base is still exactly the original kConcBase pairs and
// that its own version still carries the corresponding base pair.
//
// Run through ProtoSpace::newThread (never a raw std::thread) so this
// context is a real registered ProtoThread: it is added to space->threads,
// which the stop-the-world root scan walks independently of
// ProtoSpace::mainContext, protecting the List and ProtoSparseListObject
// cells this function allocates directly on `ctx` via the same young-chain
// mechanism proven by allocatingThreadMain in GCRootScopeTests.cpp. A raw
// std::thread building a bare ProtoContext(&space) here (as
// ConcurrentMarkSafetyTests.cpp does) would not be registered in
// space->threads; it would instead race other such threads on
// ProtoSpace::mainContext, which is safe there only because that test's
// payload is mutable objects and tagged SmallIntegers -- neither needs
// context-based rooting. This test allocates real immutable cells, so it
// needs the real per-thread registration.
const ProtoObject* concWorkerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                   const ProtoList* args, const ProtoSparseList*) {
    const long t = args->getAt(ctx, 0)->asLong(ctx);
    const ProtoSparseListObject* mine = gConcBase;
    for (int i = 0; i < kConcPerThread; ++i) {
        const ProtoObject* key = ctx->newList()
            ->appendLast(ctx, ctx->fromInteger(t * 1000000L + i))->asObject(ctx);
        mine = mine->setAt(ctx, key, ctx->fromInteger(i));
        if (mine->getAt(ctx, key) != ctx->fromInteger(i)) gConcErrors.fetch_add(1);
        if (i % 64 == 0) {
            const int b = i % kConcBase;
            const ProtoObject* bv = gConcBase->getAt(ctx, (*gConcBaseKeys)[b]);
            if (gConcBase->getSize(ctx) != static_cast<unsigned long>(kConcBase) ||
                !bv || bv->asLong(ctx) != b) {
                gConcErrors.fetch_add(1);
            }
            const ProtoObject* mv = mine->getAt(ctx, (*gConcBaseKeys)[b]);
            if (!mv || mv->asLong(ctx) != b) gConcErrors.fetch_add(1);
        }
    }
    if (mine->getSize(ctx) != static_cast<unsigned long>(kConcBase + kConcPerThread)) {
        gConcErrors.fetch_add(1);
    }
    return PROTO_NONE;
}

}  // namespace

// Several threads derive independent versions from one shared base while a
// kicker thread keeps requesting collections. The base must stay unchanged
// and every thread's version must hold exactly base + its own keys.
TEST(SparseListObjectConcurrency, VersionsFromASharedBaseWhileTheGcRuns) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;

    std::vector<const ProtoObject*> baseKeys;
    const ProtoSparseListObject* base = root->newSparseListObject();
    for (int i = 0; i < kConcBase; ++i) {
        const ProtoObject* key = root->newList()->appendLast(root, root->fromInteger(-1 - i))->asObject(root);
        baseKeys.push_back(key);
        base = base->setAt(root, key, root->fromInteger(i));
    }

    gConcErrors = 0;
    gConcBase = base;
    gConcBaseKeys = &baseKeys;

    std::atomic<bool> stopGc{false};
    std::thread gcKicker([&]() {
        while (!stopGc.load(std::memory_order_relaxed)) {
            space.triggerGC();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    std::vector<const ProtoThread*> workers;
    for (int t = 0; t < kConcThreads; ++t) {
        const ProtoList* args = root->newList()->appendLast(root, root->fromInteger(t));
        workers.push_back(space.newThread(root, ProtoString::createSymbol(root, "pslo-concurrency-worker"),
                                           concWorkerMain, args, nullptr));
    }
    {
        ProtoContext::UnmanagedScope parked(root);
        for (const ProtoThread* w : workers) const_cast<ProtoThread*>(w)->join(root);
    }

    stopGc.store(true, std::memory_order_relaxed);
    gcKicker.join();

    EXPECT_EQ(gConcErrors.load(), 0u);
    EXPECT_EQ(base->getSize(root), static_cast<unsigned long>(kConcBase));
    for (int i = 0; i < kConcBase; ++i) EXPECT_EQ(base->getAt(root, baseKeys[i])->asLong(root), i);

    gConcBase = nullptr;
    gConcBaseKeys = nullptr;
}
