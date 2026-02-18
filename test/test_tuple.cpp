#include <gtest/gtest.h>
#include "../headers/protoCore.h"

// Test fixture for ProtoTuple tests
class TupleTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space = nullptr;
    proto::ProtoContext* context;

    void SetUp() override {
        space = new proto::ProtoSpace();
        context = space->rootContext;
    }
};

TEST_F(TupleTest, CreationAndSize) {
    const proto::ProtoTuple* tuple = context->newTuple();
    ASSERT_TRUE(tuple != nullptr);
    ASSERT_EQ(tuple->getSize(context), 0);
}

TEST_F(TupleTest, CreationFromList) {
    const proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromInteger(10));
    list = list->appendLast(context, context->fromInteger(20));

    const proto::ProtoTuple* tuple = context->newTupleFromList(list);
    ASSERT_EQ(tuple->getSize(context), 2);
    ASSERT_EQ(tuple->getAt(context, 0)->asLong(context), 10);
    ASSERT_EQ(tuple->getAt(context, 1)->asLong(context), 20);
}

TEST_F(TupleTest, GetAt) {
    const proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromInteger(10));
    list = list->appendLast(context, context->fromInteger(20));
    const proto::ProtoTuple* tuple = context->newTupleFromList(list);

    ASSERT_EQ(tuple->getAt(context, 0)->asLong(context), 10);
    ASSERT_EQ(tuple->getAt(context, 1)->asLong(context), 20);
}

TEST_F(TupleTest, Interning) {
    // Create two separate lists with the same content
    const proto::ProtoList* list1 = context->newList()->appendLast(context, context->fromInteger(1));
    list1 = list1->appendLast(context, context->fromInteger(2));

    const proto::ProtoList* list2 = context->newList()->appendLast(context, context->fromInteger(1));
    list2 = list2->appendLast(context, context->fromInteger(2));

    // Create tuples from them; cast needed as newTupleFromList returns const
    const proto::ProtoTuple* tuple1 = context->newTupleFromList(list1);
    const proto::ProtoTuple* tuple2 = context->newTupleFromList(list2);

    // Because of interning, they should be the exact same object in memory
    ASSERT_EQ(tuple1, tuple2);

    // A different tuple should be a different object
    const proto::ProtoList* list3 = context->newList()->appendLast(context, context->fromInteger(99));
    const proto::ProtoTuple* tuple3 = context->newTupleFromList(list3);
    ASSERT_NE(tuple1, tuple3);
}

TEST_F(TupleTest, GetSlice) {
    const proto::ProtoList* list = context->newList();
    for (int i = 0; i < 10; ++i) {
        list = list->appendLast(context, context->fromInteger(i));
    }
    const proto::ProtoTuple* tuple = context->newTupleFromList(list);

    const proto::ProtoTuple* slice = (const proto::ProtoTuple*)tuple->getSlice(context, 2, 5);
    ASSERT_EQ(slice->getSize(context), 3);
    ASSERT_EQ(slice->getAt(context, 0)->asLong(context), 2);
    ASSERT_EQ(slice->getAt(context, 1)->asLong(context), 3);
    ASSERT_EQ(slice->getAt(context, 2)->asLong(context), 4);

    // Test that the slice is also interned correctly
    const proto::ProtoTuple* slice2 = (const proto::ProtoTuple*)tuple->getSlice(context, 2, 5);
    ASSERT_EQ(slice, slice2);
}

TEST_F(TupleTest, EmptyTupleGetFirstGetLast) {
    const proto::ProtoTuple* tuple = context->newTuple();
    ASSERT_EQ(tuple->getSize(context), 0);
    ASSERT_EQ(tuple->getFirst(context), PROTO_NONE);
    ASSERT_EQ(tuple->getLast(context), PROTO_NONE);
}

TEST_F(TupleTest, TupleHas) {
    const proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromInteger(10));
    list = list->appendLast(context, context->fromInteger(20));
    const proto::ProtoTuple* tuple = context->newTupleFromList(list);
    const proto::ProtoObject* ten = context->fromInteger(10);
    const proto::ProtoObject* twenty = context->fromInteger(20);
    const proto::ProtoObject* ninety = context->fromInteger(90);
    ASSERT_TRUE(tuple->has(context, ten));
    ASSERT_TRUE(tuple->has(context, twenty));
    ASSERT_FALSE(tuple->has(context, ninety));
}
