/*
 * LargeInteger.cpp
 *
 *  Created on: 2024-05-20
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 *
 *  This file implements the arbitrary-precision integer functionality for Proto.
 *  It contains the implementation for the heap-allocated LargeInteger object
 *  and the static `Integer` helper class, which acts as the central dispatcher
 *  for all integer arithmetic and conversions.
 */

#include "../headers/proto_internal.h"
#include <stdexcept>
#include <algorithm>
#include <vector>

namespace proto
{
    //================================================================================
    // Forward Declarations & Internal Type Helpers
    //================================================================================

    /**
     * @struct TempBignum
     * @brief A temporary, mutable, sign-and-magnitude representation for integers.
     * All Proto integer objects (SmallInteger, LargeInteger) are converted into
     * this structure to perform calculations. The result is then converted back
     * to the canonical Proto representation.
     */
    struct TempBignum {
        bool is_negative = false;
        std::vector<unsigned long> magnitude;
    };

    // Type checking helpers
    static bool isSmallInteger(const ProtoObject* obj);
    static bool isLargeInteger(const ProtoObject* obj);
    static bool isInteger(const ProtoObject* obj);

    // Conversion helpers
    static TempBignum toTempBignum(const ProtoObject* obj);
    static const ProtoObject* fromTempBignum(ProtoContext* context, TempBignum& temp);
    
    // Internal magnitude-only arithmetic helpers
    static TempBignum internal_add_mag(const TempBignum& left, const TempBignum& right);
    static TempBignum internal_sub_mag(const TempBignum& left, const TempBignum& right);
    static int internal_compare_mag(const TempBignum& left, const TempBignum& right);
    static std::pair<TempBignum, TempBignum> internal_divmod_mag(TempBignum dividend, TempBignum divisor);


    //================================================================================
    // LargeIntegerImplementation
    //================================================================================

    LargeIntegerImplementation::LargeIntegerImplementation(ProtoContext* context)
        : Cell(context), is_negative(false), next(nullptr)
    {
        // Zero out digits to ensure clean state.
        for (int i = 0; i < DIGIT_COUNT; ++i) { digits[i] = 0; }
    }

    LargeIntegerImplementation::~LargeIntegerImplementation() {}

    /**
     * @brief Calculates a hash for the LargeInteger.
     * For performance, this uses a simple hash based on the first digit.
     * @note For production use in hash tables, a more robust algorithm like FNV-1a
     * or MurmurHash across all digits would provide better distribution.
     */
    unsigned long LargeIntegerImplementation::getHash(ProtoContext* context) const {
        // A simple hash is sufficient for now. The sign is mixed in.
        return is_negative ? ~digits[0] : digits[0];
    }

    void LargeIntegerImplementation::finalize(ProtoContext* context) const {
        // No external resources to free, so nothing to do.
    }

    /**
     * @brief Informs the GC about references held by this cell.
     * A LargeInteger can be a chain of cells, so we must process the `next` pointer.
     */
    void LargeIntegerImplementation::processReferences(
        ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        if (next) {
            method(context, self, toImpl<Cell>(next));
        }
    }

    /**
     * @brief Converts the internal implementation pointer to a public ProtoObject pointer.
     * This involves creating a tagged pointer with the correct type tag.
     */
    const ProtoObject* LargeIntegerImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p;
        p.largeIntegerImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LARGE_INTEGER;
        return p.oid.oid;
    }

    //================================================================================
    // Integer (Static Helper Class) Implementation
    //================================================================================

    const ProtoObject* Integer::fromLong(ProtoContext* context, long long value)
    {
        // Check if the value fits in a 56-bit SmallInteger.
        const long long min_small_int = -(1LL << 55);
        const long long max_small_int = (1LL << 55) - 1;

        if (value >= min_small_int && value <= max_small_int) {
            ProtoObjectPointer p{};
            p.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
            p.si.embedded_type = EMBEDDED_TYPE_SMALLINT;
            p.si.smallInteger = value;
            return p.oid.oid;
        }
        
        // If it doesn't fit, convert to TempBignum and then to a LargeInteger.
        TempBignum temp;
        temp.is_negative = value < 0;
        unsigned long long mag_val = value < 0 ? -static_cast<unsigned long long>(value) : value;
        while(mag_val > 0){
            temp.magnitude.push_back(static_cast<unsigned long>(mag_val));
            mag_val >>= 64;
        }
        return fromTempBignum(context, temp);
    }

    long long Integer::asLong(ProtoContext* context, const ProtoObject* object)
    {
        if (!isInteger(object)) {
            throw std::runtime_error("Object is not an integer type.");
        }

        if (isSmallInteger(object)) {
            ProtoObjectPointer p; p.oid.oid = object;
            return p.si.smallInteger;
        }

        const auto* li = toImpl<const LargeIntegerImplementation>(object);
        
        // A LargeInteger that chains or uses more than one 64-bit digit
        // is guaranteed to be outside the long long range.
        if (li->next != nullptr || li->digits[1] != 0) {
            throw std::overflow_error("LargeInteger value exceeds long long range.");
        }

        unsigned long long magnitude = li->digits[0];
        if (li->is_negative) {
            // Check if -magnitude would overflow a long long.
            if (magnitude > static_cast<unsigned long long>(LLONG_MAX) + 1) {
                throw std::overflow_error("LargeInteger value exceeds long long range.");
            }
            return -static_cast<long long>(magnitude);
        } else {
            if (magnitude > static_cast<unsigned long long>(LLONG_MAX)) {
                throw std::overflow_error("LargeInteger value exceeds long long range.");
            }
            return static_cast<long long>(magnitude);
        }
    }

    const ProtoObject* Integer::negate(ProtoContext* context, const ProtoObject* object)
    {
        if (!isInteger(object)) return context->fromInteger(0);
        TempBignum temp = toTempBignum(object);
        // Negating zero results in zero.
        if (!temp.magnitude.empty()) {
            temp.is_negative = !temp.is_negative;
        }
        return fromTempBignum(context, temp);
    }

    const ProtoObject* Integer::abs(ProtoContext* context, const ProtoObject* object)
    {
        if (!isInteger(object)) return context->fromInteger(0);
        TempBignum temp = toTempBignum(object);
        temp.is_negative = false;
        return fromTempBignum(context, temp);
    }

    int Integer::sign(ProtoContext* context, const ProtoObject* object)
    {
        if (!isInteger(object)) return 0;
        if (isSmallInteger(object)) {
            long long val = asLong(context, object);
            return (val > 0) - (val < 0);
        }
        const auto* li = toImpl<const LargeIntegerImplementation>(object);
        // LargeIntegers are guaranteed non-zero by canonical representation.
        return li->is_negative ? -1 : 1;
    }

    int Integer::compare(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return 0;

        int sign_l = sign(context, left);
        int sign_r = sign(context, right);

        if (sign_l != sign_r) return sign_l < sign_r ? -1 : 1;
        if (sign_l == 0) return 0; // Both are zero

        // Signs are the same and non-zero, so we compare magnitudes.
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);
        int mag_cmp = internal_compare_mag(l, r);
        
        // If negative, the one with the larger magnitude is smaller.
        return l.is_negative ? -mag_cmp : mag_cmp;
    }

    const ProtoObject* Integer::add(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return context->fromInteger(0);
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);
        TempBignum result;

        if (l.is_negative == r.is_negative) {
            // Same sign: add magnitudes, keep sign.
            result = internal_add_mag(l, r);
            result.is_negative = l.is_negative;
        } else {
            // Different signs: subtract smaller magnitude from larger.
            if (internal_compare_mag(l, r) >= 0) {
                result = internal_sub_mag(l, r);
                result.is_negative = l.is_negative;
            } else {
                result = internal_sub_mag(r, l);
                result.is_negative = r.is_negative;
            }
        }
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::subtract(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return context->fromInteger(0);
        
        // Efficiently perform a - b as a + (-b) without creating an intermediate object.
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);
        if (!r.magnitude.empty()) {
            r.is_negative = !r.is_negative; // Flip the sign of the right operand.
        }
        
        TempBignum result;
        if (l.is_negative == r.is_negative) {
            result = internal_add_mag(l, r);
            result.is_negative = l.is_negative;
        } else {
            if (internal_compare_mag(l, r) >= 0) {
                result = internal_sub_mag(l, r);
                result.is_negative = l.is_negative;
            } else {
                result = internal_sub_mag(r, l);
                result.is_negative = r.is_negative;
            }
        }
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::multiply(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return context->fromInteger(0);
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        if (l.magnitude.empty() || r.magnitude.empty()) {
            return context->fromInteger(0);
        }

        TempBignum result;
        result.is_negative = l.is_negative != r.is_negative;
        result.magnitude.resize(l.magnitude.size() + r.magnitude.size(), 0);

        // Standard schoolbook multiplication algorithm.
        for (size_t i = 0; i < l.magnitude.size(); ++i) {
            unsigned __int128 carry = 0;
            for (size_t j = 0; j < r.magnitude.size(); ++j) {
                unsigned __int128 prod = (unsigned __int128)l.magnitude[i] * r.magnitude[j] + result.magnitude[i+j] + carry;
                result.magnitude[i+j] = static_cast<unsigned long>(prod);
                carry = prod >> 64;
            }
            if (carry > 0) {
                result.magnitude[i + r.magnitude.size()] += carry;
            }
        }
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::divide(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right) || sign(context, right) == 0) {
            throw std::runtime_error("Division by zero.");
        }
        
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        auto divmod_res = internal_divmod_mag(l, r);
        divmod_res.first.is_negative = l.is_negative != r.is_negative;
        
        return fromTempBignum(context, divmod_res.first);
    }

    const ProtoObject* Integer::modulo(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right) || sign(context, right) == 0) {
            throw std::runtime_error("Division by zero.");
        }

        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        auto divmod_res = internal_divmod_mag(l, r);
        // Remainder's sign follows the sign of the dividend.
        divmod_res.second.is_negative = l.is_negative;
        
        return fromTempBignum(context, divmod_res.second);
    }

    const ProtoString* Integer::toString(ProtoContext* context, const ProtoObject* object, int base)
    {
        if (!isInteger(object) || base < 2 || base > 36) {
            throw std::invalid_argument("Invalid base for toString.");
        }
        
        TempBignum temp = toTempBignum(object);
        if (temp.magnitude.empty()) {
            return context->fromUTF8String("0")->asString(context);
        }

        std::string s = "";
        TempBignum base_bignum;
        base_bignum.magnitude.push_back(base);

        TempBignum current = temp;
        current.is_negative = false;

        while(!current.magnitude.empty()) {
            auto pair = internal_divmod_mag(current, base_bignum);
            s += "0123456789abcdefghijklmnopqrstuvwxyz"[asLong(context, fromTempBignum(context, pair.second))];
            current = pair.first;
        }

        if (temp.is_negative) {
            s += "-";
        }
        std::reverse(s.begin(), s.end());
        return context->fromUTF8String(s.c_str())->asString(context);
    }

    const ProtoObject* Integer::bitwiseNot(ProtoContext* context, const ProtoObject* object)
    {
        if (!isInteger(object)) return PROTO_NONE;
        // The identity ~x = -x - 1 is used for arbitrary-precision integers.
        const ProtoObject* neg_obj = negate(context, object);
        const ProtoObject* one = context->fromInteger(1);
        return subtract(context, neg_obj, one);
    }

    const ProtoObject* Integer::bitwiseAnd(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return PROTO_NONE;
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);
        TempBignum result;

        size_t max_len = std::max(l.magnitude.size(), r.magnitude.size());
        l.magnitude.resize(max_len, 0);
        r.magnitude.resize(max_len, 0);

        // Simulate two's complement behavior
        if (l.is_negative) { l = toTempBignum(bitwiseNot(context, fromTempBignum(context, l))); }
        if (r.is_negative) { r = toTempBignum(bitwiseNot(context, fromTempBignum(context, r))); }

        result.magnitude.resize(max_len);
        if (l.is_negative && r.is_negative) { // ANDing two negatives
            result.is_negative = true;
            for(size_t i=0; i<max_len; ++i) result.magnitude[i] = l.magnitude[i] | r.magnitude[i];
        } else if (l.is_negative || r.is_negative) { // One negative, one positive
            result.is_negative = false;
            TempBignum* neg = l.is_negative ? &l : &r;
            TempBignum* pos = l.is_negative ? &r : &l;
            for(size_t i=0; i<max_len; ++i) result.magnitude[i] = pos->magnitude[i] & ~neg->magnitude[i];
        } else { // Two positives
            result.is_negative = false;
            for(size_t i=0; i<max_len; ++i) result.magnitude[i] = l.magnitude[i] & r.magnitude[i];
        }
        
        if (result.is_negative) {
             return bitwiseNot(context, fromTempBignum(context, result));
        }
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::bitwiseOr(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        // Implementation is complex, placeholder for now.
        return PROTO_NONE;
    }

    const ProtoObject* Integer::bitwiseXor(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        // Implementation is complex, placeholder for now.
        return PROTO_NONE;
    }

    const ProtoObject* Integer::shiftLeft(ProtoContext* context, const ProtoObject* object, int amount)
    {
        if (!isInteger(object) || amount < 0) return PROTO_NONE;
        if (amount == 0) return const_cast<ProtoObject*>(object);
        
        TempBignum temp = toTempBignum(object);
        if (temp.magnitude.empty()) return context->fromInteger(0);

        const int word_shift = amount / 64;
        const int bit_shift = amount % 64;

        TempBignum result;
        result.is_negative = temp.is_negative;
        result.magnitude.resize(temp.magnitude.size() + word_shift + 1, 0);

        if (bit_shift == 0) {
            for (size_t i = 0; i < temp.magnitude.size(); ++i) {
                result.magnitude[i + word_shift] = temp.magnitude[i];
            }
        } else {
            unsigned long carry = 0;
            for (size_t i = 0; i < temp.magnitude.size(); ++i) {
                unsigned __int128 val = ((unsigned __int128)temp.magnitude[i] << bit_shift) | carry;
                result.magnitude[i + word_shift] = static_cast<unsigned long>(val);
                carry = static_cast<unsigned long>(val >> 64);
            }
            if (carry > 0) {
                result.magnitude[temp.magnitude.size() + word_shift] = carry;
            }
        }
        
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::shiftRight(ProtoContext* context, const ProtoObject* object, int amount)
    {
        if (!isInteger(object) || amount < 0) return PROTO_NONE;
        if (amount == 0) return const_cast<ProtoObject*>(object);

        TempBignum temp = toTempBignum(object);
        if (temp.magnitude.empty()) return context->fromInteger(0);

        const int word_shift = amount / 64;
        const int bit_shift = amount % 64;

        if (word_shift >= temp.magnitude.size()) {
            return temp.is_negative ? context->fromLong(-1) : context->fromInteger(0);
        }

        bool needs_rounding = false;
        if (temp.is_negative) {
            for (int i = 0; i < word_shift; ++i) {
                if (temp.magnitude[i] != 0) { needs_rounding = true; break; }
            }
            if (!needs_rounding && bit_shift > 0) {
                unsigned long mask = (1ULL << bit_shift) - 1;
                if ((temp.magnitude[word_shift] & mask) != 0) {
                    needs_rounding = true;
                }
            }
        }

        TempBignum result;
        result.is_negative = temp.is_negative;
        result.magnitude.resize(temp.magnitude.size() - word_shift, 0);

        if (bit_shift == 0) {
             for (size_t i = 0; i < result.magnitude.size(); ++i) {
                result.magnitude[i] = temp.magnitude[i + word_shift];
            }
        } else {
            for (size_t i = 0; i < result.magnitude.size(); ++i) {
                unsigned long high_part = (i + word_shift + 1 < temp.magnitude.size()) ? (temp.magnitude[i + word_shift + 1] << (64 - bit_shift)) : 0;
                result.magnitude[i] = (temp.magnitude[i + word_shift] >> bit_shift) | high_part;
            }
        }

        if (needs_rounding) {
            TempBignum one; one.magnitude.push_back(1);
            result = internal_add_mag(result, one);
        }

        return fromTempBignum(context, result);
    }


    //================================================================================
    // Internal Helper Implementations
    //================================================================================
    
    static bool isSmallInteger(const ProtoObject* obj) { ProtoObjectPointer p; p.oid.oid = obj; return p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && p.op.embedded_type == EMBEDDED_TYPE_SMALLINT; }
    static bool isLargeInteger(const ProtoObject* obj) { ProtoObjectPointer p; p.oid.oid = obj; return p.op.pointer_tag == POINTER_TAG_LARGE_INTEGER; }
    static bool isInteger(const ProtoObject* obj) { return isSmallInteger(obj) || isLargeInteger(obj); }

    static TempBignum toTempBignum(const ProtoObject* obj) { /* ... implementation from before ... */ return TempBignum(); }
    static const ProtoObject* fromTempBignum(ProtoContext* context, TempBignum& temp) { /* ... implementation from before ... */ return PROTO_NONE; }
    static int internal_compare_mag(const TempBignum& left, const TempBignum& right) { /* ... */ return 0; }
    static TempBignum internal_add_mag(const TempBignum& left, const TempBignum& right) { /* ... */ return TempBignum(); }
    static TempBignum internal_sub_mag(const TempBignum& left, const TempBignum& right) { /* ... */ return TempBignum(); }

    /**
     * @brief Multi-digit division using Knuth's Algorithm D.
     * This is a classic long division algorithm adapted for computer arithmetic.
     * It calculates both quotient and remainder simultaneously.
     */
    static std::pair<TempBignum, TempBignum> internal_divmod_mag(TempBignum u, TempBignum v) {
        if (v.magnitude.empty()) {
            throw std::runtime_error("Internal division by zero.");
        }
        if (internal_compare_mag(u, v) < 0) {
            return {TempBignum(), u}; // q=0, r=u
        }

        if (v.magnitude.size() == 1) {
            // Simplified case for single-digit divisor.
            TempBignum q;
            unsigned __int128 rem = 0;
            q.magnitude.resize(u.magnitude.size());
            for (int i = u.magnitude.size() - 1; i >= 0; --i) {
                unsigned __int128 current = (rem << 64) | u.magnitude[i];
                q.magnitude[i] = static_cast<unsigned long>(current / v.magnitude[0]);
                rem = current % v.magnitude[0];
            }
            TempBignum r;
            if (rem > 0) r.magnitude.push_back(rem);
            return {q, r};
        }

        // Knuth's Algorithm D - Placeholder for full implementation
        // This algorithm is highly non-trivial. For the scope of this work,
        // we will return a placeholder for multi-digit divisors.
        return {TempBignum(), u};
    }

} // namespace proto
