/*
 * WideArith.h - portable 64-bit digit arithmetic for the bignum code.
 *
 * The bignum routines in Integer.cpp work on 64-bit digits and need four
 * primitives that a 64-bit integer type alone cannot express:
 *
 *   mul64x64   the full 128-bit product of two digits, as (hi, lo)
 *   div128by64 a 128-bit value (hi, lo) divided by one digit, with hi < d
 *   addCarry   digit + digit + carry, returning the carry out
 *   subBorrow  digit - digit - borrow, returning the borrow out
 *
 * GCC and Clang provide unsigned __int128, which compiles each of them to one
 * or two instructions.  MSVC has no 128-bit type; on x64 it has the _umul128
 * and _udiv128 intrinsics (the MUL and DIV instructions), and on ARM64 the
 * __umulh intrinsic.  Everything else falls back to plain 32-bit half-digit
 * arithmetic (Hacker's Delight, 2nd ed., 8-2 for the product and 9-4 "divlu"
 * for the quotient), which is also what an MSVC ARM64 build divides with.
 *
 * protoCore once used MSVC's internal std::_Unsigned128 class for this
 * (<__msvc_int128.hpp>, an implementation detail of <ranges> that Microsoft
 * may change at any time).  These helpers replace it, and
 * test/WideArithTests.cpp checks every path against known values.
 *
 * Internal to protoCore: not installed and not part of the API.
 */

#ifndef PROTOCORE_WIDE_ARITH_H
#define PROTOCORE_WIDE_ARITH_H

#include <cstdint>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

// PROTOCORE_WIDE_ARITH_FORCE_PORTABLE selects the portable code everywhere, so
// that the whole bignum suite can be run on it from any platform:
//   cmake -DCMAKE_CXX_FLAGS=-DPROTOCORE_WIDE_ARITH_FORCE_PORTABLE ...
#if defined(PROTOCORE_WIDE_ARITH_FORCE_PORTABLE)
// neither fast path
#elif defined(__SIZEOF_INT128__)
#define PROTOCORE_WIDE_ARITH_INT128 1
#elif defined(_MSC_VER) && defined(_M_X64)
#define PROTOCORE_WIDE_ARITH_MSVC_X64 1
#endif

namespace proto::wide {

    using u64 = std::uint64_t;

    /** mul64x64 on 32-bit halves only.  Always compiled, so it is tested on
     *  every platform even where the fast path is the one in use. */
    inline u64 mul64x64Portable(u64 a, u64 b, u64* hi) {
        const u64 aLo = a & 0xFFFFFFFFu, aHi = a >> 32;
        const u64 bLo = b & 0xFFFFFFFFu, bHi = b >> 32;
        const u64 ll = aLo * bLo;
        const u64 lh = aLo * bHi;
        const u64 hl = aHi * bLo;
        const u64 hh = aHi * bHi;
        // The middle column can carry: sum it in 64 bits from 32-bit parts.
        const u64 mid = (ll >> 32) + (lh & 0xFFFFFFFFu) + (hl & 0xFFFFFFFFu);
        *hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
        return (mid << 32) | (ll & 0xFFFFFFFFu);
    }

    /** The 128-bit product a * b: returns the low digit, stores the high one. */
    inline u64 mul64x64(u64 a, u64 b, u64* hi) {
#if defined(PROTOCORE_WIDE_ARITH_INT128)
        const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
        *hi = static_cast<u64>(p >> 64);
        return static_cast<u64>(p);
#elif defined(PROTOCORE_WIDE_ARITH_MSVC_X64)
        return _umul128(a, b, hi);
#elif defined(_MSC_VER) && defined(_M_ARM64) && !defined(PROTOCORE_WIDE_ARITH_FORCE_PORTABLE)
        *hi = __umulh(a, b);
        return a * b;
#else
        return mul64x64Portable(a, b, hi);
#endif
    }

    /**
     * (hi * 2^64 + lo) / d on 32-bit halves only, Hacker's Delight 9-4.
     * Precondition: hi < d (so the quotient fits in one digit; d != 0).
     */
    inline u64 div128by64Portable(u64 hi, u64 lo, u64 d, u64* rem) {
        constexpr u64 b = u64{1} << 32;
        // Normalise so the divisor's top bit is set.
        int s = 0;
        while ((d << s) >> 63 == 0) ++s;
        d <<= s;
        const u64 un32 = s == 0 ? hi : (hi << s) | (lo >> (64 - s));
        const u64 un10 = lo << s;
        const u64 vn1 = d >> 32, vn0 = d & 0xFFFFFFFFu;
        const u64 un1 = un10 >> 32, un0 = un10 & 0xFFFFFFFFu;

        u64 q1 = un32 / vn1;
        u64 rhat = un32 - q1 * vn1;
        while (q1 >= b || q1 * vn0 > b * rhat + un1) {
            --q1;
            rhat += vn1;
            if (rhat >= b) break;
        }
        const u64 un21 = un32 * b + un1 - q1 * d;

        u64 q0 = un21 / vn1;
        rhat = un21 - q0 * vn1;
        while (q0 >= b || q0 * vn0 > b * rhat + un0) {
            --q0;
            rhat += vn1;
            if (rhat >= b) break;
        }
        *rem = (un21 * b + un0 - q0 * d) >> s;
        return q1 * b + q0;
    }

    /** (hi * 2^64 + lo) / d.  Precondition: hi < d.  Stores the remainder. */
    inline u64 div128by64(u64 hi, u64 lo, u64 d, u64* rem) {
#if defined(PROTOCORE_WIDE_ARITH_INT128)
        const unsigned __int128 n = (static_cast<unsigned __int128>(hi) << 64) | lo;
        *rem = static_cast<u64>(n % d);
        return static_cast<u64>(n / d);
#elif defined(PROTOCORE_WIDE_ARITH_MSVC_X64)
        return _udiv128(hi, lo, d, rem);
#else
        return div128by64Portable(hi, lo, d, rem);
#endif
    }

    /** a + b + carryIn (carryIn is 0 or 1): returns the sum digit. */
    inline u64 addCarry(u64 a, u64 b, u64 carryIn, u64* carryOut) {
        const u64 s = a + b;
        const u64 c1 = s < a;
        const u64 t = s + carryIn;
        const u64 c2 = t < s;
        *carryOut = c1 | c2;
        return t;
    }

    /** a - b - borrowIn (borrowIn is 0 or 1): returns the difference digit. */
    inline u64 subBorrow(u64 a, u64 b, u64 borrowIn, u64* borrowOut) {
        const u64 d = a - b;
        const u64 b1 = a < b;
        const u64 t = d - borrowIn;
        const u64 b2 = d < borrowIn;
        *borrowOut = b1 | b2;
        return t;
    }

} // namespace proto::wide

#endif // PROTOCORE_WIDE_ARITH_H
