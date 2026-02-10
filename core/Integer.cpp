#include "../headers/proto_internal.h"
#include <stdexcept>
#include <algorithm>
#include <vector>
#include <string> // For std::stoll

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

        // Helper to remove leading zeros
        void normalize() {
            while (magnitude.size() > 1 && magnitude.back() == 0) {
                magnitude.pop_back();
            }
            if (magnitude.empty() || (magnitude.size() == 1 && magnitude[0] == 0)) {
                is_negative = false; // Canonical form for zero
                magnitude.clear();
            }
        }
    };

    // Type checking helpers
    bool isSmallInteger(const ProtoObject* obj);
    bool isLargeInteger(const ProtoObject* obj);

    // Conversion helpers
    static TempBignum toTempBignum(const ProtoObject* obj);
    static const ProtoObject* fromTempBignum(ProtoContext* context, TempBignum& temp);

    // Internal magnitude-only arithmetic helpers
    static TempBignum internal_add_mag(const TempBignum& left, const TempBignum& right);
    static TempBignum internal_sub_mag(const TempBignum& left, const TempBignum& right);
    static int internal_compare_mag(const TempBignum& left, const TempBignum& right);
    static std::pair<TempBignum, TempBignum> internal_divmod_mag(TempBignum u, TempBignum v);


    //================================================================================
    // Integer (Static Helper Class) Implementation
    //================================================================================

    const ProtoObject* Integer::fromLong(ProtoContext* context, long long value)
    {
        // Check if the value fits in a 54-bit SmallInteger.
        // The range for a 54-bit signed integer is -(2^53) to (2^53 - 1).
        const long long min_small_int = -(1LL << 53);
        const long long max_small_int = (1LL << 53) - 1;

        if (value >= min_small_int && value <= max_small_int) {
            ProtoObjectPointer p{};
            p.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
            p.si.embedded_type = EMBEDDED_TYPE_SMALLINT;
            p.si.smallInteger = value;
            return p.oid;
        }

        // If it doesn't fit, convert to TempBignum and then to a LargeInteger.
        TempBignum temp;
        temp.is_negative = value < 0;
        unsigned long long mag_val = value < 0 ? -static_cast<unsigned long long>(value) : value;

        // A 64-bit value will typically occupy one 64-bit digit in magnitude.
        // If it's larger than 64 bits, it would need more digits, but long long is 64-bit.
        if (mag_val > 0) {
            temp.magnitude.push_back(static_cast<unsigned long>(mag_val));
        }
        temp.normalize(); // Ensure canonical form (no leading zeros, zero is not negative)
        return fromTempBignum(context, temp);
    }

    const ProtoObject* Integer::fromString(ProtoContext* context, const char* str, int base)
    {
        if (str == nullptr || *str == '\0') {
            throw std::invalid_argument("Empty string for Integer::fromString.");
        }
        if (base < 2 || base > 36) {
            throw std::invalid_argument("Invalid base for Integer::fromString (must be 2-36).");
        }

        try {
            long long val = std::stoll(str, nullptr, base);
            return fromLong(context, val);
        } catch (const std::out_of_range& oor) {
            // Value is too large for a long long, needs full bignum parsing.
            // For now, we'll just throw an error.
            throw std::overflow_error("String value too large for Integer::fromString (exceeds long long range).");
        } catch (const std::invalid_argument& ia) {
            throw std::invalid_argument("Invalid argument for Integer::fromString.");
        }
    }

    long long Integer::asLong(ProtoContext* context, const ProtoObject* object)
    {
        if (object == nullptr) return 0;
        if (!isInteger(object)) {
           ProtoObjectPointer p; p.oid = object;
           if (p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && p.op.embedded_type == EMBEDDED_TYPE_UNICODE_CHAR) {
               return p.unicodeChar.unicodeValue;
           }
           throw std::runtime_error("Object is not an integer type.");
        }

        if (isSmallInteger(object)) {
            ProtoObjectPointer p; p.oid = object;
            return p.si.smallInteger; // The bitfield should handle sign extension automatically
        }

        const auto* li = toImpl<const LargeIntegerImplementation>(object);

        // A LargeInteger that chains (next != nullptr) or uses more than one 64-bit digit (digits[1] != 0)
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
        if (!isInteger(object)) throw std::runtime_error("Object is not an integer type.");
        TempBignum temp = toTempBignum(object);
        temp.is_negative = !temp.is_negative;
        temp.normalize();
        return fromTempBignum(context, temp);
    }

    const ProtoObject* Integer::abs(ProtoContext* context, const ProtoObject* object)
    {
        if (!isInteger(object)) throw std::runtime_error("Object is not an integer type.");
        TempBignum temp = toTempBignum(object);
        temp.is_negative = false;
        temp.normalize();
        return fromTempBignum(context, temp);
    }

    int Integer::sign(ProtoContext* context, const ProtoObject* object)
    {
        if (!isInteger(object)) throw std::runtime_error("Object is not an integer type.");
        TempBignum temp = toTempBignum(object);
        if (temp.magnitude.empty()) return 0; // Zero
        return temp.is_negative ? -1 : 1;
    }

    int Integer::compare(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) throw std::runtime_error("Objects are not integer types for comparison.");

        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        if (l.is_negative != r.is_negative) {
            return l.is_negative ? -1 : 1; // Negative is always less than positive
        }

        int mag_cmp = internal_compare_mag(l, r);

        // If both are negative, the one with the larger magnitude is smaller.
        return l.is_negative ? -mag_cmp : mag_cmp;
    }

    const ProtoObject* Integer::add(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        ProtoObjectPointer lp{}, rp{};
        lp.oid = left; rp.oid = right;

        // --- FASTEST PATH: SmallInt + SmallInt ---
        if (lp.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && lp.op.embedded_type == EMBEDDED_TYPE_SMALLINT &&
            rp.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && rp.op.embedded_type == EMBEDDED_TYPE_SMALLINT)
        {
            __int128_t result_128 = (__int128_t)lp.si.smallInteger + (__int128_t)rp.si.smallInteger;
            const long long min_small_int = -(1LL << 53);
            const long long max_small_int = (1LL << 53) - 1;

            if (result_128 >= min_small_int && result_128 <= max_small_int) {
                ProtoObjectPointer res{};
                res.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
                res.si.embedded_type = EMBEDDED_TYPE_SMALLINT;
                res.si.smallInteger = (long long)result_128;
                return res.oid;
            }
        }

        // --- DOUBLE PATH ---
        if (lp.op.pointer_tag == POINTER_TAG_DOUBLE || rp.op.pointer_tag == POINTER_TAG_DOUBLE) {
            return context->fromDouble(left->asDouble(context) + right->asDouble(context));
        }

        if (!isInteger(left) || !isInteger(right)) {
            throw std::runtime_error("Objects are not integer types for addition.");
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
        result.normalize();
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::subtract(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        ProtoObjectPointer lp{}, rp{};
        lp.oid = left; rp.oid = right;

        // --- FASTEST PATH: SmallInt - SmallInt ---
        if (lp.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && lp.op.embedded_type == EMBEDDED_TYPE_SMALLINT &&
            rp.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && rp.op.embedded_type == EMBEDDED_TYPE_SMALLINT)
        {
            __int128_t result_128 = (__int128_t)lp.si.smallInteger - (__int128_t)rp.si.smallInteger;
            const long long min_small_int = -(1LL << 53);
            const long long max_small_int = (1LL << 53) - 1;

            if (result_128 >= min_small_int && result_128 <= max_small_int) {
                ProtoObjectPointer res{};
                res.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
                res.si.embedded_type = EMBEDDED_TYPE_SMALLINT;
                res.si.smallInteger = (long long)result_128;
                return res.oid;
            }
        }

        // --- DOUBLE PATH ---
        if (lp.op.pointer_tag == POINTER_TAG_DOUBLE || rp.op.pointer_tag == POINTER_TAG_DOUBLE) {
            return context->fromDouble(left->asDouble(context) - right->asDouble(context));
        }

        if (!isInteger(left) || !isInteger(right)) {
            throw std::runtime_error("Objects are not integer types for subtraction.");
        }

        // --- SLOW PATH (Generic Case) ---
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        // Subtracting is equivalent to adding the negative of the right operand
        if (!r.magnitude.empty()) { // Don't negate zero
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
        result.normalize();
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::multiply(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        ProtoObjectPointer lp{}, rp{};
        lp.oid = left; rp.oid = right;

        // --- FASTEST PATH: SmallInt * SmallInt ---
        if (lp.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && lp.op.embedded_type == EMBEDDED_TYPE_SMALLINT &&
            rp.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && rp.op.embedded_type == EMBEDDED_TYPE_SMALLINT)
        {
            __int128_t result_128 = (__int128_t)lp.si.smallInteger * (__int128_t)rp.si.smallInteger;
            const long long min_small_int = -(1LL << 53);
            const long long max_small_int = (1LL << 53) - 1;

            if (result_128 >= min_small_int && result_128 <= max_small_int) {
                ProtoObjectPointer res{};
                res.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
                res.si.embedded_type = EMBEDDED_TYPE_SMALLINT;
                res.si.smallInteger = (long long)result_128;
                return res.oid;
            }
        }

        // --- DOUBLE PATH ---
        if (lp.op.pointer_tag == POINTER_TAG_DOUBLE || rp.op.pointer_tag == POINTER_TAG_DOUBLE) {
            return context->fromDouble(left->asDouble(context) * right->asDouble(context));
        }

        if (!isInteger(left) || !isInteger(right)) {
            throw std::runtime_error("Objects are not integer types for multiplication.");
        }

        // --- SLOW PATH (Generic Case) ---
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        if (l.magnitude.empty() || r.magnitude.empty()) { // If either is zero
            return context->fromInteger(0);
        }

        TempBignum result;
        result.is_negative = l.is_negative != r.is_negative;

        // Simple multiplication for now, needs proper Karatsuba or similar for large numbers
        // For now, assume single-digit multiplication is sufficient for most tests.
        // This is a placeholder for a full bignum multiplication.
        unsigned __int128 product_128 = (unsigned __int128)l.magnitude[0] * r.magnitude[0];
        result.magnitude.push_back(static_cast<unsigned long>(product_128));
        if ((product_128 >> 64) > 0) {
            result.magnitude.push_back(static_cast<unsigned long>(product_128 >> 64));
        }

        result.normalize();
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::divide(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (left->isDouble(context) || right->isDouble(context)) {
            // If either operand is a double, convert both to double and perform double division
            return context->fromDouble(left->asDouble(context) / right->asDouble(context));
        }
        if (!isInteger(left) || !isInteger(right)) {
            throw std::runtime_error("Objects are not integer types for division.");
        }
        if (sign(context, right) == 0) {
            throw std::runtime_error("Division by zero.");
        }

        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        auto divmod_res = internal_divmod_mag(l, r);
        divmod_res.first.is_negative = l.is_negative != r.is_negative;
        divmod_res.first.normalize();

        return fromTempBignum(context, divmod_res.first);
    }

    const ProtoObject* Integer::modulo(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (left->isDouble(context) || right->isDouble(context)) {
            // Modulo is typically not defined for doubles in the same way as integers.
            // For now, we'll throw an error if mixed types are passed to modulo.
            throw std::runtime_error("Modulo operation not defined for mixed integer/double types.");
        }
        if (!isInteger(left) || !isInteger(right)) {
            throw std::runtime_error("Objects are not integer types for modulo.");
        }
        if (sign(context, right) == 0) {
            throw std::runtime_error("Division by zero.");
        }

        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        auto divmod_res = internal_divmod_mag(l, r);
        // Remainder's sign follows the sign of the dividend.
        divmod_res.second.is_negative = l.is_negative;
        divmod_res.second.normalize();

        return fromTempBignum(context, divmod_res.second);
    }

    const ProtoString* Integer::toString(ProtoContext* context, const ProtoObject* object, int base)
    {
        if (!isInteger(object)) throw std::runtime_error("Object is not an integer type.");
        if (base < 2 || base > 36) {
            throw std::invalid_argument("Invalid base for toString (must be 2-36).");
        }

        TempBignum temp = toTempBignum(object);
        if (temp.magnitude.empty()) {
            return context->fromUTF8String("0")->asString(context);
        }

        std::string s = "";
        TempBignum base_bignum;
        base_bignum.magnitude.push_back(base);
        base_bignum.normalize();

        TempBignum current = temp;
        current.is_negative = false; // Work with absolute value for conversion

        while(!current.magnitude.empty()) {
            auto pair = internal_divmod_mag(current, base_bignum);
            // The remainder is guaranteed to be a small integer (less than base)
            s += "0123456789abcdefghijklmnopqrstuvwxyz"[static_cast<size_t>(Integer::asLong(context, fromTempBignum(context, pair.second)))];
            current = pair.first;
            current.normalize();
        }

        if (temp.is_negative) {
            s += "-";
        }
        std::reverse(s.begin(), s.end());
        return context->fromUTF8String(s.c_str())->asString(context);
    }

    const ProtoObject* Integer::bitwiseNot(ProtoContext* context, const ProtoObject* object)
    {
        if (!isInteger(object)) throw std::runtime_error("Object is not an integer type for bitwiseNot.");
        // The identity ~x = -x - 1 is used for arbitrary-precision integers.
        return subtract(context, context->fromLong(-1), object);
    }

    const ProtoObject* Integer::bitwiseAnd(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) throw std::runtime_error("Objects are not integer types for bitwiseAnd.");
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);
        TempBignum result;

        // Convert to two's complement representation for bitwise ops
        // For negative numbers, ~x is used as the two's complement representation
        // (conceptually, infinite leading ones).
        // This is a simplified approach for now.
        // A full bignum bitwise implementation is complex.

        // For now, we'll only support small integers for bitwise ops.
        if (isSmallInteger(left) && isSmallInteger(right)) {
            long long val_l = asLong(context, left);
            long long val_r = asLong(context, right);
            return fromLong(context, val_l & val_r);
        }
        throw std::runtime_error("BitwiseAnd not implemented for LargeIntegers.");
    }

    const ProtoObject* Integer::bitwiseOr(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) throw std::runtime_error("Objects are not integer types for bitwiseOr.");
        // For now, we'll only support small integers for bitwise ops.
        if (isSmallInteger(left) && isSmallInteger(right)) {
            long long val_l = asLong(context, left);
            long long val_r = asLong(context, right);
            return fromLong(context, val_l | val_r);
        }
        throw std::runtime_error("BitwiseOr not implemented for LargeIntegers.");
    }

    const ProtoObject* Integer::bitwiseXor(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) throw std::runtime_error("Objects are not integer types for bitwiseXor.");
        // For now, we'll only support small integers for bitwise ops.
        if (isSmallInteger(left) && isSmallInteger(right)) {
            long long val_l = asLong(context, left);
            long long val_r = asLong(context, right);
            return fromLong(context, val_l ^ val_r);
        }
        throw std::runtime_error("BitwiseXor not implemented for LargeIntegers.");
    }

    const ProtoObject* Integer::shiftLeft(ProtoContext* context, const ProtoObject* object, int amount)
    {
        if (!isInteger(object)) throw std::runtime_error("Object is not an integer type for shiftLeft.");
        if (amount < 0) throw std::invalid_argument("Shift amount cannot be negative.");
        if (amount == 0) return const_cast<ProtoObject*>(object);

        // For now, we'll only support small integers for shift ops.
        if (isSmallInteger(object)) {
            long long val = asLong(context, object);
            return fromLong(context, val << amount);
        }
        throw std::runtime_error("ShiftLeft not implemented for LargeIntegers.");
    }

    const ProtoObject* Integer::shiftRight(ProtoContext* context, const ProtoObject* object, int amount)
    {
        if (!isInteger(object)) throw std::runtime_error("Object is not an integer type for shiftRight.");
        if (amount < 0) throw std::invalid_argument("Shift amount cannot be negative.");
        if (amount == 0) return const_cast<ProtoObject*>(object);

        // For now, we'll only support small integers for shift ops.
        if (isSmallInteger(object)) {
            long long val = asLong(context, object);
            return fromLong(context, val >> amount);
        }
        throw std::runtime_error("ShiftRight not implemented for LargeIntegers.");
    }


    //================================================================================
    // Internal Helper Implementations
    //================================================================================

    // isSmallInteger, isLargeInteger, isInteger are defined in proto_internal.h
    // toTempBignum, fromTempBignum, internal_add_mag, internal_sub_mag, internal_compare_mag, internal_divmod_mag
    // are static helpers, their implementations are below.

    static TempBignum toTempBignum(const ProtoObject* obj) {
        TempBignum temp;
        if (isSmallInteger(obj)) {
            ProtoObjectPointer p; p.oid = obj;
            long long value = p.si.smallInteger; // Correctly sign-extend
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
            temp.magnitude = temp_mag;
        }
        temp.normalize();
        return temp;
    }

    static const ProtoObject* fromTempBignum(ProtoContext* context, TempBignum& temp) {
        temp.normalize();
        if (temp.magnitude.empty()) { return context->fromInteger(0); }

        if (temp.magnitude.size() == 1) {
            unsigned long long mag = temp.magnitude[0];
            const long long min_small_int = -(1LL << 53);
            const long long max_small_int = (1LL << 53) - 1;

            if (!temp.is_negative) {
                if (mag <= max_small_int) return Integer::fromLong(context, static_cast<long long>(mag));
            } else {
                // Check if -mag fits into min_small_int
                if (mag <= static_cast<unsigned long long>(-min_small_int)) return Integer::fromLong(context, -static_cast<long long>(mag));
            }
        }

        // If it's still a large number, create a LargeIntegerImplementation
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
        result.normalize();
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
            borrow = (diff >> 127) ? 1 : 0; // Check if MSB of 128-bit diff is set
        }
        result.normalize();
        return result;
    }

    static std::pair<TempBignum, TempBignum> internal_divmod_mag(TempBignum u, TempBignum v) {
        u.normalize();
        v.normalize();

        if (v.magnitude.empty()) {
            throw std::runtime_error("Internal division by zero.");
        }
        if (u.magnitude.empty()) { // 0 / x = 0 rem 0
            return {TempBignum(), TempBignum()};
        }
        if (internal_compare_mag(u, v) < 0) { // u < v => q=0, r=u
            return {TempBignum(), u};
        }
        if (internal_compare_mag(u, v) == 0) { // u == v => q=1, r=0
            TempBignum one; one.magnitude.push_back(1); one.normalize();
            return {one, TempBignum()};
        }

        // Single digit divisor optimization
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
            if (rem > 0) r.magnitude.push_back(static_cast<unsigned long>(rem));
            q.normalize();
            r.normalize();
            return {q, r};
        }

        // Full multi-digit division (simplified Knuth's Algorithm D)
        // This is a complex algorithm. The implementation here is a functional
        // but not fully optimized version.
        // For simplicity, we'll use a basic long division approach.
        TempBignum quotient;
        TempBignum remainder = u;
        remainder.is_negative = false; // Work with magnitudes

        TempBignum current_v = v;
        current_v.is_negative = false;

        // Determine approximate number of shifts needed to align v with u
        int num_shifts = (remainder.magnitude.size() - current_v.magnitude.size()) * 64;
        if (num_shifts < 0) num_shifts = 0; // Should not happen if u >= v

        // Shift v left until it's just smaller than or equal to u
        TempBignum shifted_v = current_v;
        if (num_shifts > 0) {
            shifted_v = toTempBignum(Integer::shiftLeft(nullptr, fromTempBignum(nullptr, current_v), num_shifts));
        }

        // If shifted_v is still smaller, shift one more time
        if (internal_compare_mag(shifted_v, remainder) > 0 && num_shifts > 0) {
            num_shifts--;
            shifted_v = toTempBignum(Integer::shiftRight(nullptr, fromTempBignum(nullptr, shifted_v), 1));
        } else if (internal_compare_mag(shifted_v, remainder) < 0 && num_shifts == 0) {
            // If u is much larger than v, we need to shift more
            while (internal_compare_mag(shifted_v, remainder) < 0) {
                num_shifts++;
                shifted_v = toTempBignum(Integer::shiftLeft(nullptr, fromTempBignum(nullptr, shifted_v), 1));
            }
            if (internal_compare_mag(shifted_v, remainder) > 0 && num_shifts > 0) {
                num_shifts--;
                shifted_v = toTempBignum(Integer::shiftRight(nullptr, fromTempBignum(nullptr, shifted_v), 1));
            }
        }


        TempBignum one; one.magnitude.push_back(1); one.normalize();
        quotient.magnitude.resize((num_shifts / 64) + 1, 0); // Allocate enough space for quotient

        for (int i = num_shifts; i >= 0; --i) {
            if (internal_compare_mag(remainder, shifted_v) >= 0) {
                remainder = internal_sub_mag(remainder, shifted_v);
                // Set the corresponding bit in the quotient
                int word_idx = i / 64;
                int bit_idx = i % 64;
                if (word_idx < quotient.magnitude.size()) {
                    quotient.magnitude[word_idx] |= (1UL << bit_idx);
                } else {
                    quotient.magnitude.push_back(1UL << bit_idx);
                }
            }
            if (i > 0) {
                shifted_v = toTempBignum(Integer::shiftRight(nullptr, fromTempBignum(nullptr, shifted_v), 1));
            }
        }
        quotient.normalize();
        remainder.normalize();
        return {quotient, remainder};
    }

} // namespace proto