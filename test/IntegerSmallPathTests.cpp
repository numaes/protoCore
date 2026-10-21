// IntegerSmallPathTests.cpp — exact results of the integer operations on
// SmallIntegers, at the edges of the SmallInteger range.
//
// compare, sign, negate, abs, divide and modulo have a path for two
// SmallIntegers that does not build the sign-and-magnitude temporary (a
// std::vector) of the general path (2.14.6; test/int_alloc checks that it
// does not allocate).  These cases pin the results both paths must agree
// on: the order across SmallInteger and LargeInteger, truncating division
// with the remainder's sign following the dividend, promotion of
// -(-2^53) and -2^53 / -1 to a LargeInteger, and division by zero.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <compare>
#include <stdexcept>
#include <vector>

using namespace proto;

namespace {

constexpr long long kMin = -(1LL << 53);      // smallest SmallInteger
constexpr long long kMax = (1LL << 53) - 1;   // largest SmallInteger

class IntegerSmallPathTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* context = nullptr;
    void SetUp() override {
        space = new ProtoSpace();
        context = space->rootContext;
    }
    void TearDown() override { delete space; }

    const ProtoObject* i(long long v) { return context->fromLong(v); }
    const ProtoObject* big(const char* digits) { return context->fromString(digits, 10); }
    bool isSmall(const ProtoObject* o) {
        ProtoObjectPointer p{};
        p.oid = o;
        return p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
               p.op.embedded_type == EMBEDDED_TYPE_SMALLINT;
    }
    void expectEqualInteger(const ProtoObject* got, const ProtoObject* want, const char* what) {
        EXPECT_EQ(got->compare(context, want), 0) << what;
        EXPECT_EQ(isSmall(got), isSmall(want)) << what << ": representation";
    }
};

}  // namespace

TEST_F(IntegerSmallPathTest, OrderAcrossTheSmallIntegerEdges) {
    // Ascending, SmallIntegers and LargeIntegers mixed.
    const std::vector<const ProtoObject*> ordered = {
        big("-18446744073709551616"), big("-9007199254740993"), i(kMin), i(kMin + 1),
        i(-2), i(-1), i(0), i(1), i(2), i(kMax - 1), i(kMax),
        big("9007199254740992"), big("18446744073709551616"),
    };
    ASSERT_TRUE(isSmall(ordered[2]));
    ASSERT_FALSE(isSmall(ordered[1]));
    ASSERT_FALSE(isSmall(ordered[11]));
    for (size_t a = 0; a < ordered.size(); ++a) {
        for (size_t b = 0; b < ordered.size(); ++b) {
            const int want = a < b ? -1 : (a > b ? 1 : 0);
            EXPECT_EQ(ordered[a]->compare(context, ordered[b]), want) << a << " vs " << b;
            const std::partial_ordering o = ordered[a]->partialCompare(context, ordered[b]);
            EXPECT_EQ(o < 0 ? -1 : (o > 0 ? 1 : 0), want) << a << " vs " << b << " (partial)";
        }
    }
}

TEST_F(IntegerSmallPathTest, SignNegateAbs) {
    EXPECT_EQ(i(-5)->integerSign(context), -1);
    EXPECT_EQ(i(0)->integerSign(context), 0);
    EXPECT_EQ(i(5)->integerSign(context), 1);
    EXPECT_EQ(i(kMin)->integerSign(context), -1);
    EXPECT_EQ(big("-9007199254740993")->integerSign(context), -1);

    expectEqualInteger(i(7)->negate(context), i(-7), "negate 7");
    expectEqualInteger(i(0)->negate(context), i(0), "negate 0");
    expectEqualInteger(i(kMax)->negate(context), i(-kMax), "negate max");
    // -(-2^53) = 2^53 does not fit: a LargeInteger.
    expectEqualInteger(i(kMin)->negate(context), big("9007199254740992"), "negate min");

    expectEqualInteger(i(-7)->abs(context), i(7), "abs -7");
    expectEqualInteger(i(7)->abs(context), i(7), "abs 7");
    expectEqualInteger(i(kMin)->abs(context), big("9007199254740992"), "abs min");
}

TEST_F(IntegerSmallPathTest, DivideAndModuloTruncate) {
    const long long values[] = {-1000003, -7, -3, -1, 1, 3, 7, 1000003, kMin, kMin + 1, kMax};
    for (long long a : values) {
        for (long long b : values) {
            const ProtoObject* q = i(a)->divide(context, i(b));
            const ProtoObject* r = i(a)->modulo(context, i(b));
            if (a == kMin && b == -1) continue;  // checked below
            EXPECT_EQ(q->asLong(context), a / b) << a << " / " << b;
            EXPECT_EQ(r->asLong(context), a % b) << a << " % " << b;
        }
    }
    // The one SmallInteger quotient outside the range: 2^53.
    expectEqualInteger(i(kMin)->divide(context, i(-1)), big("9007199254740992"), "min / -1");
    expectEqualInteger(i(kMin)->modulo(context, i(-1)), i(0), "min % -1");
    // Zero dividend.
    expectEqualInteger(i(0)->divide(context, i(-5)), i(0), "0 / -5");
    expectEqualInteger(i(0)->modulo(context, i(-5)), i(0), "0 % -5");
}

TEST_F(IntegerSmallPathTest, DivisionByZeroThrows) {
    EXPECT_THROW(i(5)->divide(context, i(0)), proto::ProtoException);
    EXPECT_THROW(i(5)->modulo(context, i(0)), proto::ProtoException);
    EXPECT_THROW(i(0)->divide(context, i(0)), proto::ProtoException);
}

TEST_F(IntegerSmallPathTest, MixedOperandsKeepTheGeneralPath) {
    const ProtoObject* b = big("100000000000000000000");   // 10^20
    expectEqualInteger(b->divide(context, i(10)), big("10000000000000000000"), "10^20 / 10");
    expectEqualInteger(b->modulo(context, i(7)), i(2), "10^20 % 7");   // 10^20 = 7k + 2
    expectEqualInteger(i(7)->divide(context, b), i(0), "7 / 10^20");
    expectEqualInteger(i(-7)->modulo(context, b), i(-7), "-7 % 10^20");
    EXPECT_EQ(i(kMax)->compare(context, b), -1);
    EXPECT_EQ(b->compare(context, i(kMin)), 1);
}
