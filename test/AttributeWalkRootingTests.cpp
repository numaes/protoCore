// AttributeWalkRootingTests.cpp -- what keeps the attribute tree of a
// MUTABLE receiver alive while ProtoObject::processOwnAttributes runs its
// callback.
//
// The walk resolves the receiver's state once and then walks that state's
// attribute tree, calling the callback between nodes.  The callback may
// allocate, call safepoint() and park for collections.  A mutable object's
// state is reached by the collector only through its handle, as the entry
// the handle's ref names in the table snapshot of the pause
// (docs/GLOBAL_MUTABLE_TABLE.md, "States are not roots").  So once a write
// supersedes the state being walked, nothing in the object graph reaches it,
// and a collection run from the callback would free the tree under the walk.
// The walk pins the state in a root set of the space for its duration
// (core/ProtoObject.cpp, WalkPin).
//
// Probes: every attribute value of the walked state is an external pointer
// with a counting finalizer, and after the state is superseded nothing but
// that state holds them.  A finalizer runs only when the collector frees the
// cell, so a probe freed during the walk proves the walked state was freed.
// After the walk the probes MUST be freed by later cycles: that proves the
// test really made them unreachable, and that the pin is released.
//
// Each test here FAILS when the pin is removed from processOwnAttributes
// (that variant was built and run against these tests).  The nested test
// also failed against 3.3.1's ProtoContext::pendingRoot anchor, whose single
// slot newList(n) displaced for the length of the build.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace proto;

namespace {

std::atomic<int> g_probesFreed{0};
void countFree(void*) { g_probesFreed.fetch_add(1, std::memory_order_relaxed); }

const ProtoString* sym(ProtoContext* c, const std::string& s) {
    return ProtoString::createSymbol(c, s.c_str());
}

void requestCycle(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    space.gcStarted = true;
    space.gcCV.notify_all();
}

// Runs at least `count` complete cycles, parking `ctx` at safepoints
// meanwhile.  Returns the number of cycles that completed.
uint64_t runCycles(ProtoSpace& space, ProtoContext* ctx, uint64_t count) {
    const uint64_t start = space.getGCCycleCount();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (space.getGCCycleCount() - start < count &&
           std::chrono::steady_clock::now() < deadline) {
        requestCycle(space);
        // Garbage in a child context, so each cycle has something to sweep
        // and the freed cells are handed out again soon after.
        {
            ProtoContext garbage(&space, ctx, nullptr, nullptr, nullptr, nullptr);
            for (int i = 0; i < 2000; ++i) (void) garbage.newObject(false);
        }
        ctx->safepoint();
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return space.getGCCycleCount() - start;
}

// A plain OS thread (outside the stop-the-world quorum) that keeps asking
// for cycles until it is stopped.
class CycleKicker {
public:
    explicit CycleKicker(ProtoSpace& space)
        : thread_([this, &space]() {
              while (!stop_.load(std::memory_order_relaxed)) {
                  requestCycle(space);
                  std::this_thread::sleep_for(std::chrono::microseconds(100));
              }
          }) {}
    ~CycleKicker() {
        stop_.store(true, std::memory_order_relaxed);
        thread_.join();
    }
private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

constexpr int kAttributes = 64;

// A mutable object whose state, attribute tree and probe values were built in
// a context that is then destroyed, so all of them are sweep candidates.  The
// handle is rooted in `caller`'s automatic local 0.
struct Fixture {
    ProtoSpace space;
    unsigned savedThreshold = 0;
    ProtoContext* caller = nullptr;
    ProtoObject* obj = nullptr;
    std::vector<const ProtoString*> keys;
    std::vector<const ProtoObject*> probes;

    Fixture() {
        ProtoContext* root = space.rootContext;
        // A safepoint of a context with more than 500 young cells submits
        // its young generation: the callbacks below do so on purpose.
        savedThreshold = space.maxAllocatedCellsPerContext;
        space.maxAllocatedCellsPerContext = 500;
        caller = new ProtoContext(&space, root, nullptr, nullptr, nullptr, nullptr, 1);
        for (int i = 0; i < kAttributes; ++i) keys.push_back(sym(caller, "walk_probe_" + std::to_string(i)));
        {
            ProtoContext builder(&space, caller, nullptr, nullptr, nullptr, nullptr);
            obj = const_cast<ProtoObject*>(builder.newObject(true));
            for (int i = 0; i < kAttributes; ++i) {
                const ProtoObject* probe = builder.fromExternalPointer(nullptr, &countFree);
                probes.push_back(probe);
                obj->setAttribute(&builder, keys[i], probe);
            }
            caller->getAutomaticLocals()[0] = obj;
        }   // ~builder: its young generation is submitted.
        g_probesFreed.store(0);
    }
    ~Fixture() {
        space.maxAllocatedCellsPerContext = savedThreshold;
        delete caller;
    }

    // Replaces the value of every attribute: every node of the walked tree is
    // path-copied away, so the walked state and its tree, and the probes, are
    // reachable from nothing but the walk.
    void supersede(ProtoContext* ctx) {
        for (int i = 0; i < kAttributes; ++i) obj->setAttribute(ctx, keys[i], ctx->fromInteger(-1 - i));
    }

    int indexOf(const ProtoString* name) const {
        for (int i = 0; i < kAttributes; ++i) if (keys[i] == name) return i;
        return -1;
    }
};

struct WalkRecord {
    Fixture* fixture = nullptr;
    int calls = 0;
    int wrongPairs = 0;          // name unknown, or value not the probe set with it
    int freedDuringWalk = 0;     // probes finalized before the walk ended
    uint64_t cyclesDuringWalk = 0;
    bool superseded = false;
};

void checkPair(WalkRecord* r, const ProtoString* name, const ProtoObject* value) {
    ++r->calls;
    const int i = r->fixture->indexOf(name);
    // Pointer comparison only: a freed value is never dereferenced.
    if (i < 0 || value != r->fixture->probes[i]) ++r->wrongPairs;
}

}  // namespace

// The callback supersedes the walked state, then runs collections and
// reaches safepoints.  The walk must report the state it started from, and
// no probe of that state may be freed while the walk is running.
TEST(AttributeWalkRooting, SupersededStateSurvivesCollectionsRunFromTheCallback) {
    Fixture f;
    WalkRecord record;
    record.fixture = &f;
    const uint64_t cyclesBefore = f.space.getGCCycleCount();

    f.obj->processOwnAttributes(f.caller, &record,
        [](ProtoContext* ctx, void* self, const ProtoString* name, const ProtoObject* value) {
            auto* r = static_cast<WalkRecord*>(self);
            checkPair(r, name, value);
            if (!r->superseded) {
                r->superseded = true;
                r->fixture->supersede(ctx);
                // Allocate heavily and let collections run.
                for (int i = 0; i < 2000; ++i) (void) ctx->newObject(false);
                ctx->safepoint();
                r->cyclesDuringWalk += runCycles(r->fixture->space, ctx, 3);
            } else if ((r->calls & 7) == 0) {
                r->cyclesDuringWalk += runCycles(r->fixture->space, ctx, 1);
            }
        });
    record.freedDuringWalk = g_probesFreed.load();
    const uint64_t cyclesDuringWalk = f.space.getGCCycleCount() - cyclesBefore;

    // After the walk nothing holds the probes: later cycles must free them.
    runCycles(f.space, f.caller, 4);
    const int freedAfter = g_probesFreed.load();

    std::printf("[ SupersededState ] calls=%d wrong=%d freedDuringWalk=%d "
                "cyclesDuringWalk=%llu freedAfterWalk=%d/%d\n",
                record.calls, record.wrongPairs, record.freedDuringWalk,
                static_cast<unsigned long long>(cyclesDuringWalk), freedAfter, kAttributes);
    EXPECT_EQ(record.calls, kAttributes);
    EXPECT_EQ(record.wrongPairs, 0) << "the walk reported pairs of a freed or foreign tree";
    EXPECT_EQ(record.freedDuringWalk, 0)
        << "the state being walked was freed by a collection run from the callback";
    EXPECT_GE(cyclesDuringWalk, 3u) << "no collection ran during the walk; the test proved nothing";
    EXPECT_EQ(freedAfter, kAttributes)
        << "the probes were not freed after the walk: either they were not unreachable "
           "(the test proves nothing) or the walk's pin was not released";
}

// The nested case: the callback supersedes the walked state and then builds
// large lists with ProtoContext::newList(n, items) while cycles run.  The
// walked state must stay pinned for the whole build.  (3.3.1's single
// pendingRoot slot was taken over by newList's own anchor for the length of
// the build, so the walked state was unrooted exactly then.)
TEST(AttributeWalkRooting, NewListInsideTheCallbackKeepsTheWalkedState) {
    Fixture f;
    WalkRecord record;
    record.fixture = &f;

    f.obj->processOwnAttributes(f.caller, &record,
        [](ProtoContext* ctx, void* self, const ProtoString* name, const ProtoObject* value) {
            auto* r = static_cast<WalkRecord*>(self);
            checkPair(r, name, value);
            if (r->superseded) return;
            r->superseded = true;
            r->fixture->supersede(ctx);
            // Hand this context's young generation over, so the intermediate
            // states of supersede() -- which share most of the walked tree --
            // stop protecting it: the walked state is all that holds it.
            for (int i = 0; i < 1000; ++i) (void) ctx->newObject(false);
            ctx->safepoint();
            std::vector<const ProtoObject*> items;
            for (int i = 0; i < 20000; ++i) items.push_back(ctx->fromInteger(i));
            CycleKicker kicker(r->fixture->space);
            // Build on the walking context itself, as a primitive called from
            // the callback would (a primitive runs in its caller's context),
            // until two cycles have completed inside newList calls, so at
            // least one pause fell inside a build.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (r->cyclesDuringWalk < 2 && std::chrono::steady_clock::now() < deadline) {
                const uint64_t before = r->fixture->space.getGCCycleCount();
                const ProtoList* list = ctx->newList(static_cast<unsigned>(items.size()), items.data());
                r->cyclesDuringWalk += r->fixture->space.getGCCycleCount() - before;
                if (list->getSize(ctx) != items.size()) ++r->wrongPairs;
            }
        });
    record.freedDuringWalk = g_probesFreed.load();

    runCycles(f.space, f.caller, 4);
    const int freedAfter = g_probesFreed.load();

    std::printf("[ NestedNewList ] calls=%d wrong=%d freedDuringWalk=%d "
                "cyclesInsideNewList=%llu freedAfterWalk=%d/%d\n",
                record.calls, record.wrongPairs, record.freedDuringWalk,
                static_cast<unsigned long long>(record.cyclesDuringWalk), freedAfter, kAttributes);
    EXPECT_EQ(record.calls, kAttributes);
    EXPECT_EQ(record.wrongPairs, 0);
    EXPECT_EQ(record.freedDuringWalk, 0)
        << "the state being walked was freed while newList ran inside the callback";
    EXPECT_GE(record.cyclesDuringWalk, 2u) << "no cycle completed inside a newList build";
    EXPECT_EQ(freedAfter, kAttributes)
        << "the probes were not freed after the walk (unreachable? pin released?)";
}

namespace {

// Another ProtoThread keeps replacing the receiver's state.
struct WriterState {
    ProtoObject* obj = nullptr;
    const std::vector<const ProtoString*>* keys = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> writes{0};
};
WriterState* g_writer = nullptr;

const ProtoObject* writerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoArguments*, const ProtoKeywordArguments*) {
    const ProtoString* idx = ProtoString::createSymbol(ctx, "idx");
    while (!g_writer->stop.load(std::memory_order_relaxed)) {
        ProtoContext frame(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (size_t i = 0; i < g_writer->keys->size(); ++i) {
            // A fresh heap value per write, carrying the index of its key.
            const ProtoObject* value = frame.newObject(false)->setAttribute(
                &frame, idx, frame.fromInteger(static_cast<long long>(i)));
            g_writer->obj->setAttribute(&frame, (*g_writer->keys)[i], value);
            g_writer->writes.fetch_add(1, std::memory_order_relaxed);
        }
        frame.safepoint();
    }
    return PROTO_NONE;
}

struct ConcurrentRecord {
    const std::vector<const ProtoString*>* keys = nullptr;
    const ProtoString* idx = nullptr;
    int calls = 0;
    int wrongPairs = 0;
};

}  // namespace

// A writer thread replaces the state continuously while the callback
// allocates heavily and reaches safepoints with cycles forced.  Every value
// is a heap object carrying the index of the key it was stored under: a pair
// read from a freed and reused tree reports a wrong index, a non-object, or
// crashes.
TEST(AttributeWalkRooting, ConcurrentWriterAndAllocatingCallback) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    const unsigned savedThreshold = space.maxAllocatedCellsPerContext;
    space.maxAllocatedCellsPerContext = 500;
    ProtoContext caller(&space, root, nullptr, nullptr, nullptr, nullptr, 1);

    std::vector<const ProtoString*> keys;
    for (int i = 0; i < kAttributes; ++i) keys.push_back(sym(&caller, "shared_" + std::to_string(i)));
    const ProtoString* idx = ProtoString::createSymbol(&caller, "idx");
    auto* obj = const_cast<ProtoObject*>(caller.newObject(true));
    for (int i = 0; i < kAttributes; ++i)
        obj->setAttribute(&caller, keys[i], caller.newObject(false)->setAttribute(&caller, idx, caller.fromInteger(i)));
    caller.getAutomaticLocals()[0] = obj;

    WriterState writer;
    writer.obj = obj;
    writer.keys = &keys;
    g_writer = &writer;
    const ProtoThread* writerThread = space.newThread(&caller, sym(&caller, "walk-writer"), writerMain, nullptr, nullptr);
    {
        ProtoContext::UnmanagedScope parked(&caller);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (writer.writes.load() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    int calls = 0, wrong = 0, walks = 0;
    const uint64_t writesBefore = writer.writes.load();
    const uint64_t cyclesBefore = space.getGCCycleCount();
    {
        CycleKicker kicker(space);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while ((walks < 200 || space.getGCCycleCount() - cyclesBefore < 20) &&
               std::chrono::steady_clock::now() < deadline) {
            ConcurrentRecord record;
            record.keys = &keys;
            record.idx = idx;
            obj->processOwnAttributes(&caller, &record,
                [](ProtoContext* ctx, void* self, const ProtoString* name, const ProtoObject* value) {
                    auto* r = static_cast<ConcurrentRecord*>(self);
                    ++r->calls;
                    long long expected = -1;
                    for (size_t i = 0; i < r->keys->size(); ++i)
                        if ((*r->keys)[i] == name) expected = static_cast<long long>(i);
                    // Allocate past the submission threshold and safepoint:
                    // the walking context's own young generation is handed
                    // over, and the thread parks for the forced cycles.
                    {
                        ProtoContext scratch(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
                        for (int i = 0; i < 300; ++i) (void) scratch.newObject(false);
                    }
                    for (int i = 0; i < 600; ++i) (void) ctx->newObject(false);
                    ctx->safepoint();
                    if (expected < 0 || !value || !value->isCell(ctx)) { ++r->wrongPairs; return; }
                    const ProtoObject* stored = value->getAttribute(ctx, r->idx);
                    if (!stored || !stored->isInteger(ctx) || stored->asLong(ctx) != expected) ++r->wrongPairs;
                });
            calls += record.calls;
            wrong += record.wrongPairs;
            if (record.calls != kAttributes) ++wrong;
            ++walks;
        }
    }
    const uint64_t cycles = space.getGCCycleCount() - cyclesBefore;
    const uint64_t writesDuring = writer.writes.load() - writesBefore;

    writer.stop.store(true);
    {
        ProtoContext::UnmanagedScope parked(&caller);
        const_cast<ProtoThread*>(writerThread)->join(&caller);
    }
    g_writer = nullptr;
    space.maxAllocatedCellsPerContext = savedThreshold;

    std::printf("[ ConcurrentWriter ] walks=%d calls=%d wrong=%d cycles=%llu writesDuringWalks=%llu\n",
                walks, calls, wrong, static_cast<unsigned long long>(cycles),
                static_cast<unsigned long long>(writesDuring));
    EXPECT_EQ(wrong, 0) << "a walk reported a pair of a freed or reused tree";
    EXPECT_GE(cycles, 20u) << "too few cycles ran during the walks";
    EXPECT_GT(writesDuring, 0u) << "the writer never ran during the walks";
}
