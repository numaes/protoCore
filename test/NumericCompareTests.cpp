// NumericCompareTests.cpp — ProtoObject::partialCompare (IEEE partial order)
// and the container behaviour around NaN.
//
// partialCompare compares numbers by value across SmallInteger, LargeInteger
// and double, exactly.  Any NaN is unordered with everything, itself
// included, so <, <=, ==, >=, > against 0 are all false and != is true — the
// IEEE / Python / JavaScript semantics of the comparison operators.  Strings
// compare by content; any other pair is equivalent when identical and
// unordered otherwise.  compare() keeps its behaviour: for an unordered
// numeric pair it still returns 0.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

#include <cmath>
#include <compare>
#include <limits>
#include <string>
#include <vector>

using namespace proto;

namespace {

class NumericCompareTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* context = nullptr;
    void SetUp() override {
        space = new ProtoSpace();
        context = space->rootContext;
    }
    void TearDown() override { delete space; }

    const ProtoObject* d(double v) { return context->fromDouble(v); }
    const ProtoObject* i(long long v) { return context->fromLong(v); }
    const ProtoObject* big(const char* digits) { return context->fromString(digits, 10); }
};

void expectUnordered(std::partial_ordering o, const std::string& what) {
    EXPECT_TRUE(o == std::partial_ordering::unordered) << what;
    EXPECT_FALSE(o < 0) << what;
    EXPECT_FALSE(o <= 0) << what;
    EXPECT_FALSE(o == 0) << what;
    EXPECT_FALSE(o >= 0) << what;
    EXPECT_FALSE(o > 0) << what;
    EXPECT_TRUE(o != 0) << what;
}

int signOf(std::partial_ordering o) {
    if (o < 0) return -1;
    if (o > 0) return 1;
    return 0;
}

}  // namespace

TEST_F(NumericCompareTest, PartialCompareWithNaNIsUnordered) {
    const double inf = std::numeric_limits<double>::infinity();
    const double quiet = std::numeric_limits<double>::quiet_NaN();
    const ProtoObject* nan = d(quiet);
    const std::vector<const ProtoObject*> nans = {nan, d(std::nan("1")), d(std::copysign(quiet, -1.0))};
    const std::vector<const ProtoObject*> others = {
        nan, nans[1], nans[2],
        d(0.0), d(-0.0), d(1.5), d(inf), d(-inf),
        i(0), i(7), i(-1), i((1LL << 53) - 1),
        big("1180591620717411303424"), big("-1180591620717411303424"),
    };
    for (size_t a = 0; a < nans.size(); ++a) {
        for (size_t b = 0; b < others.size(); ++b) {
            const std::string what = "NaN #" + std::to_string(a) + " vs value #" + std::to_string(b);
            expectUnordered(nans[a]->partialCompare(context, others[b]), what + " (NaN left)");
            expectUnordered(others[b]->partialCompare(context, nans[a]), what + " (NaN right)");
            // compare() is unchanged by this commit: unordered still maps to 0.
            EXPECT_EQ(nans[a]->compare(context, others[b]), 0) << what;
        }
    }
}

TEST_F(NumericCompareTest, PartialCompareOrderedNumbersMatchCompare) {
    const double inf = std::numeric_limits<double>::infinity();
    struct Ranked { const ProtoObject* value; int rank; const char* name; };
    const std::vector<Ranked> values = {
        {d(-inf), 0, "-inf"},
        {big("-1180591620717411303424"), 1, "-2^70"},
        {i(-1), 2, "-1"},
        {d(-0.5), 3, "-0.5"},
        {d(-0.0), 4, "-0.0"},
        {i(0), 4, "SmallInteger 0"},
        {d(0.0), 4, "0.0"},
        {i(1), 5, "1"},
        {d(1.5), 6, "1.5"},
        {i((1LL << 53) + 1), 7, "2^53+1"},
        {big("1180591620717411303424"), 8, "2^70"},
        {d(inf), 9, "+inf"},
    };
    for (const Ranked& a : values) {
        for (const Ranked& b : values) {
            const std::partial_ordering o = a.value->partialCompare(context, b.value);
            const int expected = (a.rank > b.rank) - (a.rank < b.rank);
            const std::string what = std::string(a.name) + " vs " + b.name;
            EXPECT_FALSE(o == std::partial_ordering::unordered) << what;
            EXPECT_EQ(signOf(o), expected) << what;
            const int c = a.value->compare(context, b.value);
            EXPECT_EQ((c > 0) - (c < 0), expected) << what << " (compare)";
        }
    }

    // The exact integer/double cases of CompareIntegerWithDoubleIsExact.
    const ProtoObject* two70 = big("1180591620717411303424");
    const ProtoObject* two70plus1 = big("1180591620717411303425");
    const ProtoObject* d70 = d(std::ldexp(1.0, 70));
    EXPECT_TRUE(two70->partialCompare(context, d70) == 0);
    EXPECT_TRUE(d70->partialCompare(context, two70) == 0);
    EXPECT_TRUE(two70plus1->partialCompare(context, d70) > 0);
    EXPECT_TRUE(d70->partialCompare(context, two70plus1) < 0);
    EXPECT_TRUE(i((1LL << 53) + 1)->partialCompare(context, d(std::ldexp(1.0, 53))) > 0);
    EXPECT_TRUE(i(3)->partialCompare(context, d(3.5)) < 0);
    EXPECT_TRUE(i(4)->partialCompare(context, d(3.5)) > 0);
    EXPECT_TRUE(i(-4)->partialCompare(context, d(-3.5)) < 0);
    EXPECT_TRUE(i(3)->partialCompare(context, d(3.0)) == 0);
}

TEST_F(NumericCompareTest, PartialCompareNonNumeric) {
    const char* text = "a string long enough to live on the heap, not inline";
    const ProtoObject* s1 = context->fromUTF8String(text);
    const ProtoObject* s2 = context->fromUTF8String(text);
    EXPECT_TRUE(s1->partialCompare(context, s2) == 0) << "equal content";
    EXPECT_TRUE(context->fromUTF8String("a")->partialCompare(context, context->fromUTF8String("b")) < 0);
    EXPECT_TRUE(context->fromUTF8String("b")->partialCompare(context, context->fromUTF8String("a")) > 0);

    expectUnordered(s1->partialCompare(context, i(1)), "string vs integer");
    expectUnordered(i(1)->partialCompare(context, s1), "integer vs string");
    expectUnordered(s1->partialCompare(context, d(1.5)), "string vs double");

    const ProtoObject* o1 = context->newObject(false);
    const ProtoObject* o2 = context->newObject(false);
    EXPECT_TRUE(o1->partialCompare(context, o1) == 0) << "an object is equivalent to itself";
    expectUnordered(o1->partialCompare(context, o2), "two distinct objects");
    expectUnordered(o1->partialCompare(context, i(0)), "object vs integer");
    EXPECT_TRUE(PROTO_NONE->partialCompare(context, PROTO_NONE) == 0);
}

// protoCore's own containers never consult compare(): sets are keyed by hash
// and list/tuple has() matches doubles by identity.  These guards pin that a
// NaN element does not alias numbers there.
TEST_F(NumericCompareTest, NaNElementsDoNotAliasNumbersInContainers) {
    const ProtoObject* nan = d(std::numeric_limits<double>::quiet_NaN());

    const ProtoSet* set = context->newSet()->add(context, nan);
    EXPECT_TRUE(set->has(context, nan)->asBoolean(context));
    EXPECT_FALSE(set->has(context, d(0.0))->asBoolean(context));
    EXPECT_FALSE(set->has(context, i(0))->asBoolean(context));
    EXPECT_FALSE(set->has(context, d(1.5))->asBoolean(context));
    set = set->add(context, d(1.5))->add(context, i(7));
    EXPECT_EQ(set->getSize(context), 3u);
    set = set->remove(context, nan);
    EXPECT_EQ(set->getSize(context), 2u);
    EXPECT_TRUE(set->has(context, d(1.5))->asBoolean(context));
    EXPECT_TRUE(set->has(context, i(7))->asBoolean(context));

    const ProtoMultiset* bag = context->newMultiset()->add(context, nan)->add(context, nan)->add(context, d(1.5));
    EXPECT_EQ(bag->count(context, nan)->asLong(context), 2);
    EXPECT_EQ(bag->count(context, d(1.5))->asLong(context), 1);
    EXPECT_EQ(bag->count(context, i(0))->asLong(context), 0);

    const ProtoObject* onePointFive = d(1.5);
    const ProtoList* list = context->newList()->appendLast(context, onePointFive)->appendLast(context, nan);
    EXPECT_TRUE(list->has(context, nan));
    EXPECT_FALSE(list->has(context, i(7)));
    EXPECT_FALSE(list->has(context, d(2.5)));
    const ProtoTuple* tuple = context->newTuple({onePointFive, nan});
    EXPECT_TRUE(tuple->has(context, nan));
    EXPECT_FALSE(tuple->has(context, i(7)));
    EXPECT_FALSE(tuple->has(context, d(2.5)));
}
