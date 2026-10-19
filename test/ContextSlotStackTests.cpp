// ContextSlotStackTests.cpp - ProtoContext slot arrays from the thread's stack.
//
// A context's automaticLocals array is carved from a per-thread region
// (ContextSlotStack, proto_internal.h) instead of new[]: contexts of one
// thread nest LIFO, so creating one moves the stack's top up and destroying
// it moves it back.  These tests check that nested contexts take adjacent
// slots and reuse them, that slots start as PROTO_NONE, that a context at the
// top grows in place and one below it moves with its values, that contexts
// destroyed out of order leave holes the stack later absorbs, that a full
// stack falls back to the heap, and that slots stay GC roots.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <vector>

using namespace proto;

class ContextSlotStack_ : public ::testing::Test {
protected:
    ProtoSpace space;
    ProtoContext* ctx = nullptr;
    void SetUp() override { ctx = space.rootContext; }

    ContextSlotStack* stack(ProtoContext* c) { return contextSlotStackOf(c->mutableValueCache_); }
};

TEST_F(ContextSlotStack_, NestedContextsTakeAdjacentSlotsAndReuseThem) {
    const ProtoObject** first = nullptr;
    {
        ProtoContext a(&space, ctx, nullptr, nullptr, nullptr, nullptr, 100);
        ASSERT_NE(stack(&a), nullptr);
        first = a.getAutomaticLocals();
        ProtoContext b(&space, &a, nullptr, nullptr, nullptr, nullptr, 50);
        EXPECT_EQ(b.getAutomaticLocals(), first + 100) << "a child's slots follow its parent's";
        EXPECT_EQ(stack(&b)->top, first + 150);
    }
    ProtoContext c(&space, ctx, nullptr, nullptr, nullptr, nullptr, 10);
    EXPECT_EQ(c.getAutomaticLocals(), first) << "slots are reused after the frames are gone";
}

TEST_F(ContextSlotStack_, SlotsStartAsNone) {
    {
        ProtoContext a(&space, ctx, nullptr, nullptr, nullptr, nullptr, 64);
        for (unsigned i = 0; i < 64; ++i) a.setAutomaticLocal(i, ctx->fromInteger(i));
    }
    ProtoContext b(&space, ctx, nullptr, nullptr, nullptr, nullptr, 64);
    for (unsigned i = 0; i < 64; ++i) EXPECT_EQ(b.getAutomaticLocal(i), PROTO_NONE) << "slot " << i;
}

TEST_F(ContextSlotStack_, ResizeAtTheTopGrowsInPlace) {
    ProtoContext a(&space, ctx, nullptr, nullptr, nullptr, nullptr, 4);
    const ProtoObject** slots = a.getAutomaticLocals();
    a.setAutomaticLocal(3, ctx->fromInteger(33));
    a.resizeAutomaticLocals(40);
    EXPECT_EQ(a.getAutomaticLocals(), slots);
    EXPECT_EQ(a.getAutomaticLocalsCount(), 40u);
    EXPECT_EQ(a.getAutomaticLocal(3)->asLong(ctx), 33);
    for (unsigned i = 4; i < 40; ++i) EXPECT_EQ(a.getAutomaticLocal(i), PROTO_NONE) << "slot " << i;
    EXPECT_EQ(stack(&a)->top, slots + 40);
}

TEST_F(ContextSlotStack_, ResizeBelowTheTopMovesWithItsValues) {
    ProtoContext a(&space, ctx, nullptr, nullptr, nullptr, nullptr, 4);
    for (unsigned i = 0; i < 4; ++i) a.setAutomaticLocal(i, ctx->fromInteger(10 + i));
    const ProtoObject** before = a.getAutomaticLocals();
    {
        ProtoContext b(&space, &a, nullptr, nullptr, nullptr, nullptr, 8);
        a.resizeAutomaticLocals(20);   // b is above a: a moves
        EXPECT_NE(a.getAutomaticLocals(), before);
        for (unsigned i = 0; i < 4; ++i) EXPECT_EQ(a.getAutomaticLocal(i)->asLong(ctx), 10 + i);
        for (unsigned i = 4; i < 20; ++i) EXPECT_EQ(a.getAutomaticLocal(i), PROTO_NONE);
    }
    // a's old slots and b's are holes/popped; a's new slots stay valid.
    for (unsigned i = 0; i < 4; ++i) EXPECT_EQ(a.getAutomaticLocal(i)->asLong(ctx), 10 + i);
}

TEST_F(ContextSlotStack_, OutOfOrderContextsLeaveHolesThatAreAbsorbed) {
    ContextSlotStack* s = nullptr;
    const ProtoObject** base = nullptr;
    {
        ProtoContext probe(&space, ctx, nullptr, nullptr, nullptr, nullptr, 1);
        s = stack(&probe);
        base = probe.getAutomaticLocals();
    }
    // Two heap contexts on the same parent (as embedders create builder
    // contexts with new), destroyed in creation order rather than LIFO.
    auto* a = new ProtoContext(&space, ctx, nullptr, nullptr, nullptr, nullptr, 30);
    auto* b = new ProtoContext(&space, ctx, nullptr, nullptr, nullptr, nullptr, 20);
    EXPECT_EQ(a->getAutomaticLocals(), base);
    EXPECT_EQ(b->getAutomaticLocals(), base + 30);
    const unsigned holesBefore = s->holeCount;
    delete a;   // not at the top: its slots become a hole
    EXPECT_EQ(s->holeCount, holesBefore + 1);
    EXPECT_EQ(s->top, base + 50);
    delete b;   // at the top: pops, and the hole below it is absorbed
    EXPECT_EQ(s->holeCount, holesBefore) << "the hole is absorbed when the top pops down to it";
    EXPECT_EQ(s->top, base);
}

TEST_F(ContextSlotStack_, AFullStackFallsBackToTheHeap) {
    ProtoContext big(&space, ctx, nullptr, nullptr, nullptr, nullptr, ContextSlotStack::kSlots - 8);
    ContextSlotStack* s = stack(&big);
    const ProtoObject** top = s->top;
    {
        ProtoContext over(&space, &big, nullptr, nullptr, nullptr, nullptr, 64);
        EXPECT_FALSE(over.getAutomaticLocals() >= s->base && over.getAutomaticLocals() < s->limit)
            << "no room: the slots come from new[]";
        EXPECT_EQ(s->top, top);
        for (unsigned i = 0; i < 64; ++i) EXPECT_EQ(over.getAutomaticLocal(i), PROTO_NONE);
    }
    EXPECT_EQ(s->top, top);
}

// Slots on the stack are roots: an object held only in a slot survives
// collection cycles.
TEST_F(ContextSlotStack_, SlotsOnTheStackAreRoots) {
    ProtoContext frame(&space, ctx, nullptr, nullptr, nullptr, nullptr, 8);
    const ProtoString* key = ProtoString::createSymbol(ctx, "kept");
    frame.setAutomaticLocal(5, frame.newObject()->setAttribute(&frame, key, frame.fromInteger(4242)));
    for (int round = 0; round < 3; ++round) {
        {
            ProtoContext garbage(&space, &frame, nullptr, nullptr, nullptr, nullptr, 4);
            for (int i = 0; i < 20000; ++i) garbage.newObject();
        }
        space.triggerGC();
        for (int i = 0; i < 50; ++i) frame.safepoint();
    }
    EXPECT_EQ(frame.getAutomaticLocal(5)->getAttribute(&frame, key)->asLong(&frame), 4242);
}
