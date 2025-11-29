#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include <limits>
#include <stdexcept>

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

// --- Creation, Conversion, and Boundary Tests ---

TEST_F(NumericTest, CreationAndConversion) {
    // Test SmallInteger range
    const proto::ProtoObject* i = context->fromLong(12345);
    ASSERT_TRUE(i->isInteger(context));
    ASSERT_FALSE(i->isDouble(context));
    ASSERT_EQ(i->asLong(context), 12345);

    // Test exact boundaries of SmallInteger
    const long long max_small_int = (1LL << 55) - 1;
    const long long min_small_int = -(1LL << 55);
    
    const proto::ProtoObject* max_si = context->fromLong(max_small_int);
    ASSERT_TRUE(max_si->isInteger(context));
    ASSERT_EQ(max_si->asLong(context), max_small_int);

    const proto::ProtoObject* min_si = context->fromLong(min_small_int);
    ASSERT_TRUE(min_si->isInteger(context));
    ASSERT_EQ(min_si->asLong(context), min_small_int);

    // Test just outside SmallInteger range (should create LargeInteger)
    const proto::ProtoObject* large_pos = context->fromLong(max_small_int + 1);
    ASSERT_TRUE(large_pos->isInteger(context));
    ASSERT_EQ(large_pos->asLong(context), max_small_int + 1);

    const proto::ProtoObject* large_neg = context->fromLong(min_small_int - 1);
    ASSERT_TRUE(large_neg->isInteger(context));
    ASSERT_EQ(large_neg->asLong(context), min_small_int - 1);

    // Test asLong exception for out-of-range LargeInteger
    const proto::ProtoObject* too_large = context->fromLong(1LL << 60)->multiply(context, context->fromLong(1LL << 60));
    ASSERT_THROW(too_large->asLong(context), std::overflow_error);
}

TEST_F(NumericTest, DoubleCreationAndConversion) {
    const proto::ProtoObject* d = context->fromFloat(123.45);
    ASSERT_TRUE(d->isDouble(context));
    ASSERT_FALSE(d->isInteger(context));
    ASSERT_DOUBLE_EQ(d->asDouble(context), 123.45);

    // Test conversion from integer to double
    const proto::ProtoObject* i = context->fromLong(10);
    ASSERT_DOUBLE_EQ(i->asDouble(context), 10.0);
}

// --- Arithmetic Tests ---

TEST_F(NumericTest, FastPathArithmetic) {
    // SmallInt + SmallInt -> SmallInt
    const proto::ProtoObject* a = context->fromLong(100);
    const proto::ProtoObject* b = context->fromLong(200);
    const proto::ProtoObject* result = a->add(context, b);
    ASSERT_TRUE(isSmallInteger(result));
    ASSERT_EQ(result->asLong(context), 300);

    // SmallInt + SmallInt -> LargeInt (Overflow)
    const long long max_small_int = (1LL << 55) - 1;
    const proto::ProtoObject* c = context->fromLong(max_small_int);
    const proto::ProtoObject* d = context->fromLong(1);
    const proto::ProtoObject* overflow_result = c->add(context, d);
    ASSERT_TRUE(isLargeInteger(overflow_result));
    ASSERT_EQ(overflow_result->asLong(context), max_small_int + 1);

    // LargeInt - SmallInt -> SmallInt (Underflow)
    const proto::ProtoObject* underflow_result = overflow_result->subtract(context, d);
    ASSERT_TRUE(isSmallInteger(underflow_result));
    ASSERT_EQ(underflow_result->asLong(context), max_small_int);
}

TEST_F(NumericTest, LargeIntegerArithmetic) {
    const proto::ProtoObject* a = context->fromLong(1LL << 60);
    const proto::ProtoObject* b = context->fromLong((1LL << 60) + 1);

    // Multiplication
    const proto::ProtoObject* prod = a->multiply(context, b);
    // We can't easily verify the exact value, but we can check properties
    ASSERT_TRUE(prod->isInteger(context));
    ASSERT_EQ(prod->sign(context), 1);

    // Division
    const proto::ProtoObject* quot = b->divide(context, a);
    ASSERT_EQ(quot->asLong(context), 1);

    // Modulo
    const proto::ProtoObject* rem = b->modulo(context, a);
    ASSERT_EQ(rem->asLong(context), 1);
}

TEST_F(NumericTest, MixedTypeArithmetic) {
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

TEST_F(NumericTest, DivisionAndErrorHandling) {
    const proto::ProtoObject* a = context->fromLong(10);
    const proto::ProtoObject* b = context->fromLong(3);
    const proto::ProtoObject* neg_a = context->fromLong(-10);
    const proto::ProtoObject* zero = context->fromLong(0);

    ASSERT_EQ(a->divide(context, b)->asLong(context), 3);
    ASSERT_EQ(a->modulo(context, b)->asLong(context), 1);

    // Test negative division (remainder sign follows dividend)
    ASSERT_EQ(neg_a->divide(context, b)->asLong(context), -3);
    ASSERT_EQ(neg_a->modulo(context, b)->asLong(context), -1);

    // Test division by zero
    ASSERT_THROW(a->divide(context, zero), std::runtime_error);
    ASSERT_THROW(a->modulo(context, zero), std::runtime_error);
}

// --- Bitwise and Shift Tests ---

TEST_F(NumericTest, BitwiseNot) {
    const proto::ProtoObject* a = context->fromLong(5); // ...0101
    const proto::ProtoObject* not_a = a->bitwiseNot(context);
    ASSERT_EQ(not_a->asLong(context), -6); // ...1010
}

TEST_F(NumericTest, BitwiseOperations) {
    const proto::ProtoObject* p6 = context->fromLong(6);   // ...0110
    const proto::ProtoObject* p10 = context->fromLong(10); // ...1010
    const proto::ProtoObject* n4 = context->fromLong(-4);  // ...1100 (in two's complement)
    const proto::ProtoObject* n7 = context->fromLong(-7);  // ...1001 (in two's complement)

    // Positive & Positive
    ASSERT_EQ(p6->bitwiseAnd(context, p10)->asLong(context), 2); // ...0010

    // Positive & Negative
    ASSERT_EQ(p6->bitwiseAnd(context, n4)->asLong(context), 4); // ...0100

    // Negative & Negative
    ASSERT_EQ(n4->bitwiseAnd(context, n7)->asLong(context), -8); // ...1000
    
    // OR operations
    ASSERT_EQ(p6->bitwiseOr(context, p10)->asLong(context), 14); // ...1110
    ASSERT_EQ(p6->bitwiseOr(context, n4)->asLong(context), -2);  // ...1110
    ASSERT_EQ(n4->bitwiseOr(context, n7)->asLong(context), -3);  // ...1101
}

TEST_F(NumericTest, ShiftOperations) {
    const proto::ProtoObject* p = context->fromLong(100); // 01100100
    const proto::ProtoObject* n = context->fromLong(-100);

    // Left shift
    ASSERT_EQ(p->shiftLeft(context, 2)->asLong(context), 400);
    ASSERT_EQ(n->shiftLeft(context, 2)->asLong(context), -400);

    // Right shift (positive)
    ASSERT_EQ(p->shiftRight(context, 2)->asLong(context), 25);

    // Right shift (negative, arithmetic, rounds to -inf)
    const proto::ProtoObject* neg9 = context->fromLong(-9);
    ASSERT_EQ(neg9->shiftRight(context, 1)->asLong(context), -5); // floor(-4.5)
}

// --- Other Functionality ---

TEST_F(NumericTest, ToString) {
    const proto::ProtoObject* num = context->fromLong(255);
    
    ASSERT_STREQ(num->toString(context, 10)->asUTF8(context), "255");
    ASSERT_STREQ(num->toString(context, 16)->asUTF8(context), "ff");
    ASSERT_STREQ(num->toString(context, 2)->asUTF8(context), "11111111");

    const proto::ProtoObject* neg_num = context->fromLong(-42);
    ASSERT_STREQ(neg_num->toString(context, 10)->asUTF8(context), "-42");
}

TEST_F(NumericTest, DivmodApi) {
    const proto::ProtoObject* a = context->fromLong(10);
    const proto::ProtoObject* b = context->fromLong(3);
    const proto::ProtoObject* result = a->divmod(context, b);

    ASSERT_TRUE(result->isTuple(context));
    const proto::ProtoTuple* tuple = result->asTuple(context);
    ASSERT_EQ(tuple->getSize(context), 2);
    ASSERT_EQ(tuple->getAt(context, 0)->asLong(context), 3); // Quotient
    ASSERT_EQ(tuple->getAt(context, 1)->asLong(context), 1); // Remainder
}
