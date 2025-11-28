#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include <limits>

using namespace proto;

class NumericTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space;
    proto::ProtoContext* context;

    void SetUp() override {
        space = new proto::ProtoSpace();
        context = space->rootContext;
    }

    void TearDown() override {
        delete space;
    }
};

// --- Integer Tests ---

TEST_F(NumericTest, SmallIntegerCreation) {
    const proto::ProtoObject* i = context->fromLong(123);
    ASSERT_TRUE(i->isInteger(context));
    ASSERT_FALSE(i->isDouble(context));
    ASSERT_EQ(i->asLong(context), 123);
}

TEST_F(NumericTest, LargeIntegerCreation) {
    long long large_val = (1LL << 55); // Guaranteed to be a LargeInteger
    const proto::ProtoObject* li = context->fromLong(large_val);
    ASSERT_TRUE(li->isInteger(context));
    ASSERT_EQ(li->asLong(context), large_val);

    const proto::ProtoObject* neg_li = context->fromLong(-large_val);
    ASSERT_TRUE(neg_li->isInteger(context));
    ASSERT_EQ(neg_li->asLong(context), -large_val);
}

TEST_F(NumericTest, IntegerAddition) {
    const proto::ProtoObject* a = context->fromLong(100);
    const proto::ProtoObject* b = context->fromLong(200);
    const proto::ProtoObject* result = a->add(context, b);
    ASSERT_EQ(result->asLong(context), 300);
}

TEST_F(NumericTest, SmallToLargeOverflow) {
    long long val = (1LL << 55) - 1;
    const proto::ProtoObject* a = context->fromLong(val);
    const proto::ProtoObject* b = context->fromLong(1);
    
    ASSERT_TRUE(a->isInteger(context)); // Should be SmallInteger
    
    const proto::ProtoObject* result = a->add(context, b);
    ASSERT_TRUE(result->isInteger(context)); // Should be LargeInteger
    ASSERT_EQ(result->asLong(context), val + 1);
}

TEST_F(NumericTest, LargeToSmallUnderflow) {
    long long val = (1LL << 55);
    const proto::ProtoObject* a = context->fromLong(val);
    const proto::ProtoObject* b = context->fromLong(1);

    ASSERT_TRUE(a->isInteger(context)); // Should be LargeInteger

    const proto::ProtoObject* result = a->subtract(context, b);
    ASSERT_TRUE(result->isInteger(context)); // Should be SmallInteger
    ASSERT_EQ(result->asLong(context), val - 1);
}

TEST_F(NumericTest, IntegerCompare) {
    const proto::ProtoObject* small_a = context->fromLong(10);
    const proto::ProtoObject* small_b = context->fromLong(20);
    const proto::ProtoObject* large_a = context->fromLong(1LL << 60);
    const proto::ProtoObject* large_b = context->fromLong(1LL << 61);

    ASSERT_EQ(small_a->compare(context, small_b), -1);
    ASSERT_EQ(small_b->compare(context, small_a), 1);
    ASSERT_EQ(large_a->compare(context, large_a), 0);
    
    // Critical: comparing Small vs Large
    ASSERT_EQ(small_a->compare(context, large_a), -1);
    ASSERT_EQ(large_a->compare(context, small_a), 1);
}


// --- Double Tests ---

TEST_F(NumericTest, DoubleCreation) {
    const proto::ProtoObject* d = context->fromFloat(123.45);
    ASSERT_TRUE(d->isDouble(context));
    ASSERT_FALSE(d->isInteger(context));
    ASSERT_DOUBLE_EQ(d->asDouble(context), 123.45);
}

TEST_F(NumericTest, DoubleAddition) {
    const proto::ProtoObject* a = context->fromFloat(1.5);
    const proto::ProtoObject* b = context->fromFloat(2.5);
    const proto::ProtoObject* result = a->add(context, b);
    ASSERT_TRUE(result->isDouble(context));
    ASSERT_DOUBLE_EQ(result->asDouble(context), 4.0);
}


// --- Mixed-Type Arithmetic ---

TEST_F(NumericTest, IntegerAndDoubleAddition) {
    const proto::ProtoObject* i = context->fromLong(10);
    const proto::ProtoObject* d = context->fromFloat(2.5);

    // Integer + Double
    const proto::ProtoObject* result1 = i->add(context, d);
    ASSERT_TRUE(result1->isDouble(context));
    ASSERT_DOUBLE_EQ(result1->asDouble(context), 12.5);

    // Double + Integer
    const proto::ProtoObject* result2 = d->add(context, i);
    ASSERT_TRUE(result2->isDouble(context));
    ASSERT_DOUBLE_EQ(result2->asDouble(context), 12.5);
}

TEST_F(NumericTest, MixedTypeCompare) {
    const proto::ProtoObject* i = context->fromLong(10);
    const proto::ProtoObject* d_less = context->fromFloat(9.5);
    const proto::ProtoObject* d_equal = context->fromFloat(10.0);
    const proto::Proto_object* d_more = context->fromFloat(10.5);

    ASSERT_EQ(i->compare(context, d_less), 1);
    ASSERT_EQ(i->compare(context, d_equal), 0);
    ASSERT_EQ(i->compare(context, d_more), -1);
}
