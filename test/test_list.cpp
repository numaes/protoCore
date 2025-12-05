#include <gtest/gtest.h>
#include "../headers/protoCore.h"

using namespace proto;

class ListTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space;
    proto::ProtoContext* context;

    void SetUp() override {
        // Corrected: Use the default constructors
        space = new proto::ProtoSpace();
        context = space->rootContext;
    }

    void TearDown() override {
        delete space;
    }
};

TEST_F(ListTest, CreationAndSize) {
    // Corrected: Use const pointer
    const proto::ProtoList* list = context->newList();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->getSize(context), 0);
}

TEST_F(ListTest, AppendAndGet) {
    // Corrected: Use const pointers
    const proto::ProtoList* list = context->newList();
    const proto::ProtoObject* val1 = context->fromInteger(10);
    const proto::ProtoObject* val2 = context->fromInteger(20);

    list = list->appendLast(context, val1);
    ASSERT_EQ(list->getSize(context), 1);
    ASSERT_EQ(list->getAt(context, 0)->asLong(context), 10);

    list = list->appendLast(context, val2);
    ASSERT_EQ(list->getSize(context), 2);
    ASSERT_EQ(list->getAt(context, 1)->asLong(context), 20);
}

TEST_F(ListTest, ImmutabilityOnAppend) {
    // Corrected: Use const pointers
    const proto::ProtoList* list1 = context->newList();
    const proto::ProtoObject* val1 = context->fromInteger(10);
    const proto::ProtoList* list2 = list1->appendLast(context, val1);

    ASSERT_NE(list1, list2);
    ASSERT_EQ(list1->getSize(context), 0);
    ASSERT_EQ(list2->getSize(context), 1);
}

TEST_F(ListTest, RemoveAt) {
    // Corrected: Use const pointers
    const proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromInteger(10));
    list = list->appendLast(context, context->fromInteger(20));
    list = list->appendLast(context, context->fromInteger(30));

    const proto::ProtoList* modified_list = list->removeAt(context, 1);

    ASSERT_EQ(modified_list->getSize(context), 2);
    ASSERT_EQ(modified_list->getAt(context, 0)->asLong(context), 10);
    ASSERT_EQ(modified_list->getAt(context, 1)->asLong(context), 30);
    ASSERT_EQ(list->getSize(context), 3); // Original list is unchanged
}

TEST_F(ListTest, GetSlice) {
    // Corrected: Use const pointers
    const proto::ProtoList* list = context->newList();
    for (int i = 0; i < 10; ++i) {
        list = list->appendLast(context, context->fromInteger(i));
    }

    const proto::ProtoList* slice = list->getSlice(context, 2, 5);
    ASSERT_EQ(slice->getSize(context), 3);
    ASSERT_EQ(slice->getAt(context, 0)->asLong(context), 2);
    ASSERT_EQ(slice->getAt(context, 1)->asLong(context), 3);
    ASSERT_EQ(slice->getAt(context, 2)->asLong(context), 4);
}
