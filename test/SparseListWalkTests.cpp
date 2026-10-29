// SparseListWalkTests.cpp -- what ProtoSparseList::processElements and
// processValues guarantee while their callback runs.
//
// Up to 3.3.1 the AVL form was walked with an iterator chain of Cells held
// only in a C++ local, so the whole walk -- every callback included -- ran
// inside one GC critical section.  A thread inside a critical section never
// parks: no stop-the-world can begin until the walk ends.  A callback that
// waited for a collection, or for another thread that needed one, waited
// for the whole walk or forever; a callback that allocated under a heap
// ceiling could not wait for headroom either, so the heap overshot the
// ceiling by one batch per refill.
//
// The walk now allocates nothing, holds no critical section across the
// callback, and pins its receiver in a root set of the space for its duration
// -- the walking thread's own walk-pin shard (WalkPin and
// ProtoSpaceImplementation::walkPins, headers/proto_internal.h), so that
// concurrent walkers do not serialise on one lock: the critical section used to keep the
// receiver alive implicitly, and callers rely on that -- the tree that
// getOwnAttributes() answers for a MUTABLE object is reachable, once a write
// supersedes the state, from nothing but the caller's C++ local.
//
// Every test below that names a failure mode fails against 3.3.1 / master
// 43fae387 (critical section) and the receiver tests also fail with the pin
// removed (both variants were built and run against this file).  Waits are
// bounded: a regression reports a failed expectation, it does not hang.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace proto;

namespace {

void requestCycle(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    space.gcStarted = true;
    space.gcCV.notify_all();
}

// Asks for cycles and parks `ctx` at safepoints until `count` cycles have
// completed or `budget` has elapsed.  Returns the cycles that completed.
uint64_t runCycles(ProtoSpace& space, ProtoContext* ctx, uint64_t count,
                   std::chrono::milliseconds budget = std::chrono::milliseconds(5000)) {
    const uint64_t start = space.getGCCycleCount();
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (space.getGCCycleCount() - start < count &&
           std::chrono::steady_clock::now() < deadline) {
        requestCycle(space);
        {
            ProtoContext garbage(&space, ctx, nullptr, nullptr, nullptr, nullptr);
            for (int i = 0; i < 2000; ++i) (void) garbage.newObject(false);
        }
        ctx->safepoint();
        ProtoThread::sleepFor(ctx, 500);
    }
    return space.getGCCycleCount() - start;
}

// key i -> i + 1 (0 is the small form's empty-slot sentinel), value = an
// object carrying i in attribute "idx".
const ProtoSparseList* buildIndexedList(ProtoContext* ctx, const ProtoString* idx, int n) {
    const ProtoSparseList* list = ctx->newSparseList();
    for (int i = 0; i < n; ++i) {
        const ProtoObject* v = ctx->newObject(false)->setAttribute(ctx, idx, ctx->fromInteger(i));
        list = list->setAt(ctx, static_cast<proto_ulong>(i + 1), v);
    }
    return list;
}

struct DepthRecord {
    int calls = 0;
    unsigned maxDepth = 0;
};

}  // namespace

// White-box statement of the contract: the callback runs at the caller's
// critical-section depth (0 here), for both forms and both entry points.
TEST(SparseListWalk, CallbackRunsOutsideAnyCriticalSection) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    const ProtoString* idx = ProtoString::createSymbol(&ctx, "idx");
    for (int n : {3, 64, 1000}) {
        const ProtoSparseList* list = buildIndexedList(&ctx, idx, n);
        ctx.getAutomaticLocals()[0] = list->asObject(&ctx);
        DepthRecord elements, values;
        list->processElements(&ctx, &elements,
            [](ProtoContext* c, void* self, proto_ulong, const ProtoObject*) {
                auto* r = static_cast<DepthRecord*>(self);
                ++r->calls;
                r->maxDepth = std::max(r->maxDepth, c->criticalSectionDepth);
            });
        list->processValues(&ctx, &values,
            [](ProtoContext* c, void* self, const ProtoObject*) {
                auto* r = static_cast<DepthRecord*>(self);
                ++r->calls;
                r->maxDepth = std::max(r->maxDepth, c->criticalSectionDepth);
            });
        EXPECT_EQ(elements.calls, n);
        EXPECT_EQ(values.calls, n);
        EXPECT_EQ(elements.maxDepth, 0u) << "processElements ran its callback inside a critical section, n=" << n;
        EXPECT_EQ(values.maxDepth, 0u) << "processValues ran its callback inside a critical section, n=" << n;
    }
}

// Same order and same pairs as the public iterator, for both forms; and
// processValues reports the same values.
TEST(SparseListWalk, VisitsEveryEntryInIteratorOrder) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    for (int n : {0, 1, 3, 4, 100, 5000}) {
        const ProtoSparseList* list = ctx.newSparseList();
        // Scrambled insertion order, so the tree is not built in key order.
        for (int i = 0; i < n; ++i) {
            const proto_ulong key = static_cast<proto_ulong>((i * 7919) % 100003 + 1);
            list = list->setAt(&ctx, key, ctx.fromInteger(i));
        }
        ctx.getAutomaticLocals()[0] = list->asObject(&ctx);

        std::vector<std::pair<proto_ulong, const ProtoObject*>> expected;
        for (auto* it = list->getIterator(&ctx); it && it->hasNext(&ctx);
             it = const_cast<ProtoSparseListIterator*>(it)->advance(&ctx))
            expected.emplace_back(it->nextKey(&ctx), it->nextValue(&ctx));

        std::vector<std::pair<proto_ulong, const ProtoObject*>> got;
        list->processElements(&ctx, &got, [](ProtoContext*, void* self, proto_ulong k, const ProtoObject* v) {
            static_cast<std::vector<std::pair<proto_ulong, const ProtoObject*>>*>(self)->emplace_back(k, v);
        });
        std::vector<const ProtoObject*> values;
        list->processValues(&ctx, &values, [](ProtoContext*, void* self, const ProtoObject* v) {
            static_cast<std::vector<const ProtoObject*>*>(self)->push_back(v);
        });

        ASSERT_EQ(got.size(), static_cast<size_t>(n));
        EXPECT_EQ(got, expected) << "n=" << n;
        ASSERT_EQ(values.size(), got.size());
        for (size_t i = 0; i < got.size(); ++i) EXPECT_EQ(values[i], got[i].second);
        EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
    }
}

namespace {

// A ProtoThread that, once told to, needs collections to complete before it
// can report success.
struct Helper {
    ProtoSpace* space = nullptr;
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
    std::atomic<uint64_t> cyclesSeen{0};
};
Helper* g_helper = nullptr;

const ProtoObject* helperMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoArguments*, const ProtoKeywordArguments*) {
    Helper* h = g_helper;
    const auto goDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!h->go.load() && std::chrono::steady_clock::now() < goDeadline)
        ProtoThread::sleepFor(ctx, 1000);
    // Wait (off the quorum, at safepoints) for two complete cycles.
    const uint64_t start = h->space->getGCCycleCount();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (h->space->getGCCycleCount() - start < 2 && std::chrono::steady_clock::now() < deadline) {
        requestCycle(*h->space);
        ctx->safepoint();
        ProtoThread::sleepFor(ctx, 500);
    }
    h->cyclesSeen.store(h->space->getGCCycleCount() - start);
    h->done.store(h->space->getGCCycleCount() - start >= 2);
    return PROTO_NONE;
}

struct WaitRecord {
    Helper* helper = nullptr;
    int calls = 0;
    bool helperFinishedInsideTheCallback = false;
    double waitedSeconds = 0;
};

}  // namespace

// The callback hands work to another ProtoThread and waits for it, and that
// work needs collections.  Inside a critical section the walking thread holds
// the stop-the-world quorum even while it sleeps, so no cycle can start and
// the helper cannot finish: the bounded wait times out (and an unbounded one
// would be a deadlock).  Outside one, the helper finishes within milliseconds.
TEST(SparseListWalk, CallbackCanWaitForAThreadThatNeedsACollection) {
    ProtoSpace space;
    ProtoContext caller(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    const ProtoString* idx = ProtoString::createSymbol(&caller, "idx");
    const ProtoSparseList* list = buildIndexedList(&caller, idx, 64);   // AVL form
    caller.getAutomaticLocals()[0] = list->asObject(&caller);

    Helper helper;
    helper.space = &space;
    g_helper = &helper;
    const ProtoThread* thread = space.newThread(&caller, ProtoString::createSymbol(&caller, "walk-helper"),
                                                helperMain, nullptr, nullptr);
    WaitRecord record;
    record.helper = &helper;
    list->processElements(&caller, &record,
        [](ProtoContext* ctx, void* self, proto_ulong, const ProtoObject*) {
            auto* r = static_cast<WaitRecord*>(self);
            if (r->calls++ != 0) return;
            r->helper->go.store(true);
            const auto t0 = std::chrono::steady_clock::now();
            const auto deadline = t0 + std::chrono::seconds(8);
            while (!r->helper->done.load() && std::chrono::steady_clock::now() < deadline)
                ProtoThread::sleepFor(ctx, 1000);
            r->helperFinishedInsideTheCallback = r->helper->done.load();
            r->waitedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        });
    const_cast<ProtoThread*>(thread)->join(&caller);
    g_helper = nullptr;

    std::printf("[ WaitForHelper ] calls=%d finishedInside=%d waited=%.3fs helperCycles=%llu\n",
                record.calls, record.helperFinishedInsideTheCallback ? 1 : 0, record.waitedSeconds,
                static_cast<unsigned long long>(helper.cyclesSeen.load()));
    EXPECT_EQ(record.calls, 64);
    EXPECT_TRUE(record.helperFinishedInsideTheCallback)
        << "no collection could run while the callback waited: the walk holds the quorum";
    EXPECT_TRUE(helper.done.load());
}

namespace {

struct CeilingRecord {
    ProtoSpace* space = nullptr;
    const ProtoString* idx = nullptr;
    int calls = 0;
    int wrong = 0;
    int peakHeap = 0;
};

}  // namespace

// The callback allocates heavily on the walking context under a hard heap
// ceiling and hands its garbage over at safepoints.  The ceiling must hold:
// inside a critical section an allocation at the ceiling cannot wait, so each
// refill overshoots by one batch and the heap grows with the garbage.  Every
// value must still read back: collections run between callbacks.
TEST(SparseListWalk, AllocatingCallbackRespectsTheHeapCeiling) {
    ProtoSpace space;
    const unsigned savedThreshold = space.maxAllocatedCellsPerContext;
    space.maxAllocatedCellsPerContext = 500;
    ProtoContext caller(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    const ProtoString* idx = ProtoString::createSymbol(&caller, "idx");
    const ProtoSparseList* list = buildIndexedList(&caller, idx, 512);
    caller.getAutomaticLocals()[0] = list->asObject(&caller);
    caller.safepoint();

    const int hard = space.heapSize + 400000;
    space.setHeapLimits(0, hard);
    const uint64_t cyclesBefore = space.getGCCycleCount();

    CeilingRecord record;
    record.space = &space;
    record.idx = idx;
    list->processElements(&caller, &record,
        [](ProtoContext* ctx, void* self, proto_ulong key, const ProtoObject* value) {
            auto* r = static_cast<CeilingRecord*>(self);
            ++r->calls;
            // ~4,000 garbage cells per entry, ~2 million in all: five times
            // the headroom, so the walk cannot finish without collections.
            for (int i = 0; i < 4000; ++i) (void) ctx->newObject(false);
            ctx->safepoint();
            r->peakHeap = std::max(r->peakHeap, r->space->heapSize);
            const ProtoObject* stored = value ? value->getAttribute(ctx, r->idx) : nullptr;
            if (!stored || !stored->isInteger(ctx) || stored->asLong(ctx) != static_cast<long long>(key - 1))
                ++r->wrong;
        });
    const uint64_t cycles = space.getGCCycleCount() - cyclesBefore;
    space.setHeapLimits(0, 0);
    space.maxAllocatedCellsPerContext = savedThreshold;

    std::printf("[ HeapCeiling ] calls=%d wrong=%d cycles=%llu hard=%d peak=%d (%+d)\n",
                record.calls, record.wrong, static_cast<unsigned long long>(cycles), hard,
                record.peakHeap, record.peakHeap - hard);
    EXPECT_EQ(record.calls, 512);
    EXPECT_EQ(record.wrong, 0);
    EXPECT_GE(cycles, 2u) << "no collection ran during the walk";
    EXPECT_LE(record.peakHeap, hard) << "the heap crossed its hard ceiling during the walk";
}

namespace {

std::atomic<int> g_probesFreed{0};
void countFree(void*) { g_probesFreed.fetch_add(1, std::memory_order_relaxed); }

struct ProbeRecord {
    ProtoSpace* space = nullptr;
    ProtoObject* obj = nullptr;
    const std::vector<const ProtoString*>* keys = nullptr;
    const std::vector<const ProtoObject*>* probes = nullptr;
    int calls = 0;
    int wrong = 0;
    int freedDuringWalk = 0;
    uint64_t cycles = 0;
};

// The receiver of the walk is the attribute tree of a mutable object's
// state, held only in a C++ local, as runtimes do with getOwnAttributes().
// The callback supersedes that state and runs collections.  The walk must
// keep the tree (and so its probe values) alive; afterwards later cycles
// must free the probes, which proves they were unreachable and the pin was
// released.
void runSupersededReceiverWalk(int attributes) {
    ProtoSpace space;
    const unsigned savedThreshold = space.maxAllocatedCellsPerContext;
    space.maxAllocatedCellsPerContext = 500;
    auto* caller = new ProtoContext(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    std::vector<const ProtoString*> keys;
    std::vector<const ProtoObject*> probes;
    for (int i = 0; i < attributes; ++i)
        keys.push_back(ProtoString::createSymbol(caller, ("sl_walk_probe_" + std::to_string(i)).c_str()));
    ProtoObject* obj = nullptr;
    {
        ProtoContext builder(&space, caller, nullptr, nullptr, nullptr, nullptr);
        obj = const_cast<ProtoObject*>(builder.newObject(true));
        for (int i = 0; i < attributes; ++i) {
            const ProtoObject* probe = builder.fromExternalPointer(nullptr, &countFree);
            probes.push_back(probe);
            obj->setAttribute(&builder, keys[i], probe);
        }
        caller->getAutomaticLocals()[0] = obj;
    }
    g_probesFreed.store(0);

    const ProtoSparseList* attrs = obj->getOwnAttributes(caller);   // C++ local only
    ProbeRecord record;
    record.space = &space;
    record.obj = obj;
    record.keys = &keys;
    record.probes = &probes;
    attrs->processElements(caller, &record,
        [](ProtoContext* ctx, void* self, proto_ulong key, const ProtoObject* value) {
            auto* r = static_cast<ProbeRecord*>(self);
            ++r->calls;
            bool known = false;
            for (size_t i = 0; i < r->keys->size(); ++i)
                if (reinterpret_cast<proto_ulong>((*r->keys)[i]) == key) {
                    known = true;
                    if (value != (*r->probes)[i]) ++r->wrong;   // pointer compare only
                }
            if (!known) ++r->wrong;
            if (r->calls == 1) {
                for (size_t i = 0; i < r->keys->size(); ++i)
                    r->obj->setAttribute(ctx, (*r->keys)[i], ctx->fromInteger(-1 - static_cast<int>(i)));
                for (int i = 0; i < 1000; ++i) (void) ctx->newObject(false);
                ctx->safepoint();
                r->cycles += runCycles(*r->space, ctx, 3);
            } else if ((r->calls & 7) == 0) {
                r->cycles += runCycles(*r->space, ctx, 1);
            }
        });
    record.freedDuringWalk = g_probesFreed.load();
    runCycles(space, caller, 4);
    const int freedAfter = g_probesFreed.load();
    space.maxAllocatedCellsPerContext = savedThreshold;
    delete caller;

    std::printf("[ SupersededReceiver n=%d ] calls=%d wrong=%d freedDuringWalk=%d cycles=%llu freedAfter=%d\n",
                attributes, record.calls, record.wrong, record.freedDuringWalk,
                static_cast<unsigned long long>(record.cycles), freedAfter);
    EXPECT_EQ(record.calls, attributes);
    EXPECT_EQ(record.wrong, 0);
    EXPECT_GE(record.cycles, 3u) << "no collection ran during the walk; the test proved nothing";
    EXPECT_EQ(record.freedDuringWalk, 0) << "the walked tree was freed while the walk stood on it";
    EXPECT_EQ(freedAfter, attributes) << "probes not freed after the walk (unreachable? pin released?)";
}

}  // namespace

// An object's attribute tree is always in the AVL form, even with few entries.
TEST(SparseListWalk, SupersededReceiverSurvivesCollections) { runSupersededReceiverWalk(64); }
TEST(SparseListWalk, SupersededReceiverWithFewEntriesSurvivesCollections) { runSupersededReceiverWalk(3); }

namespace {

struct GarbageRecord {
    ProtoSpace* space = nullptr;
    const std::vector<const ProtoObject*>* probes = nullptr;
    int calls = 0;
    int wrong = 0;
    uint64_t cycles = 0;
};

// A list built in a context that is then destroyed, held only in a C++
// local: garbage as far as the collector can see, except for the walk.
void runGarbageReceiverWalk(int entries) {
    ProtoSpace space;
    ProtoContext caller(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    std::vector<const ProtoObject*> probes;
    const ProtoSparseList* list = nullptr;
    {
        ProtoContext builder(&space, &caller, nullptr, nullptr, nullptr, nullptr);
        list = builder.newSparseList();
        for (int i = 0; i < entries; ++i) {
            const ProtoObject* probe = builder.fromExternalPointer(nullptr, &countFree);
            probes.push_back(probe);
            list = list->setAt(&builder, static_cast<proto_ulong>(i + 1), probe);
        }
    }
    g_probesFreed.store(0);
    GarbageRecord record;
    record.space = &space;
    record.probes = &probes;
    list->processElements(&caller, &record,
        [](ProtoContext* ctx, void* self, proto_ulong key, const ProtoObject* value) {
            auto* r = static_cast<GarbageRecord*>(self);
            ++r->calls;
            if (key < 1 || key > r->probes->size() || value != (*r->probes)[key - 1]) ++r->wrong;
            r->cycles += runCycles(*r->space, ctx, 2);
        });
    const int freedDuringWalk = g_probesFreed.load();
    runCycles(space, &caller, 4);
    const int freedAfter = g_probesFreed.load();
    std::printf("[ GarbageReceiver n=%d ] calls=%d wrong=%d freedDuringWalk=%d cycles=%llu freedAfter=%d\n",
                entries, record.calls, record.wrong, freedDuringWalk,
                static_cast<unsigned long long>(record.cycles), freedAfter);
    EXPECT_EQ(record.calls, entries);
    EXPECT_EQ(record.wrong, 0);
    EXPECT_GE(record.cycles, 2u * entries) << "collections did not run during the walk";
    EXPECT_EQ(freedDuringWalk, 0) << "the walked list was freed while the walk stood on it";
    EXPECT_EQ(freedAfter, entries) << "probes not freed after the walk (unreachable? pin released?)";
}

}  // namespace

// The small inline form (at most 3 entries) never held a critical section,
// so before the pin its receiver was unprotected even on 3.3.1.
TEST(SparseListWalk, GarbageReceiverSurvivesCollectionsSmallForm) { runGarbageReceiverWalk(3); }
TEST(SparseListWalk, GarbageReceiverSurvivesCollectionsAvlForm) { runGarbageReceiverWalk(40); }

// A callback that throws: the exception reaches the caller and the pin is
// released.  Nested walks hold one pin each and release both.
TEST(SparseListWalk, PinsNestAndAreReleasedOnException) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    struct AllPins {
        const ProtoSpaceImplementation* impl;
        proto_ulong size() const {
            proto_ulong n = 0;
            for (auto* shard : impl->walkPins) n += shard->size();
            return n;
        }
    } allPins{space.impl};
    AllPins* roots = &allPins;
    const proto_ulong base = roots->size();
    const ProtoString* idx = ProtoString::createSymbol(&ctx, "idx");
    const ProtoSparseList* outer = buildIndexedList(&ctx, idx, 50);
    const ProtoSparseList* inner = buildIndexedList(&ctx, idx, 2);
    ctx.getAutomaticLocals()[0] = outer->asObject(&ctx);

    struct Nest { const ProtoSparseList* inner; AllPins* roots; proto_ulong maxPins; int innerCalls; } nest{inner, roots, 0, 0};
    outer->processElements(&ctx, &nest, [](ProtoContext* c, void* self, proto_ulong, const ProtoObject*) {
        auto* n = static_cast<Nest*>(self);
        n->inner->processValues(c, n, [](ProtoContext*, void* s, const ProtoObject*) {
            auto* m = static_cast<Nest*>(s);
            ++m->innerCalls;
            m->maxPins = std::max(m->maxPins, m->roots->size());
        });
    });
    EXPECT_EQ(nest.innerCalls, 100);
    EXPECT_EQ(nest.maxPins, base + 2) << "each walk holds exactly one pin while it runs";
    EXPECT_EQ(roots->size(), base);

    bool caught = false;
    try {
        outer->processElements(&ctx, nullptr, [](ProtoContext*, void*, proto_ulong k, const ProtoObject*) {
            if (k == 10) throw std::runtime_error("from the callback");
        });
    } catch (const std::runtime_error&) {
        caught = true;
    }
    EXPECT_TRUE(caught);
    EXPECT_EQ(roots->size(), base) << "the pin leaked when the callback threw";
    EXPECT_EQ(ctx.criticalSectionDepth, 0u);
}

namespace {

proto_ulong totalWalkPins(const ProtoSpaceImplementation* impl) {
    proto_ulong n = 0;
    for (auto* shard : impl->walkPins) n += shard->size();
    return n;
}

// More walking threads than walk-pin shards, so some threads share a shard.
constexpr int kConcurrentWalkers = static_cast<int>(ProtoSpaceImplementation::kWalkPinShards) + 4;
constexpr int kEntriesPerWalker = 24;   // AVL form

struct ConcurrentWalk {
    ProtoSpace* space = nullptr;
    std::atomic<int> inside{0};      // walkers parked inside their first callback
    std::atomic<bool> release{false};
    std::atomic<int> calls{0};
    std::atomic<int> wrong{0};
    std::atomic<int> timedOut{0};
};
ConcurrentWalk* g_concurrent = nullptr;

struct WalkerState {
    ConcurrentWalk* shared = nullptr;
    std::vector<const ProtoObject*> probes;
    int calls = 0;
};

// Builds a list of probes in a context it then destroys -- garbage as far as
// the collector can see -- and walks it, holding only a C++ local.  The first
// callback waits (parked, off the quorum) until the main thread has seen
// every walker inside and has run collections.
const ProtoObject* concurrentWalkerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                        const ProtoArguments*, const ProtoKeywordArguments*) {
    ConcurrentWalk* shared = g_concurrent;
    WalkerState state;
    state.shared = shared;
    const ProtoSparseList* list = nullptr;
    {
        ProtoContext builder(shared->space, ctx, nullptr, nullptr, nullptr, nullptr);
        list = builder.newSparseList();
        for (int i = 0; i < kEntriesPerWalker; ++i) {
            const ProtoObject* probe = builder.fromExternalPointer(nullptr, &countFree);
            state.probes.push_back(probe);
            list = list->setAt(&builder, static_cast<proto_ulong>(i + 1), probe);
        }
    }
    list->processElements(ctx, &state,
        [](ProtoContext* c, void* self, proto_ulong key, const ProtoObject* value) {
            auto* s = static_cast<WalkerState*>(self);
            ++s->calls;
            s->shared->calls.fetch_add(1);
            if (key < 1 || key > s->probes.size() || value != s->probes[key - 1]) s->shared->wrong.fetch_add(1);
            if (s->calls == 1) {
                s->shared->inside.fetch_add(1);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (!s->shared->release.load() && std::chrono::steady_clock::now() < deadline)
                    ProtoThread::sleepFor(c, 500);
                if (!s->shared->release.load()) s->shared->timedOut.fetch_add(1);
            }
            for (int i = 0; i < 200; ++i) (void) c->newObject(false);
            c->safepoint();
        });
    return PROTO_NONE;
}

}  // namespace

// Twenty ProtoThreads walk at once, each standing on a receiver that only its
// pin keeps alive, while the main thread runs collections.  Every pin lands in
// the walking thread's own shard: with all walkers inside their callbacks, the
// pins are spread over every shard and no shard holds more than the
// round-robin share (two threads per shard beyond the sixteenth).  No probe is
// freed during the walks; every probe is freed after them; every pin is
// released.
TEST(SparseListWalk, ConcurrentWalkersArePinnedInTheirOwnShards) {
    ProtoSpace space;
    ProtoContext caller(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    const proto_ulong base = totalWalkPins(space.impl);
    g_probesFreed.store(0);

    ConcurrentWalk shared;
    shared.space = &space;
    g_concurrent = &shared;
    std::vector<const ProtoThread*> threads;
    for (int t = 0; t < kConcurrentWalkers; ++t)
        threads.push_back(space.newThread(&caller,
            ProtoString::createSymbol(&caller, ("walker-" + std::to_string(t)).c_str()),
            concurrentWalkerMain, nullptr, nullptr));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (shared.inside.load() < kConcurrentWalkers && std::chrono::steady_clock::now() < deadline)
        ProtoThread::sleepFor(&caller, 1000);
    const int allInside = shared.inside.load();

    std::vector<proto_ulong> perShard;
    for (auto* shard : space.impl->walkPins) perShard.push_back(shard->size());
    const uint64_t cycles = runCycles(space, &caller, 3);
    const int freedDuringWalks = g_probesFreed.load();
    shared.release.store(true);
    for (const ProtoThread* t : threads) const_cast<ProtoThread*>(t)->join(&caller);
    g_concurrent = nullptr;

    runCycles(space, &caller, 4);
    const int freedAfter = g_probesFreed.load();
    const proto_ulong pinsAfter = totalWalkPins(space.impl);

    proto_ulong pinnedInside = 0, maxPerShard = 0, emptyShards = 0;
    std::string occupancy;
    for (proto_ulong n : perShard) {
        pinnedInside += n;
        maxPerShard = std::max(maxPerShard, n);
        if (n == 0) ++emptyShards;
        occupancy += std::to_string(n) + " ";
    }
    const proto_ulong share = (kConcurrentWalkers + ProtoSpaceImplementation::kWalkPinShards - 1)
                              / ProtoSpaceImplementation::kWalkPinShards;
    std::printf("[ ConcurrentWalkers ] walkers=%d inside=%d shards=[ %s] cycles=%llu calls=%d wrong=%d "
                "timedOut=%d freedDuringWalks=%d freedAfter=%d pinsAfter=%llu\n",
                kConcurrentWalkers, allInside, occupancy.c_str(), static_cast<unsigned long long>(cycles),
                shared.calls.load(), shared.wrong.load(), shared.timedOut.load(), freedDuringWalks, freedAfter,
                static_cast<unsigned long long>(pinsAfter));

    ASSERT_EQ(base, 0u);
    EXPECT_EQ(allInside, kConcurrentWalkers) << "not every walker reached its callback";
    EXPECT_EQ(pinnedInside, static_cast<proto_ulong>(kConcurrentWalkers)) << "one pin per walk in progress";
    EXPECT_EQ(emptyShards, 0u) << "a shard was left unused while threads shared others";
    EXPECT_LE(maxPerShard, share) << "a shard holds more than its round-robin share";
    EXPECT_GE(cycles, 3u) << "no collection ran while the walkers stood on their receivers";
    EXPECT_EQ(freedDuringWalks, 0) << "a walked list was freed while its walk stood on it";
    EXPECT_EQ(shared.calls.load(), kConcurrentWalkers * kEntriesPerWalker);
    EXPECT_EQ(shared.wrong.load(), 0);
    EXPECT_EQ(shared.timedOut.load(), 0);
    EXPECT_EQ(freedAfter, kConcurrentWalkers * kEntriesPerWalker)
        << "probes not freed after the walks (unreachable? pins released?)";
    EXPECT_EQ(pinsAfter, base) << "a pin leaked";
}
