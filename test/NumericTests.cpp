#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include <cmath>
#include <limits>
#include <sstream>
#include <bitset>
#include <stdexcept>
#include <string>

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
    const long long max_small_int = (1LL << 53) - 1; // Corrected to 53 bits for signed 54-bit field
    const long long min_small_int = -(1LL << 53);    // Corrected to 53 bits for signed 54-bit field
    
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
    const proto::ProtoObject* d = context->fromDouble(123.45);
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
    ASSERT_TRUE(result->isInteger(context));
    ASSERT_EQ(result->asLong(context), 300);

    // SmallInt + SmallInt -> LargeInt (Overflow)
    const long long max_small_int = (1LL << 53) - 1; // Corrected to 53 bits for signed 54-bit field
    const proto::ProtoObject* c = context->fromLong(max_small_int);
    const proto::ProtoObject* d = context->fromLong(1);
    const proto::ProtoObject* overflow_result = c->add(context, d);
    ASSERT_TRUE(overflow_result->isInteger(context));
    ASSERT_EQ(overflow_result->asLong(context), max_small_int + 1);

    // LargeInt - SmallInt -> SmallInt (Underflow)
    const proto::ProtoObject* underflow_result = overflow_result->subtract(context, d);
    ASSERT_TRUE(underflow_result->isInteger(context));
    ASSERT_EQ(underflow_result->asLong(context), max_small_int);
}

TEST_F(NumericTest, LargeIntegerArithmetic) {
    const proto::ProtoObject* a = context->fromLong(1LL << 60);
    const proto::ProtoObject* b = context->fromLong((1LL << 60) + 1);

    // Multiplication
    const proto::ProtoObject* prod = a->multiply(context, b);
    // We can't easily verify the exact value, but we can check properties
    ASSERT_TRUE(prod->isInteger(context));
    // Check sign is positive
    ASSERT_FALSE(prod->compare(context, context->fromLong(0)) < 0);

    // Division
    const proto::ProtoObject* quot = b->divide(context, a);
    ASSERT_EQ(quot->asLong(context), 1);

    // Modulo
    const proto::ProtoObject* rem = b->modulo(context, a);
    ASSERT_EQ(rem->asLong(context), 1);
}

TEST_F(NumericTest, MixedTypeArithmetic) {
    const proto::ProtoObject* i = context->fromLong(10);
    const proto::ProtoObject* d = context->fromDouble(2.5);

    // Integer + Double
    const proto::ProtoObject* result1 = i->add(context, d);
    ASSERT_TRUE(result1->isDouble(context));
    ASSERT_DOUBLE_EQ(result1->asDouble(context), 12.5);

    // Double + Integer
    const proto::ProtoObject* result2 = d->add(context, i);
    ASSERT_TRUE(result2->isDouble(context));
    ASSERT_DOUBLE_EQ(result2->asDouble(context), 12.5);
}

TEST_F(NumericTest, LargeIntegerHashCoversAllDigits) {
    // Hashing only the lowest 64-bit digit gave every multiple of 2^64 the
    // same hash, so hash-keyed structures kept only one of 2^64, 2^65, 2^70.
    const proto::ProtoObject* two64 = context->fromLong(1LL << 60)->multiply(context, context->fromLong(16));
    const proto::ProtoObject* two65 = two64->multiply(context, context->fromLong(2));
    const proto::ProtoObject* two70 = two64->multiply(context, context->fromLong(64));
    EXPECT_NE(two64->getHash(context), two65->getHash(context));
    EXPECT_NE(two65->getHash(context), two70->getHash(context));
    // Equal values built differently hash equally.
    EXPECT_EQ(context->fromString("18446744073709551616", 10)->getHash(context), two64->getHash(context));
    EXPECT_EQ(context->fromString("1180591620717411303424", 10)->getHash(context), two70->getHash(context));
}

TEST_F(NumericTest, AsDoubleOfLargeInteger) {
    // asDouble went through asLong and threw for integers beyond long long.
    const proto::ProtoObject* two70 = context->fromString("1180591620717411303424", 10);
    EXPECT_DOUBLE_EQ(two70->asDouble(context), std::ldexp(1.0, 70));
    const proto::ProtoObject* negTwo64 = context->fromString("-18446744073709551616", 10);
    EXPECT_DOUBLE_EQ(negTwo64->asDouble(context), -std::ldexp(1.0, 64));
}

TEST_F(NumericTest, CompareIntegerWithDoubleIsExact) {
    // Integer vs double used to convert the integer to double: it threw
    // beyond long long and rounded above 2^53.
    const proto::ProtoObject* two70 = context->fromString("1180591620717411303424", 10);
    const proto::ProtoObject* two70plus1 = context->fromString("1180591620717411303425", 10);
    const proto::ProtoObject* d70 = context->fromDouble(std::ldexp(1.0, 70));
    EXPECT_EQ(two70->compare(context, d70), 0);
    EXPECT_EQ(d70->compare(context, two70), 0);
    EXPECT_EQ(two70plus1->compare(context, d70), 1);
    EXPECT_EQ(d70->compare(context, two70plus1), -1);

    const proto::ProtoObject* two53plus1 = context->fromLong((1LL << 53) + 1);
    EXPECT_EQ(two53plus1->compare(context, context->fromDouble(std::ldexp(1.0, 53))), 1);

    EXPECT_EQ(context->fromLong(3)->compare(context, context->fromDouble(3.5)), -1);
    EXPECT_EQ(context->fromLong(4)->compare(context, context->fromDouble(3.5)), 1);
    EXPECT_EQ(context->fromLong(-4)->compare(context, context->fromDouble(-3.5)), -1);
    EXPECT_EQ(context->fromLong(3)->compare(context, context->fromDouble(3.0)), 0);
    EXPECT_EQ(two70->compare(context, context->fromDouble(std::numeric_limits<double>::infinity())), -1);
    EXPECT_EQ(two70->compare(context, context->fromDouble(-std::numeric_limits<double>::infinity())), 1);
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
    ASSERT_EQ(n4->bitwiseAnd(context, n7)->asLong(context), -8);
    
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

// Helper to get UTF8 string for testing, as ProtoString doesn't expose it directly.
const char* get_utf8(proto::ProtoContext* c, const proto::ProtoString* s) {
    // This is a bit of a hack for testing purposes.
    // It converts the ProtoString to a ProtoList of characters,
    // then builds a std::string from the long value of each character.
    const proto::ProtoList* list = s->asList(c);
    std::string result;
    for (proto::proto_ulong i = 0; i < list->getSize(c); ++i) {
        result += static_cast<char>(list->getAt(c, i)->asLong(c));
    }
    // The string needs to be stored somewhere the pointer can reference.
    // A static variable is a simple way to do this for tests.
    static std::string static_str;
    static_str = result;
    return static_str.c_str();
}

const proto::ProtoString* to_string_in_base(proto::ProtoContext* c, const proto::ProtoObject* num, int base) {
    long long val = num->asLong(c);
    std::stringstream ss;
    if (base == 16) {
        ss << std::hex << val;
    } else if (base == 2) {
        std::string binary_str = std::bitset<64>(val).to_string();
        size_t first_one = binary_str.find('1');
        return c->fromUTF8String(first_one != std::string::npos ? binary_str.substr(first_one).c_str() : "0")->asString(c);
    } else {
        ss << std::dec << val;
    }
    return c->fromUTF8String(ss.str().c_str())->asString(c);
}

TEST_F(NumericTest, ToString) {
    const proto::ProtoObject* num = context->fromLong(255);
    ASSERT_STREQ(get_utf8(context, to_string_in_base(context, num, 10)), "255");
    ASSERT_STREQ(get_utf8(context, to_string_in_base(context, num, 16)), "ff");
    ASSERT_STREQ(get_utf8(context, to_string_in_base(context, num, 2)), "11111111");

    const proto::ProtoObject* neg_num = context->fromLong(-42);
    ASSERT_STREQ(get_utf8(context, to_string_in_base(context, neg_num, 10)), "-42");
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

// A double's hash must agree with equality: 0.0 and -0.0 compare equal, and
// every NaN is one set element whatever its sign or payload.  The hash used
// std::hash<double> on the bit pattern, so NaNs of different sign or payload
// (x86 0.0/0.0 is a negative NaN, std::nan("") a positive one) hashed apart.
TEST_F(NumericTest, DoubleHashAgreesWithEquality) {
    const double quiet = std::numeric_limits<double>::quiet_NaN();
    const double values[] = {
        quiet, -quiet, std::copysign(quiet, -1.0), std::nan("1"), std::nan("0x7ffff"),
        -std::nan("12345"),
    };
    const proto::proto_ulong nanHash = context->fromDouble(quiet)->getHash(context);
    for (double v : values) {
        ASSERT_TRUE(std::isnan(v));
        EXPECT_EQ(context->fromDouble(v)->getHash(context), nanHash);
    }
    EXPECT_EQ(context->fromDouble(-0.0)->getHash(context), context->fromDouble(0.0)->getHash(context));
    EXPECT_EQ(context->fromDouble(1.5)->getHash(context), context->fromDouble(1.5)->getHash(context));
    EXPECT_NE(context->fromDouble(1.5)->getHash(context), nanHash);

    const ProtoSet* set = context->newSet()
        ->add(context, context->fromDouble(-0.0))
        ->add(context, context->fromDouble(std::copysign(quiet, -1.0)));
    EXPECT_TRUE(set->has(context, context->fromDouble(0.0))->asBoolean(context));
    EXPECT_TRUE(set->has(context, context->fromDouble(std::nan("7")))->asBoolean(context));
    EXPECT_EQ(set->add(context, context->fromDouble(quiet))->getSize(context), 2u);
}

// --- Arbitrary-precision results checked against reference values ---
//
// Each expected value was computed with Python's arbitrary-precision
// integers (truncating division, remainder with the dividend's sign,
// two's-complement bitwise operations).  The cases are the smallest
// failures a differential run against Python found:
//   * division by a divisor of two or more 64-bit words answered a wrong
//     quotient when the quotient needed more than one word of shifts
//     (2^140 / 2^70 gave 2^65 - 1);
//   * and/or/xor lost the sign extension of a negative LargeInteger
//     operand, so -1 | x answered 2^64 - 1;
//   * shiftLeft of a negative SmallInteger overflowed 64 bits silently.

namespace {

const proto::ProtoObject* integerFromDecimal(proto::ProtoContext* c, const std::string& text) {
    const bool negative = text[0] == '-';
    const proto::ProtoObject* value = c->fromLong(0);
    const proto::ProtoObject* ten = c->fromLong(10);
    for (size_t i = negative ? 1 : 0; i < text.size(); ++i)
        value = value->multiply(c, ten)->add(c, c->fromLong(text[i] - '0'));
    return negative ? c->fromLong(0)->subtract(c, value) : value;
}

std::string decimalOf(proto::ProtoContext* c, const proto::ProtoObject* value) {
    std::string out;
    value->asIntegerString(c, 10)->toUTF8String(c, out);
    return out;
}

struct ReferenceCase { const char* op; const char* left; const char* right; const char* expected; };

const ReferenceCase kReferenceCases[] = {
    {"div", "1393796574908163946345982392040522594123776", "1180591620717411303424", "1180591620717411303424"},
    {"div", "1119732000052112050245306781229232592828783795427572907473838", "24472835931805791705", "45754076199925298930140939934848764746878"},
    {"mod", "-1290403823332970199439001116237131073989879508999553744134169", "34377772333454995692", "-2456115007569960341"},
    {"div", "-1606938044258990275541962092341162602522202993782792835313721", "18446744073709551617", "-87112285931760246641901533019663016919296"},
    {"mod", "1606938044258990275541962092341162602522202993782792835313721", "-18446744073709551617", "12089"},
    {"div", "340282366920938463463374607431768211455", "18446744073709551615", "18446744073709551617"},
    {"mod", "515377520732011331036461129765621272702107522001", "9094947017729282379150390625", "7617714795878974575886818876"},
    {"and", "-43641587748514744", "-1", "-43641587748514744"},
    {"or", "13937581705629591", "-1", "-1"},
    {"xor", "1", "-53412493823008955", "-53412493823008956"},
    {"and", "-1180591620717411303424", "1180591620717411303423", "0"},
    {"and", "-1180591620717411303429", "1267650600228229401496703205383", "1267650600228229401496703205379"},
    {"or", "-1361129467683753853853498429727072845824", "18446744073709551616", "-1361129467683753853835051685653363294208"},
    {"xor", "-1237940039285380274899124225", "-18446744073709551616", "1237940057732124348608675839"},
    {"shl", "-7410793187882849", "35", "-254632915035011359474450432"},
    {"shl", "7410793187882849", "35", "254632915035011359474450432"},
    {"shl", "-1", "63", "-9223372036854775808"},
    {"shl", "-4503599627370496", "2", "-18014398509481984"},
    {"shl", "3", "60", "3458764513820540928"},
};

} // namespace

TEST_F(NumericTest, ArbitraryPrecisionMatchesReferenceValues) {
    for (const ReferenceCase& k : kReferenceCases) {
        const proto::ProtoObject* x = integerFromDecimal(context, k.left);
        const proto::ProtoObject* y = integerFromDecimal(context, k.right);
        const std::string op = k.op;
        const proto::ProtoObject* r =
            op == "div" ? x->divide(context, y) :
            op == "mod" ? x->modulo(context, y) :
            op == "and" ? x->bitwiseAnd(context, y) :
            op == "or"  ? x->bitwiseOr(context, y) :
            op == "xor" ? x->bitwiseXor(context, y) :
                          x->shiftLeft(context, static_cast<int>(y->asLong(context)));
        EXPECT_EQ(decimalOf(context, r), k.expected) << k.left << " " << k.op << " " << k.right;
    }
}
