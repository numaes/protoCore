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

TEST_F(ListTest, EmptyListGetFirstGetLast) {
    const proto::ProtoList* list = context->newList();
    ASSERT_EQ(list->getSize(context), 0);
    ASSERT_EQ(list->getFirst(context), PROTO_NONE);
    ASSERT_EQ(list->getLast(context), PROTO_NONE);
}

TEST_F(ListTest, ListIteratorExhaustion) {
    const proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromInteger(1));
    const proto::ProtoListIterator* it = list->getIterator(context);
    ASSERT_TRUE(it->hasNext(context));
    ASSERT_NE(it->next(context), nullptr);
    it = it->advance(context);
    ASSERT_FALSE(it->hasNext(context));
}

// has() compares integer elements by value.  It converted both integers with
// asLong, which throws std::overflow_error for a LargeInteger beyond long
// long, so has() threw on any list holding such an element (or when asked for
// one) instead of answering.
TEST_F(ListTest, HasComparesLargeIntegersByValue) {
    const ProtoObject* two70 = context->fromString("1180591620717411303424", 10);
    const ProtoObject* sameValue = context->fromString("1180591620717411303424", 10);
    const ProtoObject* two70plus1 = context->fromString("1180591620717411303425", 10);
    const ProtoObject* negTwo70 = context->fromString("-1180591620717411303424", 10);
    ASSERT_NE(two70, sameValue) << "the test needs two distinct objects of equal value";

    // Inline form (up to five elements) and AVL form (more than five).
    const ProtoList* small = context->newList()
        ->appendLast(context, context->fromInteger(7))
        ->appendLast(context, two70);
    const ProtoList* avl = small;
    for (int i = 0; i < 8; ++i) avl = avl->appendLast(context, context->fromInteger(100 + i));

    for (const ProtoList* list : {small, avl}) {
        EXPECT_TRUE(list->has(context, sameValue));
        EXPECT_FALSE(list->has(context, two70plus1));
        EXPECT_FALSE(list->has(context, negTwo70));
        EXPECT_TRUE(list->has(context, context->fromInteger(7)));
        EXPECT_FALSE(list->has(context, context->fromInteger(8)));
    }
    // A LargeInteger that fits in long long still equals the SmallInteger.
    const ProtoList* withSmall = context->newList()->appendLast(context, context->fromInteger(42));
    EXPECT_TRUE(withSmall->has(context, context->fromLong(42)));
    EXPECT_FALSE(withSmall->has(context, two70));
}
