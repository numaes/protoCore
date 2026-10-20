// SmallIntegerFastPathTests.cpp — ProtoObject::isInteger / asLong and
// ProtoContext::fromInteger / fromLong test for a SmallInteger first, in the
// function itself (3.0.0).  These pin that they answer what they answered
// before, at and across the boundaries of the 54-bit SmallInteger range.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

using namespace proto;

namespace {
constexpr long long kMax = (1LL << 53) - 1;   // the SmallInteger range
constexpr long long kMin = -(1LL << 53);
}

TEST(SmallIntegerFastPath, RoundTripsAcrossTheRange) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const long long values[] = {0, 1, -1, 42, -42, 1023, -1024, kMax, kMin, kMax - 1, kMin + 1};
    for (long long v : values) {
        const ProtoObject* o = c->fromLong(v);
        EXPECT_TRUE(o->isInteger(c)) << v;
        EXPECT_EQ(o->asLong(c), v);
        EXPECT_EQ(c->fromInteger(v), o) << "fromInteger and fromLong make the same embedded word";
        EXPECT_FALSE(ProtoObject::isCellPointer(o)) << "a SmallInteger is not a cell: " << v;
    }
}

TEST(SmallIntegerFastPath, OutsideTheRangeIsALargeInteger) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const long long values[] = {kMax + 1, kMin - 1, (1LL << 62), -(1LL << 62), 9223372036854775807LL};
    for (long long v : values) {
        const ProtoObject* o = c->fromLong(v);
        EXPECT_TRUE(ProtoObject::isCellPointer(o)) << "a LargeInteger cell: " << v;
        EXPECT_TRUE(o->isInteger(c)) << v;
        EXPECT_EQ(o->asLong(c), v);
        EXPECT_EQ(c->fromInteger(v)->asLong(c), v);
    }
}

TEST(SmallIntegerFastPath, NonIntegersTakeTheSlowPath) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    EXPECT_FALSE(PROTO_TRUE->isInteger(c));
    EXPECT_FALSE(PROTO_NONE->isInteger(c));
    EXPECT_FALSE(c->fromDouble(1.5)->isInteger(c));
    EXPECT_EQ(c->fromUnicodeChar('A')->asLong(c), 65) << "a character's code point, as before";
}
