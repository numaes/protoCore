// GlobalMutableTableTests.cpp -- mutable objects shared between ProtoSpaces.
//
// Until the table of mutable states became process-global, every ProtoSpace
// had its own table and its own ref counter, both starting at 1.  An object of
// space A read through a context of space B was resolved in B's table: it
// answered the state of B's object with the same ref, or its own birth state,
// and never an error.  See docs/GLOBAL_MUTABLE_TABLE.md.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace proto;

namespace {

struct Space {
    ProtoSpace   space;
    ProtoContext ctx{&space, space.rootContext, nullptr, nullptr, nullptr, nullptr};
};

const ProtoString* sym(ProtoContext* c, const char* s) { return ProtoString::createSymbol(c, s); }

long longAttr(ProtoContext* c, const ProtoObject* o, const char* name) {
    const ProtoObject* v = o->getAttribute(c, sym(c, name));
    return (v && v != PROTO_NONE && v->isInteger(c)) ? v->asLong(c) : -1;
}

// Forces `minCycles` complete cycles in `space`: the young generation is
// submitted (safepoint) so the cycles have real candidates.
uint64_t forceCycles(ProtoSpace& space, ProtoContext* parent, uint64_t minCycles) {
    const uint64_t start = space.getGCCycleCount();
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + 40000);
    for (int batch = 0; batch < 400 && space.getGCCycleCount() - start < minCycles; ++batch) {
        ProtoContext garbage(&space, parent, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 5000; ++i) {
            (void) garbage.newObject(false);
            if ((i & 1023) == 0) garbage.safepoint();
        }
        garbage.safepoint();
    }
    space.setHeapLimits(0, 0);
    return space.getGCCycleCount() - start;
}

}  // namespace

// The first mutable of each space had ref 1 in both, so each space's read of
// the other's object resolved its own object.
TEST(GlobalMutableTable, ReadThroughAnotherSpaceSeesTheOwnersState) {
    Space a, b;
    const ProtoObject* inA = a.ctx.newObject(true);
    const ProtoObject* inB = b.ctx.newObject(true);
    inA->setAttribute(&a.ctx, sym(&a.ctx, "value"), a.ctx.fromInteger(42));
    inB->setAttribute(&b.ctx, sym(&b.ctx, "value"), b.ctx.fromInteger(7));

    EXPECT_EQ(longAttr(&b.ctx, inA, "value"), 42) << "A's object read through B's context";
    EXPECT_EQ(longAttr(&a.ctx, inB, "value"), 7) << "B's object read through A's context";
    EXPECT_EQ(longAttr(&a.ctx, inA, "value"), 42);
    EXPECT_EQ(longAttr(&b.ctx, inB, "value"), 7);
}

TEST(GlobalMutableTable, WriteThroughAnotherSpaceIsVisibleToTheOwner) {
    Space a, b;
    const ProtoObject* inA = a.ctx.newObject(true);
    inA->setAttribute(&a.ctx, sym(&a.ctx, "value"), a.ctx.fromInteger(1));
    inA->setAttribute(&b.ctx, sym(&b.ctx, "value"), b.ctx.fromInteger(99));
    EXPECT_EQ(longAttr(&a.ctx, inA, "value"), 99);
    EXPECT_EQ(longAttr(&b.ctx, inA, "value"), 99);
}

// A state written by B's thread onto A's object holds cells allocated in B.
// Only the global table reaches them, so B's collector must mark the table;
// A's collector marks the same table and must not lose A's cells either.
TEST(GlobalMutableTable, StateAllocatedInAnotherSpaceSurvivesBothCollectors) {
    Space a, b;
    const ProtoObject* inA = a.ctx.newObject(true);
    const ProtoString* key = sym(&a.ctx, "payload");
    {
        ProtoContext writer(&b.space, &b.ctx, nullptr, nullptr, nullptr, nullptr);
        ProtoContext::CriticalSection cs(&writer);
        const ProtoList* list = writer.newList();
        for (int i = 0; i < 20; ++i) list = list->appendLast(&writer, writer.fromInteger(i * 3));
        inA->setAttribute(&writer, key, list->asObject(&writer));
        writer.safepoint();
    }
    ASSERT_GE(forceCycles(b.space, &b.ctx, 3), 3u);
    ASSERT_GE(forceCycles(a.space, &a.ctx, 3), 3u);

    for (ProtoContext* c : {&a.ctx, &b.ctx}) {
        const ProtoObject* v = inA->getAttribute(c, key);
        ASSERT_NE(v, nullptr);
        ASSERT_NE(v, PROTO_NONE);
        const ProtoList* l = v->asList(c);
        ASSERT_NE(l, nullptr);
        ASSERT_EQ(l->getSize(c), 20u);
        for (int i = 0; i < 20; ++i) EXPECT_EQ(l->getAt(c, i)->asLong(c), i * 3);
    }
}

// Refs name their space: a space's refs are disjoint from every other's, and
// the first space of the process keeps the refs it always had.
TEST(GlobalMutableTable, RefsAreUniqueAcrossSpaces) {
    Space a, b;
    const auto* ca = toImpl<const ProtoObjectCell>(a.ctx.newObject(true));
    const auto* cb = toImpl<const ProtoObjectCell>(b.ctx.newObject(true));
    EXPECT_NE(ca->mutable_ref, 0u);
    EXPECT_NE(cb->mutable_ref, 0u);
    EXPECT_NE(ca->mutable_ref >> kMutableRefSpaceShift, cb->mutable_ref >> kMutableRefSpaceShift);
}

// Entries of a destroyed space are removed by a later cycle of a live space.
TEST(GlobalMutableTable, EntriesOfADestroyedSpaceArePurged) {
    Space a;
    const ProtoObject* inA = a.ctx.newObject(true);
    inA->setAttribute(&a.ctx, sym(&a.ctx, "value"), a.ctx.fromInteger(5));
    unsigned long deadId = 0;
    {
        auto b = std::make_unique<Space>();
        const ProtoObject* first = b->ctx.newObject(true);
        deadId = toImpl<const ProtoObjectCell>(first)->mutable_ref >> kMutableRefSpaceShift;
        for (int i = 0; i < 100; ++i) {
            const ProtoObject* o = i == 0 ? first : b->ctx.newObject(true);
            o->setAttribute(&b->ctx, sym(&b->ctx, "value"), b->ctx.fromInteger(i));
        }
        ASSERT_EQ(multispace::countMutableEntriesOfSpace(a.space.rootContext, deadId), 100u);
    }
    ASSERT_GE(forceCycles(a.space, &a.ctx, 2), 2u);
    EXPECT_EQ(multispace::countMutableEntriesOfSpace(a.space.rootContext, deadId), 0u);
    EXPECT_EQ(longAttr(&a.ctx, inA, "value"), 5);
}

// Registered threads of two spaces write shared and private mutable objects
// while both collectors run.  A freed-and-reused table node or state shows up
// as a non-integer read, a lost private write, or an AddressSanitizer report.
namespace {

struct StressShared {
    std::vector<const ProtoObject*> shared;   // owned by space A, written by all
    std::atomic<int> started{0};
    std::atomic<bool> go{false};
    std::atomic<int> done{0};
    std::atomic<long> errors{0};
    int iterations = 4000;
};
StressShared* gStress = nullptr;

const ProtoObject* stressMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoList*, const ProtoSparseList*) {
    StressShared& s = *gStress;
    const int self = s.started.fetch_add(1);
    {
        ProtoContext::UnmanagedScope parked(ctx);
        while (!s.go.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const ProtoString* slot = sym(ctx, "slot");
    const ProtoString* mine = sym(ctx, "mine");
    const ProtoObject* priv = ctx->newObject(true);
    for (int i = 0; i < s.iterations; ++i) {
        ProtoContext step(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* target = s.shared[(i + self * 7) % s.shared.size()];
        const ProtoList* boxed = step.newList()->appendLast(&step, step.fromInteger(i));
        target->setAttribute(&step, slot, boxed->asObject(&step));
        const ProtoObject* back = target->getAttribute(&step, slot);
        const ProtoList* l = back ? back->asList(&step) : nullptr;
        if (!l || l->getSize(&step) != 1 || !l->getAt(&step, 0)->isInteger(&step)) s.errors.fetch_add(1);
        priv->setAttribute(&step, mine, step.fromInteger(i));
        if (longAttr(&step, priv, "mine") != i) s.errors.fetch_add(1);
        if ((i & 63) == 0) step.safepoint();
    }
    s.done.fetch_add(1);
    return PROTO_NONE;
}

}  // namespace

TEST(GlobalMutableTable, ThreadsOfTwoSpacesShareMutablesUnderCollection) {
    Space a, b;
    StressShared s;
    for (int i = 0; i < 16; ++i) s.shared.push_back(a.ctx.newObject(true));
    gStress = &s;
    a.space.setHeapLimits(0, a.space.heapSize + 60000);
    b.space.setHeapLimits(0, b.space.heapSize + 60000);
    const uint64_t cyclesA = a.space.getGCCycleCount();
    const uint64_t cyclesB = b.space.getGCCycleCount();

    std::vector<const ProtoThread*> threads;
    for (Space* sp : {&a, &b, &a, &b}) {
        threads.push_back(sp->space.newThread(sp->space.rootContext, sym(sp->space.rootContext, "stress"),
                                              stressMain, nullptr, nullptr));
    }
    {
        ProtoContext::UnmanagedScope pa(a.space.rootContext);
        ProtoContext::UnmanagedScope pb(b.space.rootContext);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (s.started.load() < 4 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        s.go = true;
        while (s.done.load() < 4 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(s.done.load(), 4) << "the workers did not finish";
    {
        ProtoContext::UnmanagedScope pa(a.space.rootContext);
        ProtoContext::UnmanagedScope pb(b.space.rootContext);
        for (size_t i = 0; i < threads.size(); ++i) {
            Space* sp = (i % 2 == 0) ? &a : &b;
            const_cast<ProtoThread*>(threads[i])->join(sp->space.rootContext);
        }
    }
    gStress = nullptr;
    a.space.setHeapLimits(0, 0);
    b.space.setHeapLimits(0, 0);
    EXPECT_EQ(s.errors.load(), 0);
    EXPECT_GE(a.space.getGCCycleCount() - cyclesA, 2u) << "space A must have collected";
    EXPECT_GE(b.space.getGCCycleCount() - cyclesB, 2u) << "space B must have collected";
}
