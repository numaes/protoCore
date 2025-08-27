#include <gtest/gtest.h>
#include "../headers/proto.h"

// Test fixture for ProtoList tests
class ListTest : public ::testing::Test {
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

TEST_F(ListTest, CreationAndSize) {
    proto::ProtoList* list = context->newList();
    ASSERT_TRUE(list != nullptr);
    ASSERT_EQ(list->getSize(context), 0);
}

TEST_F(ListTest, AppendAndGet) {
    proto::ProtoList* list = context->newList();
    proto::ProtoObject* val1 = context->fromInteger(10);
    proto::ProtoObject* val2 = context->fromInteger(20);

    list = list->appendLast(context, val1);
    ASSERT_EQ(list->getSize(context), 1);
    ASSERT_EQ(list->getFirst(context), val1);
    ASSERT_EQ(list->getLast(context), val1);

    list = list->appendLast(context, val2);
    ASSERT_EQ(list->getSize(context), 2);
    ASSERT_EQ(list->getAt(context, 0), val1);
    ASSERT_EQ(list->getAt(context, 1), val2);
    ASSERT_EQ(list->getLast(context), val2);
}

TEST_F(ListTest, ImmutabilityOnAppend) {
    proto::ProtoList* list1 = context->newList();
    proto::ProtoObject* val1 = context->fromInteger(10);
    proto::ProtoList* list2 = list1->appendLast(context, val1);

    // The original list must remain unchanged
    ASSERT_EQ(list1->getSize(context), 0);
    // The new list has the new element
    ASSERT_EQ(list2->getSize(context), 1);
    // They must be different objects
    ASSERT_NE(list1, list2);
}

TEST_F(ListTest, RemoveAt) {
    proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromInteger(10));
    list = list->appendLast(context, context->fromInteger(20));
    list = list->appendLast(context, context->fromInteger(30));

    proto::ProtoList* modified_list = list->removeAt(context, 1);

    // Original list is unchanged
    ASSERT_EQ(list->getSize(context), 3);
    ASSERT_EQ(list->getAt(context, 1)->asInteger(context), 20);

    // Modified list has the element removed
    ASSERT_EQ(modified_list->getSize(context), 2);
    ASSERT_EQ(modified_list->getAt(context, 0)->asInteger(context), 10);
    ASSERT_EQ(modified_list->getAt(context, 1)->asInteger(context), 30);
}

TEST_F(ListTest, GetSlice) {
    proto::ProtoList* list = context->newList();
    for (int i = 0; i < 10; ++i) {
        list = list->appendLast(context, context->fromInteger(i));
    }

    proto::ProtoList* slice = list->getSlice(context, 2, 5);
    ASSERT_EQ(slice->getSize(context), 3);
    ASSERT_EQ(slice->getAt(context, 0)->asInteger(context), 2);
    ASSERT_EQ(slice->getAt(context, 1)->asInteger(context), 3);
    ASSERT_EQ(slice->getAt(context, 2)->asInteger(context), 4);
}
