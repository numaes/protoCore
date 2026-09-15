// LargeIntegerRangeTests.cpp — values at and around the 64-bit digit boundaries.
//
// A LargeInteger stores LargeIntegerImplementation::DIGIT_COUNT 64-bit digits
// per cell, chained through `next`.  Integer::asLong used to reject only
// `next != nullptr` or `digits[1] != 0`, so a value whose only non-zero digit
// was digits[2] or digits[3] passed the check and was returned as digits[0]:
// 2^128, 2^192 and 2^200 all read back as 0 instead of overflowing.  2^64
// (digits[1]) and 2^256 (a second chunk) were rejected correctly, which is why
// the bug showed only in that window.
//
// These tests cover 2^(64k) for k = 1..8 with the values on either side, both
// signs, a round trip through asIntegerString in base 10 and base 16, and the
// exact boundary of what asLong may return.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

#include <limits>
#include <stdexcept>
#include <string>

using namespace proto;

namespace {

std::string text(ProtoContext* ctx, const ProtoObject* value, int base) {
    std::string out;
    const ProtoString* s = value->asIntegerString(ctx, base);
    if (s) s->toUTF8String(ctx, out);
    return out;
}

// Decimal and hexadecimal spelling of 2^(64k) + delta, computed here rather
// than taken from protoCore, by long multiplication on decimal digits.
std::string powerOfTwoDecimal(int exponent, int delta) {
    std::string digits = "1";
    for (int i = 0; i < exponent; ++i) {
        int carry = 0;
        for (int d = static_cast<int>(digits.size()) - 1; d >= 0; --d) {
            const int doubled = (digits[d] - '0') * 2 + carry;
            digits[d] = static_cast<char>('0' + doubled % 10);
            carry = doubled / 10;
        }
        if (carry) digits.insert(digits.begin(), static_cast<char>('0' + carry));
    }
    if (delta == 0) return digits;
    // delta is +1 or -1 and the value is a power of two, so no special cases
    // beyond a single borrow or carry chain.
    int index = static_cast<int>(digits.size()) - 1;
    if (delta > 0) {
        while (index >= 0 && digits[index] == '9') { digits[index] = '0'; --index; }
        if (index < 0) digits.insert(digits.begin(), '1');
        else digits[index] = static_cast<char>(digits[index] + 1);
    } else {
        while (index >= 0 && digits[index] == '0') { digits[index] = '9'; --index; }
        digits[index] = static_cast<char>(digits[index] - 1);
        if (digits.size() > 1 && digits[0] == '0') digits.erase(digits.begin());
    }
    return digits;
}

std::string hexOfPowerOfTwo(int exponent, int delta) {
    // 2^(64k) is 1 followed by 16k hex zeros; +-1 only touches the tail.
    const int zeros = exponent / 4;
    if (delta == 0) return "1" + std::string(zeros, '0');
    if (delta > 0) return "1" + std::string(zeros - 1, '0') + "1";
    return std::string(zeros, 'f');
}

}  // namespace

TEST(LargeIntegerRange, PowersOfTwoRoundTripInBothBases) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    for (int k = 1; k <= 8; ++k) {
        const int exponent = 64 * k;
        for (int delta : {-1, 0, 1}) {
            for (int sign : {1, -1}) {
                const std::string magnitude = powerOfTwoDecimal(exponent, delta);
                const std::string decimal = (sign < 0 ? "-" : "") + magnitude;
                const std::string hexadecimal =
                    (sign < 0 ? "-" : "") + hexOfPowerOfTwo(exponent, delta);

                const ProtoObject* fromDecimal = ctx->fromString(decimal.c_str(), 10);
                const ProtoObject* fromHex = ctx->fromString(hexadecimal.c_str(), 16);
                ASSERT_NE(fromDecimal, nullptr) << decimal;
                ASSERT_NE(fromHex, nullptr) << hexadecimal;

                EXPECT_EQ(text(ctx, fromDecimal, 10), decimal);
                EXPECT_EQ(text(ctx, fromDecimal, 16), hexadecimal);
                EXPECT_EQ(text(ctx, fromHex, 10), decimal) << "base 16 -> base 10: " << hexadecimal;
            }
        }
    }
}

// asLong must throw for every value outside long long, including those whose
// only non-zero digit sits above digits[1] within one chunk.
TEST(LargeIntegerRange, AsLongOverflowsAboveTheFirstDigit) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    for (int k = 1; k <= 8; ++k) {
        const int exponent = 64 * k;
        for (int delta : {-1, 0, 1}) {
            for (int sign : {1, -1}) {
                if (k == 1 && delta == -1) continue;   // 2^64 - 1 handled below
                const std::string decimal =
                    (sign < 0 ? "-" : "") + powerOfTwoDecimal(exponent, delta);
                const ProtoObject* value = ctx->fromString(decimal.c_str(), 10);
                EXPECT_THROW((void) value->asLong(ctx), std::overflow_error)
                    << decimal << " must not be readable as a long long";
            }
        }
    }

    // 2^64 - 1 and -(2^64 - 1) are also outside long long.
    for (const char* spelling : {"18446744073709551615", "-18446744073709551615"}) {
        const ProtoObject* value = ctx->fromString(spelling, 10);
        EXPECT_THROW((void) value->asLong(ctx), std::overflow_error) << spelling;
    }

    // The largest values that do fit still read back exactly.
    const long long maxLong = std::numeric_limits<long long>::max();
    const ProtoObject* fitting = ctx->fromString(std::to_string(maxLong).c_str(), 10);
    EXPECT_EQ(fitting->asLong(ctx), maxLong);
    const ProtoObject* negative = ctx->fromString("-9223372036854775807", 10);
    EXPECT_EQ(negative->asLong(ctx), -9223372036854775807LL);
}

// The three values from the protoClojure report, spelled out.
TEST(LargeIntegerRange, ReportedValuesAreNotZero) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    struct Case { const char* decimal; int exponent; };
    const Case cases[] = {
        {"340282366920938463463374607431768211456", 128},
        {"6277101735386680763835789423207666416102355444464034512896", 192},
        {"1606938044258990275541962092341162602522202993782792835301376", 200},
    };
    for (const Case& c : cases) {
        const ProtoObject* value = ctx->fromString(c.decimal, 10);
        ASSERT_NE(value, nullptr);
        EXPECT_EQ(text(ctx, value, 10), c.decimal);
        EXPECT_NE(text(ctx, value, 10), "0");
        EXPECT_THROW((void) value->asLong(ctx), std::overflow_error)
            << c.decimal << " used to be returned as 0 by asLong";
        // Arithmetic on it still works: v / v == 1.
        const ProtoObject* quotient = value->divide(ctx, value);
        EXPECT_EQ(text(ctx, quotient, 10), "1");
    }
}
