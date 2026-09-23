// ProtoMPSCQueueTests.cpp - single-threaded semantics of ProtoMPSCQueue.
// PMQ-SPEC section 2 and section 2.1.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <vector>

using namespace proto;

namespace {

const ProtoObject* boxed(ProtoContext* c, long v) {
    // A real cell, so the GC has something to trace (an item that is only a
    // SmallInteger would be embedded and prove nothing about tracing).
    return c->newList()->appendLast(c, c->fromInteger(v))->asObject(c);
}

long unboxed(ProtoContext* c, const ProtoObject* o) {
    return static_cast<long>(o->asList(c)->getAt(c, 0)->asLong(c));
}

}  // namespace

TEST(MPSCQueue, NewQueueIsEmptyAndTakeAllReturnsAnEmptyList) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    const ProtoMPSCQueue* q = ctx.newMPSCQueue();
    EXPECT_TRUE(q->isEmpty(&ctx));

    const ProtoList* got = q->takeAll(&ctx);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->getSize(&ctx), 0u);
    EXPECT_TRUE(q->isEmpty(&ctx));
}

TEST(MPSCQueue, OneProducerSeesFIFOOrder) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoMPSCQueue* q = ctx.newMPSCQueue();

    constexpr long kN = 1000;
    for (long i = 0; i < kN; ++i) q->push(&ctx, boxed(&ctx, i));
    EXPECT_FALSE(q->isEmpty(&ctx));

    const ProtoList* got = q->takeAll(&ctx);
    ASSERT_EQ(got->getSize(&ctx), static_cast<unsigned long>(kN));
    for (long i = 0; i < kN; ++i)
        EXPECT_EQ(unboxed(&ctx, got->getAt(&ctx, static_cast<int>(i))), i) << "at " << i;

    EXPECT_TRUE(q->isEmpty(&ctx));
    EXPECT_EQ(q->takeAll(&ctx)->getSize(&ctx), 0u);
}

TEST(MPSCQueue, ItemsPushedAfterATakeAllBelongToTheNextBatch) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoMPSCQueue* q = ctx.newMPSCQueue();

    for (long i = 0; i < 5; ++i) q->push(&ctx, boxed(&ctx, i));
    const ProtoList* first = q->takeAll(&ctx);
    for (long i = 5; i < 9; ++i) q->push(&ctx, boxed(&ctx, i));
    const ProtoList* second = q->takeAll(&ctx);

    ASSERT_EQ(first->getSize(&ctx), 5u);
    ASSERT_EQ(second->getSize(&ctx), 4u);
    EXPECT_EQ(unboxed(&ctx, first->getAt(&ctx, 0)), 0);
    EXPECT_EQ(unboxed(&ctx, first->getAt(&ctx, 4)), 4);
    EXPECT_EQ(unboxed(&ctx, second->getAt(&ctx, 0)), 5);
    EXPECT_EQ(unboxed(&ctx, second->getAt(&ctx, 3)), 8);
}

// PMQ-SPEC section 2: items are never copied - the queue stores the pointer.
TEST(MPSCQueue, ItemsAreReturnedByIdentity) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoMPSCQueue* q = ctx.newMPSCQueue();

    std::vector<const ProtoObject*> pushed;
    for (long i = 0; i < 20; ++i) {
        const ProtoObject* o = boxed(&ctx, i);
        pushed.push_back(o);
        q->push(&ctx, o);
    }
    const ProtoList* got = q->takeAll(&ctx);
    ASSERT_EQ(got->getSize(&ctx), pushed.size());
    for (size_t i = 0; i < pushed.size(); ++i)
        EXPECT_EQ(got->getAt(&ctx, static_cast<int>(i)), pushed[i]) << "at " << i;
}

// Embedded values are valid items and survive the round trip untouched.
TEST(MPSCQueue, EmbeddedItemsRoundTrip) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoMPSCQueue* q = ctx.newMPSCQueue();

    q->push(&ctx, ctx.fromInteger(7));
    q->push(&ctx, ctx.fromBoolean(true));
    q->push(&ctx, PROTO_NONE);

    const ProtoList* got = q->takeAll(&ctx);
    ASSERT_EQ(got->getSize(&ctx), 3u);
    EXPECT_EQ(got->getAt(&ctx, 0)->asLong(&ctx), 7);
    EXPECT_EQ(got->getAt(&ctx, 1), ctx.fromBoolean(true));
    EXPECT_EQ(got->getAt(&ctx, 2), PROTO_NONE);
}

// A batch larger than the inline-list form, so the AVL builder runs.
TEST(MPSCQueue, LargeBatchKeepsOrder) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoMPSCQueue* q = ctx.newMPSCQueue();

    constexpr long kN = 50000;
    for (long i = 0; i < kN; ++i) q->push(&ctx, ctx.fromInteger(i));
    const ProtoList* got = q->takeAll(&ctx);
    ASSERT_EQ(got->getSize(&ctx), static_cast<unsigned long>(kN));
    for (long i = 0; i < kN; i += 997)
        EXPECT_EQ(got->getAt(&ctx, static_cast<int>(i))->asLong(&ctx), i) << "at " << i;
    EXPECT_EQ(got->getAt(&ctx, kN - 1)->asLong(&ctx), kN - 1);
}

TEST(MPSCQueue, ObjectModelIntegration) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    const ProtoMPSCQueue* q = ctx.newMPSCQueue();
    const ProtoObject* o = q->asObject(&ctx);

    EXPECT_TRUE(o->isMPSCQueue(&ctx));
    EXPECT_EQ(o->asMPSCQueue(&ctx), q);
    EXPECT_EQ(o->getPrototype(&ctx), space.mpscQueuePrototype);

    // It is not any other collection.
    EXPECT_FALSE(o->isMap(&ctx));
    EXPECT_EQ(o->asMap(&ctx), nullptr);
    EXPECT_EQ(o->asList(&ctx), nullptr);
    EXPECT_EQ(o->asSparseList(&ctx), nullptr);

    // Nothing else answers true to isMPSCQueue.
    EXPECT_FALSE(ctx.newMap()->asObject(&ctx)->isMPSCQueue(&ctx));
    EXPECT_FALSE(ctx.newList()->asObject(&ctx)->isMPSCQueue(&ctx));
    EXPECT_FALSE(ctx.fromInteger(3)->isMPSCQueue(&ctx));

    // Identity hash: stable across mutation, distinct per queue.
    const unsigned long h = q->getHash(&ctx);
    q->push(&ctx, ctx.fromInteger(1));
    EXPECT_EQ(q->getHash(&ctx), h);
    EXPECT_NE(ctx.newMPSCQueue()->getHash(&ctx), h);
}

// A queue stored as an attribute is an ordinary traced reference: this is
// how an actor holds its three bands.
TEST(MPSCQueue, SurvivesAsAnAttributeOfAMutableObject) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    const ProtoObject* actor = ctx.newObject(true);
    const ProtoString* key = ProtoString::createSymbol(&ctx, "__mailbox__");
    const ProtoMPSCQueue* q = ctx.newMPSCQueue();
    const ProtoObject* updated = actor->setAttribute(&ctx, key, q->asObject(&ctx));
    ASSERT_NE(updated, nullptr);

    const ProtoObject* back = actor->getAttribute(&ctx, key, false);
    ASSERT_NE(back, nullptr);
    ASSERT_TRUE(back->isMPSCQueue(&ctx));
    back->asMPSCQueue(&ctx)->push(&ctx, ctx.fromInteger(5));
    EXPECT_EQ(q->takeAll(&ctx)->getSize(&ctx), 1u);
}
