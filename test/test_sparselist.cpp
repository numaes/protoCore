#include <gtest/gtest.h>
#include "../headers/proto.h"

// Test fixture for ProtoSparseList tests
class SparseListTest : public ::testing::Test {
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

TEST_F(SparseListTest, CreationAndSize) {
    proto::ProtoSparseList* sl = context->newSparseList();
    ASSERT_TRUE(sl != nullptr);
    ASSERT_EQ(sl->getSize(context), 0);
}

TEST_F(SparseListTest, SetAndGet) {
    proto::ProtoSparseList* sl = context->newSparseList();
    proto::ProtoObject* val1 = context->fromInteger(100);
    unsigned long key1 = 12345;

    sl = sl->setAt(context, key1, val1);
    ASSERT_EQ(sl->getSize(context), 1);
    ASSERT_TRUE(sl->has(context, key1));
    ASSERT_EQ(sl->getAt(context, key1), val1);
}

TEST_F(SparseListTest, UseAsStringKeys) {
    proto::ProtoSparseList* dict = context->newSparseList();
    proto::ProtoString* key1 = context->fromUTF8String("first_name");
    proto::ProtoString* key2 = context->fromUTF8String("last_name");

    unsigned long key1_hash = key1->getObjectHash(context);
    unsigned long key2_hash = key2->getObjectHash(context);

    dict = dict->setAt(context, key1_hash, context->fromUTF8String("John"));
    dict = dict->setAt(context, key2_hash, context->fromUTF8String("Doe"));

    ASSERT_EQ(dict->getSize(context), 2);
    ASSERT_TRUE(dict->has(context, key1_hash));
    ASSERT_FALSE(dict->has(context, context->fromUTF8String("age")->getObjectHash(context)));

    proto::ProtoString* retrieved = dict->getAt(context, key1_hash)->asString(context);
    ASSERT_EQ(retrieved->cmp_to_string(context, context->fromUTF8String("John")), 0);
}

TEST_F(SparseListTest, ImmutabilityOnSet) {
    proto::ProtoSparseList* sl1 = context->newSparseList();
    proto::ProtoSparseList* sl2 = sl1->setAt(context, 1, context->fromInteger(1));

    ASSERT_EQ(sl1->getSize(context), 0);
    ASSERT_EQ(sl2->getSize(context), 1);
    ASSERT_NE(sl1, sl2);
}

TEST_F(SparseListTest, RemoveAt) {
    proto::ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 10, context->fromInteger(100));
    sl = sl->setAt(context, 20, context->fromInteger(200));

    proto::ProtoSparseList* modified_sl = sl->removeAt(context, 10);

    // Original is unchanged
    ASSERT_EQ(sl->getSize(context), 2);
    ASSERT_TRUE(sl->has(context, 10));

    // Modified has the key removed
    ASSERT_EQ(modified_sl->getSize(context), 1);
    ASSERT_FALSE(modified_sl->has(context, 10));
    ASSERT_TRUE(modified_sl->has(context, 20));
}
