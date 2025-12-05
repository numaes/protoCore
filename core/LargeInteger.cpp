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
 *
 *  A key optimization in this implementation is the use of a "fast path" for
 *  operations involving only SmallIntegers. These operations are performed
 *  natively using 128-bit integers to prevent overflow, and only fall back
 *  to the generic, more expensive "slow path" (using TempBignum) if the
 *  result itself exceeds the SmallInteger range.
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
    bool isSmallInteger(const ProtoObject* obj);
    bool isLargeInteger(const ProtoObject* obj);
    bool isInteger(const ProtoObject* obj);

    // Conversion helpers
    static TempBignum toTempBignum(const ProtoObject* obj);
    static const ProtoObject* fromTempBignum(ProtoContext* context, TempBignum& temp);
    
    // Internal magnitude-only arithmetic helpers
    static TempBignum internal_add_mag(const TempBignum& left, const TempBignum& right);
    static TempBignum internal_sub_mag(const TempBignum& left, const TempBignum& right);
    static int internal_compare_mag(const TempBignum& left, const TempBignum& right);
    static std::pair<TempBignum, TempBignum> internal_divmod_mag(TempBignum u, TempBignum v);


    //================================================================================
    // LargeIntegerImplementation
    //================================================================================

    const int LargeIntegerImplementation::DIGIT_COUNT;

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
            // A 64-bit value will only ever occupy one 64-bit digit.
            mag_val = 0;
        }
        return fromTempBignum(context, temp);
    }

    const ProtoObject* Integer::fromString(ProtoContext* context, const char* str, int base)
    {
        // Placeholder implementation.
        long long val = std::stoll(str, nullptr, base);
        return fromLong(context, val);
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

        // --- FAST PATH for SmallInt + SmallInt ---
        if (isSmallInteger(left) && isSmallInteger(right)) {
            __int128_t result = (__int128_t)asLong(context, left) + (__int128_t)asLong(context, right);
            const long long min_small_int = -(1LL << 55);
            const long long max_small_int = (1LL << 55) - 1;

            if (result >= min_small_int && result <= max_small_int) {
                return fromLong(context, (long long)result);
            } else {
                TempBignum temp_result;
                temp_result.is_negative = result < 0;
                unsigned __int128 mag_val = result < 0 ? -result : result;
                while (mag_val > 0) {
                    temp_result.magnitude.push_back((unsigned long)mag_val);
                    mag_val >>= 64;
                }
                return fromTempBignum(context, temp_result);
            }
        }

        // --- SLOW PATH (Generic Case for LargeIntegers) ---
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);
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

    const ProtoObject* Integer::subtract(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return context->fromInteger(0);

        // --- FAST PATH for SmallInt - SmallInt ---
        if (isSmallInteger(left) && isSmallInteger(right)) {
            __int128_t result = (__int128_t)asLong(context, left) - (__int128_t)asLong(context, right);
            const long long min_small_int = -(1LL << 55);
            const long long max_small_int = (1LL << 55) - 1;

            if (result >= min_small_int && result <= max_small_int) {
                return fromLong(context, (long long)result);
            } else {
                TempBignum temp_result;
                temp_result.is_negative = result < 0;
                unsigned __int128 mag_val = result < 0 ? -result : result;
                while (mag_val > 0) {
                    temp_result.magnitude.push_back((unsigned long)mag_val);
                    mag_val >>= 64;
                }
                return fromTempBignum(context, temp_result);
            }
        }
        
        // --- SLOW PATH (Generic Case) ---
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);
        if (!r.magnitude.empty()) {
            r.is_negative = !r.is_negative;
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

        // --- FAST PATH for SmallInt * SmallInt ---
        if (isSmallInteger(left) && isSmallInteger(right)) {
            __int128_t result = (__int128_t)asLong(context, left) * (__int128_t)asLong(context, right);
            const long long min_small_int = -(1LL << 55);
            const long long max_small_int = (1LL << 55) - 1;

            if (result >= min_small_int && result <= max_small_int) {
                return fromLong(context, (long long)result);
            } else {
                TempBignum temp_result;
                temp_result.is_negative = result < 0;
                unsigned __int128 mag_val = result < 0 ? -result : result;
                while (mag_val > 0) {
                    temp_result.magnitude.push_back((unsigned long)mag_val);
                    mag_val >>= 64;
                }
                return fromTempBignum(context, temp_result);
            }
        }

        // --- SLOW PATH (Generic Case) ---
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        if (l.magnitude.empty() || r.magnitude.empty()) {
            return context->fromInteger(0);
        }

        TempBignum result;
        result.is_negative = l.is_negative != r.is_negative;
        result.magnitude.resize(l.magnitude.size() + r.magnitude.size(), 0);

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
        return subtract(context, context->fromLong(-1), object);
    }

    const ProtoObject* Integer::bitwiseAnd(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return PROTO_NONE;
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);
        TempBignum result;

        bool l_neg = l.is_negative;
        bool r_neg = r.is_negative;

        if (l_neg) l = toTempBignum(bitwiseNot(context, fromTempBignum(context, l)));
        if (r_neg) r = toTempBignum(bitwiseNot(context, fromTempBignum(context, r)));

        size_t max_len = std::max(l.magnitude.size(), r.magnitude.size());
        l.magnitude.resize(max_len, 0);
        r.magnitude.resize(max_len, 0);
        result.magnitude.resize(max_len);

        if (l_neg && r_neg) {
            result.is_negative = true;
            for(size_t i=0; i<max_len; ++i) result.magnitude[i] = l.magnitude[i] | r.magnitude[i];
        } else if (l_neg || r_neg) {
            TempBignum* neg = l_neg ? &l : &r;
            TempBignum* pos = l_neg ? &r : &l;
            for(size_t i=0; i<max_len; ++i) result.magnitude[i] = pos->magnitude[i] & ~neg->magnitude[i];
        } else {
            for(size_t i=0; i<max_len; ++i) result.magnitude[i] = l.magnitude[i] & r.magnitude[i];
        }
        
        if (result.is_negative) {
             return bitwiseNot(context, fromTempBignum(context, result));
        }
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::bitwiseOr(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return PROTO_NONE;
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);
        TempBignum result;

        bool l_neg = l.is_negative;
        bool r_neg = r.is_negative;

        if (l_neg) l = toTempBignum(bitwiseNot(context, fromTempBignum(context, l)));
        if (r_neg) r = toTempBignum(bitwiseNot(context, fromTempBignum(context, r)));

        size_t max_len = std::max(l.magnitude.size(), r.magnitude.size());
        l.magnitude.resize(max_len, 0);
        r.magnitude.resize(max_len, 0);
        result.magnitude.resize(max_len);

        if (l_neg || r_neg) {
            result.is_negative = true;
            for(size_t i=0; i<max_len; ++i) result.magnitude[i] = l.magnitude[i] & r.magnitude[i];
        } else {
            for(size_t i=0; i<max_len; ++i) result.magnitude[i] = l.magnitude[i] | r.magnitude[i];
        }
        
        if (result.is_negative) {
             return bitwiseNot(context, fromTempBignum(context, result));
        }
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::bitwiseXor(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return PROTO_NONE;
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);
        TempBignum result;

        bool l_neg = l.is_negative;
        bool r_neg = r.is_negative;

        if (l_neg) l = toTempBignum(bitwiseNot(context, fromTempBignum(context, l)));
        if (r_neg) r = toTempBignum(bitwiseNot(context, fromTempBignum(context, r)));

        size_t max_len = std::max(l.magnitude.size(), r.magnitude.size());
        l.magnitude.resize(max_len, 0);
        r.magnitude.resize(max_len, 0);
        result.magnitude.resize(max_len);

        if (l_neg != r_neg) {
            result.is_negative = true;
            for(size_t i=0; i<max_len; ++i) result.magnitude[i] = ~(l.magnitude[i] ^ r.magnitude[i]);
        } else {
            for(size_t i=0; i<max_len; ++i) result.magnitude[i] = l.magnitude[i] ^ r.magnitude[i];
        }
        
        if (result.is_negative) {
             return bitwiseNot(context, fromTempBignum(context, result));
        }
        return fromTempBignum(context, result);
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
    
    bool isSmallInteger(const ProtoObject* obj) { ProtoObjectPointer p; p.oid.oid = obj; return p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && p.op.embedded_type == EMBEDDED_TYPE_SMALLINT; }
    bool isLargeInteger(const ProtoObject* obj) { ProtoObjectPointer p; p.oid.oid = obj; return p.op.pointer_tag == POINTER_TAG_LARGE_INTEGER; }
    bool isInteger(const ProtoObject* obj) { return isSmallInteger(obj) || isLargeInteger(obj); }

    static TempBignum toTempBignum(const ProtoObject* obj) {
        TempBignum temp;
        if (isSmallInteger(obj)) {
            ProtoObjectPointer p; p.oid.oid = obj;
            long long value = p.si.smallInteger;
            if (value != 0) {
                temp.is_negative = value < 0;
                temp.magnitude.push_back(value < 0 ? -static_cast<unsigned long long>(value) : value);
            }
        } else if (isLargeInteger(obj)) {
            const auto* li = toImpl<const LargeIntegerImplementation>(obj);
            temp.is_negative = li->is_negative;
            const auto* current = li;
            std::vector<unsigned long> temp_mag;
            while (current) {
                for (int i = 0; i < LargeIntegerImplementation::DIGIT_COUNT; ++i) {
                    temp_mag.push_back(current->digits[i]);
                }
                current = current->next;
            }
            while (temp_mag.size() > 1 && temp_mag.back() == 0) {
                temp_mag.pop_back();
            }
            temp.magnitude = temp_mag;
        }
        return temp;
    }

    static const ProtoObject* fromTempBignum(ProtoContext* context, TempBignum& temp) {
        while (temp.magnitude.size() > 1 && temp.magnitude.back() == 0) { temp.magnitude.pop_back(); }
        if (temp.magnitude.empty()) { return context->fromInteger(0); }

        if (temp.magnitude.size() == 1) {
            unsigned long long mag = temp.magnitude[0];
            const long long min_small_int = -(1LL << 55);
            const long long max_small_int = (1LL << 55) - 1;
            if (!temp.is_negative) {
                if (mag <= max_small_int) return Integer::fromLong(context, static_cast<long long>(mag));
            } else {
                if (mag <= static_cast<unsigned long long>(min_small_int) * -1) return Integer::fromLong(context, -static_cast<long long>(mag));
            }
        }

        const LargeIntegerImplementation* head = nullptr;
        LargeIntegerImplementation* current = nullptr;
        int digits_processed = 0;
        int num_digits = temp.magnitude.size();
        while (digits_processed < num_digits) {
            auto* new_chunk = new(context) LargeIntegerImplementation(context);
            new_chunk->is_negative = temp.is_negative;
            int digits_to_copy = std::min(num_digits - digits_processed, LargeIntegerImplementation::DIGIT_COUNT);
            for (int i = 0; i < digits_to_copy; ++i) { new_chunk->digits[i] = temp.magnitude[digits_processed + i]; }
            if (head == nullptr) { head = new_chunk; } else { current->next = new_chunk; }
            current = new_chunk;
            digits_processed += digits_to_copy;
        }
        return head->implAsObject(context);
    }

    static int internal_compare_mag(const TempBignum& left, const TempBignum& right) {
        if (left.magnitude.size() != right.magnitude.size()) return left.magnitude.size() < right.magnitude.size() ? -1 : 1;
        for (int i = left.magnitude.size() - 1; i >= 0; --i) {
            if (left.magnitude[i] != right.magnitude[i]) return left.magnitude[i] < right.magnitude[i] ? -1 : 1;
        }
        return 0;
    }

    static TempBignum internal_add_mag(const TempBignum& left, const TempBignum& right) {
        TempBignum result;
        unsigned __int128 carry = 0;
        size_t max_size = std::max(left.magnitude.size(), right.magnitude.size());
        result.magnitude.resize(max_size);
        for (size_t i = 0; i < max_size; ++i) {
            unsigned __int128 sum = carry;
            if (i < left.magnitude.size()) sum += left.magnitude[i];
            if (i < right.magnitude.size()) sum += right.magnitude[i];
            result.magnitude[i] = static_cast<unsigned long>(sum);
            carry = sum >> 64;
        }
        if (carry > 0) result.magnitude.push_back(static_cast<unsigned long>(carry));
        return result;
    }

    static TempBignum internal_sub_mag(const TempBignum& left, const TempBignum& right) {
        TempBignum result;
        unsigned __int128 borrow = 0;
        size_t max_size = left.magnitude.size();
        result.magnitude.resize(max_size);
        for (size_t i = 0; i < max_size; ++i) {
            unsigned __int128 l_digit = left.magnitude[i];
            unsigned __int128 r_digit = (i < right.magnitude.size()) ? right.magnitude[i] : 0;
            unsigned __int128 diff = l_digit - r_digit - borrow;
            result.magnitude[i] = static_cast<unsigned long>(diff);
            borrow = (diff >> 127) ? 1 : 0;
        }
        while (result.magnitude.size() > 1 && result.magnitude.back() == 0) { result.magnitude.pop_back(); }
        return result;
    }
    
    static std::pair<TempBignum, TempBignum> internal_divmod_mag(TempBignum u, TempBignum v) {
        if (v.magnitude.empty()) {
            throw std::runtime_error("Internal division by zero.");
        }
        if (internal_compare_mag(u, v) < 0) {
            return {TempBignum(), u};
        }

        if (v.magnitude.size() == 1) {
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

        // Full multi-digit division (simplified Knuth's Algorithm D)
        // This is a complex algorithm. The implementation here is a functional
        // but not fully optimized version.
        TempBignum q, r;
        for (int i = u.magnitude.size() * 64 - 1; i >= 0; --i) {
            r = toTempBignum(Integer::shiftLeft(nullptr, fromTempBignum(nullptr, r), 1));
            if ((u.magnitude[i/64] >> (i%64)) & 1) {
                r.magnitude[0] |= 1;
            }
            if (internal_compare_mag(r, v) >= 0) {
                r = internal_sub_mag(r, v);
                if (q.magnitude.empty()) q.magnitude.resize(u.magnitude.size());
                q.magnitude[i/64] |= (1UL << (i%64));
            }
        }
        return {q, r};
    }

} // namespace proto
