#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

class SetTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space;
    proto::ProtoContext* context;

    void SetUp() override {
        space = new proto::ProtoSpace();
        context = new proto::ProtoContext(space);
    }

    void TearDown() override {
        delete context;
        delete space;
    }
};

TEST_F(SetTest, CreateEmptySet) {
    const proto::ProtoSet* set = context->newSet();
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->getSize(context), 0);
}

TEST_F(SetTest, AddAndHas) {
    const proto::ProtoSet* set = context->newSet();
    const proto::ProtoObject* val1 = context->fromInteger(10);
    const proto::ProtoObject* val2 = context->fromInteger(20);

    set = set->add(context, val1);
    ASSERT_EQ(set->getSize(context), 1);
    ASSERT_TRUE(set->has(context, val1)->asBoolean(context));
    ASSERT_FALSE(set->has(context, val2)->asBoolean(context));

    set = set->add(context, val2);
    ASSERT_EQ(set->getSize(context), 2);
    ASSERT_TRUE(set->has(context, val2)->asBoolean(context));
}

TEST_F(SetTest, AddExisting) {
    const proto::ProtoSet* set = context->newSet();
    const proto::ProtoObject* val1 = context->fromInteger(10);

    set = set->add(context, val1);
    ASSERT_EQ(set->getSize(context), 1);

    set = set->add(context, val1);
    ASSERT_EQ(set->getSize(context), 1);
}

TEST_F(SetTest, Remove) {
    const proto::ProtoSet* set = context->newSet();
    const proto::ProtoObject* val1 = context->fromInteger(10);
    const proto::ProtoObject* val2 = context->fromInteger(20);

    set = set->add(context, val1)->add(context, val2);
    ASSERT_EQ(set->getSize(context), 2);

    set = set->remove(context, val1);
    ASSERT_EQ(set->getSize(context), 1);
    ASSERT_FALSE(set->has(context, val1)->asBoolean(context));
    ASSERT_TRUE(set->has(context, val2)->asBoolean(context));
}

TEST_F(SetTest, RemoveNonExistent) {
    const proto::ProtoSet* set = context->newSet();
    const proto::ProtoObject* val1 = context->fromInteger(10);
    const proto::ProtoObject* val2 = context->fromInteger(20);

    set = set->add(context, val1);
    ASSERT_EQ(set->getSize(context), 1);

    set = set->remove(context, val2);
    ASSERT_EQ(set->getSize(context), 1);
}

// --- Multiset Tests ---

class MultisetTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space;
    proto::ProtoContext* context;

    void SetUp() override {
        space = new proto::ProtoSpace();
        context = new proto::ProtoContext(space);
    }

    void TearDown() override {
        delete context;
        delete space;
    }
};

TEST_F(MultisetTest, CreateEmptyMultiset) {
    const proto::ProtoMultiset* multiset = context->newMultiset();
    ASSERT_NE(multiset, nullptr);
    ASSERT_EQ(multiset->getSize(context), 0);
}

TEST_F(MultisetTest, AddAndCount) {
    const proto::ProtoMultiset* multiset = context->newMultiset();
    const proto::ProtoObject* val1 = context->fromInteger(10);
    const proto::ProtoObject* val2 = context->fromInteger(20);

    multiset = multiset->add(context, val1);
    ASSERT_EQ(multiset->getSize(context), 1);
    ASSERT_EQ(multiset->count(context, val1)->asLong(context), 1);
    ASSERT_EQ(multiset->count(context, val2)->asLong(context), 0);

    multiset = multiset->add(context, val1);
    ASSERT_EQ(multiset->getSize(context), 2);
    ASSERT_EQ(multiset->count(context, val1)->asLong(context), 2);
}

TEST_F(MultisetTest, Remove) {
    const proto::ProtoMultiset* multiset = context->newMultiset();
    const proto::ProtoObject* val1 = context->fromInteger(10);

    multiset = multiset->add(context, val1)->add(context, val1);
    ASSERT_EQ(multiset->getSize(context), 2);
    ASSERT_EQ(multiset->count(context, val1)->asLong(context), 2);

    multiset = multiset->remove(context, val1);
    ASSERT_EQ(multiset->getSize(context), 1);
    ASSERT_EQ(multiset->count(context, val1)->asLong(context), 1);

    multiset = multiset->remove(context, val1);
    ASSERT_EQ(multiset->getSize(context), 0);
    ASSERT_EQ(multiset->count(context, val1)->asLong(context), 0);
}

TEST_F(MultisetTest, RemoveNonExistent) {
    const proto::ProtoMultiset* multiset = context->newMultiset();
    const proto::ProtoObject* val1 = context->fromInteger(10);
    const proto::ProtoObject* val2 = context->fromInteger(20);

    multiset = multiset->add(context, val1);
    ASSERT_EQ(multiset->getSize(context), 1);

    multiset = multiset->remove(context, val2);
    ASSERT_EQ(multiset->getSize(context), 1);
}
