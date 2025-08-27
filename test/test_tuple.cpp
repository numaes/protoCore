#include <gtest/gtest.h>
#include "../headers/proto.h"

// Test fixture for ProtoTuple tests
class TupleTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space;
    proto::ProtoContext* context;

    void SetUp() override {
        space = new proto::ProtoSpace(nullptr, 0, nullptr);
        context = new proto::ProtoContext(nullptr, nullptr, 0, nullptr, space);
    }

    void TearDown() override {
        delete context;
        delete space;
    }
};

TEST_F(TupleTest, CreationAndSize) {
    proto::ProtoTuple* tuple = context->newTuple();
    ASSERT_TRUE(tuple != nullptr);
    ASSERT_EQ(tuple->getSize(context), 0);
}

TEST_F(TupleTest, CreationFromList) {
    proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromInteger(10));
    list = list->appendLast(context, context->fromInteger(20));

    proto::ProtoTuple* tuple = context->newTupleFromList(list);
    ASSERT_EQ(tuple->getSize(context), 2);
    ASSERT_EQ(tuple->getAt(context, 0)->asInteger(context), 10);
    ASSERT_EQ(tuple->getAt(context, 1)->asInteger(context), 20);
}

TEST_F(TupleTest, GetAt) {
    proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromInteger(10));
    list = list->appendLast(context, context->fromInteger(20));
    proto::ProtoTuple* tuple = context->newTupleFromList(list);

    ASSERT_EQ(tuple->getAt(context, 0)->asInteger(context), 10);
    ASSERT_EQ(tuple->getAt(context, 1)->asInteger(context), 20);
}

TEST_F(TupleTest, Interning) {
    // Create two separate lists with the same content
    proto::ProtoList* list1 = context->newList()->appendLast(context, context->fromInteger(1));
    list1 = list1->appendLast(context, context->fromInteger(2));

    proto::ProtoList* list2 = context->newList()->appendLast(context, context->fromInteger(1));
    list2 = list2->appendLast(context, context->fromInteger(2));

    // Create tuples from them
    proto::ProtoTuple* tuple1 = context->newTupleFromList(list1);
    proto::ProtoTuple* tuple2 = context->newTupleFromList(list2);

    // Because of interning, they should be the exact same object in memory
    ASSERT_EQ(tuple1, tuple2);

    // A different tuple should be a different object
    proto::ProtoList* list3 = context->newList()->appendLast(context, context->fromInteger(99));
    proto::ProtoTuple* tuple3 = context->newTupleFromList(list3);
    ASSERT_NE(tuple1, tuple3);
}

TEST_F(TupleTest, GetSlice) {
    proto::ProtoList* list = context->newList();
    for (int i = 0; i < 10; ++i) {
        list = list->appendLast(context, context->fromInteger(i));
    }
    proto::ProtoTuple* tuple = context->newTupleFromList(list);

    proto::ProtoTuple* slice = (proto::ProtoTuple*)tuple->getSlice(context, 2, 5);
    ASSERT_EQ(slice->getSize(context), 3);
    ASSERT_EQ(slice->getAt(context, 0)->asInteger(context), 2);
    ASSERT_EQ(slice->getAt(context, 1)->asInteger(context), 3);
    ASSERT_EQ(slice->getAt(context, 2)->asInteger(context), 4);

    // Test that the slice is also interned correctly
    proto::ProtoTuple* slice2 = (proto::ProtoTuple*)tuple->getSlice(context, 2, 5);
    ASSERT_EQ(slice, slice2);
}
