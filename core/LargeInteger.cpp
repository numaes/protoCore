#include "../headers/proto_internal.h"
#include <stdexcept>
#include <algorithm>

namespace proto
{
    //================================================================================
    // Forward Declarations & Type Helpers
    //================================================================================

    struct TempBignum {
        bool is_negative = false;
        std::vector<unsigned long> magnitude;
    };

    static bool isSmallInteger(const ProtoObject* obj);
    static bool isLargeInteger(const ProtoObject* obj);
    static bool isInteger(const ProtoObject* obj);

    static TempBignum toTempBignum(const ProtoObject* obj);
    static const ProtoObject* fromTempBignum(ProtoContext* context, TempBignum& temp);
    
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
        for (int i = 0; i < DIGIT_COUNT; ++i) { digits[i] = 0; }
    }

    LargeIntegerImplementation::~LargeIntegerImplementation() {}

    unsigned long LargeIntegerImplementation::getHash(ProtoContext* context) const {
        return is_negative ? ~digits[0] : digits[0];
    }

    void LargeIntegerImplementation::finalize(ProtoContext* context) const {}

    void LargeIntegerImplementation::processReferences(
        ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        if (next) {
            method(context, self, toImpl<Cell>(next));
        }
    }

    const ProtoObject* LargeIntegerImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p;
        p.largeIntegerImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LARGE_INTEGER;
        return p.oid.oid;
    }

    //================================================================================
    // Integer (Helper Class) Implementation
    //================================================================================

    const ProtoObject* Integer::fromLong(ProtoContext* context, long long value)
    {
        const long long min_small_int = -(1LL << 55);
        const long long max_small_int = (1LL << 55) - 1;

        if (value >= min_small_int && value <= max_small_int) {
            ProtoObjectPointer p{};
            p.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
            p.si.embedded_type = EMBEDDED_TYPE_SMALLINT;
            p.si.smallInteger = value;
            return p.oid.oid;
        }
        
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
        if (!isInteger(object)) return 0;

        if (isSmallInteger(object)) {
            ProtoObjectPointer p; p.oid.oid = object;
            return p.si.smallInteger;
        }

        const auto* li = toImpl<const LargeIntegerImplementation>(object);
        if (li->next != nullptr || li->digits[1] != 0) {
            return 0; // Error: Too large
        }

        unsigned long long magnitude = li->digits[0];
        if (li->is_negative) {
            if (magnitude > static_cast<unsigned long long>(LLONG_MAX) + 1) return 0; // Error: Too large
            return -static_cast<long long>(magnitude);
        } else {
            if (magnitude > static_cast<unsigned long long>(LLONG_MAX)) return 0; // Error: Too large
            return static_cast<long long>(magnitude);
        }
    }

    const ProtoObject* Integer::negate(ProtoContext* context, const ProtoObject* object)
    {
        if (!isInteger(object)) return context->fromInteger(0);
        TempBignum temp = toTempBignum(object);
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
        // LargeIntegers are guaranteed non-zero.
        return li->is_negative ? -1 : 1;
    }

    int Integer::compare(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return 0;

        int sign_l = sign(context, left);
        int sign_r = sign(context, right);

        if (sign_l != sign_r) {
            return sign_l < sign_r ? -1 : 1;
        }
        if (sign_l == 0) return 0; // Both are zero

        // Signs are the same and non-zero
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);
        int mag_cmp = internal_compare_mag(l, r);
        return l.is_negative ? -mag_cmp : mag_cmp;
    }

    const ProtoObject* Integer::add(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return context->fromInteger(0);
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
        const ProtoObject* neg_right = negate(context, right);
        return add(context, left, neg_right);
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
        if (!isInteger(left) || !isInteger(right) || sign(context, right) == 0) return PROTO_NONE; // Division by zero
        
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        auto divmod_res = internal_divmod_mag(l, r);
        divmod_res.first.is_negative = l.is_negative != r.is_negative;
        
        return fromTempBignum(context, divmod_res.first);
    }

    const ProtoObject* Integer::modulo(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right) || sign(context, right) == 0) return PROTO_NONE; // Division by zero

        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        auto divmod_res = internal_divmod_mag(l, r);
        divmod_res.second.is_negative = l.is_negative; // Remainder has sign of dividend
        
        return fromTempBignum(context, divmod_res.second);
    }

    const ProtoString* Integer::toString(ProtoContext* context, const ProtoObject* object, int base)
    {
        if (!isInteger(object) || base < 2 || base > 36) return nullptr;
        
        TempBignum temp = toTempBignum(object);
        if (temp.magnitude.empty()) {
            return context->fromUTF8String("0")->asString(context);
        }

        std::string s = "";
        TempBignum base_bignum;
        base_bignum.magnitude.push_back(base);

        while(!temp.magnitude.empty()) {
            auto pair = internal_divmod_mag(temp, base_bignum);
            s += "0123456789abcdefghijklmnopqrstuvwxyz"[asLong(context, fromTempBignum(context, pair.second))];
            temp = pair.first;
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
        // ~x == -x - 1
        const ProtoObject* neg_obj = negate(context, object);
        const ProtoObject* one = context->fromInteger(1);
        return subtract(context, neg_obj, one);
    }

    // NOTE: Full bitwise operations for negative numbers are complex.
    // The following implementations are correct only for positive operands.
    const ProtoObject* Integer::bitwiseAnd(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return PROTO_NONE;
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        if (l.is_negative || r.is_negative) return PROTO_NONE; // Placeholder

        TempBignum result;
        result.is_negative = false;
        size_t min_size = std::min(l.magnitude.size(), r.magnitude.size());
        result.magnitude.resize(min_size);
        for(size_t i = 0; i < min_size; ++i) {
            result.magnitude[i] = l.magnitude[i] & r.magnitude[i];
        }
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::bitwiseOr(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return PROTO_NONE;
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        if (l.is_negative || r.is_negative) return PROTO_NONE; // Placeholder

        TempBignum result;
        result.is_negative = false;
        size_t max_size = std::max(l.magnitude.size(), r.magnitude.size());
        result.magnitude.resize(max_size);
        for(size_t i = 0; i < max_size; ++i) {
            unsigned long l_digit = (i < l.magnitude.size()) ? l.magnitude[i] : 0;
            unsigned long r_digit = (i < r.magnitude.size()) ? r.magnitude[i] : 0;
            result.magnitude[i] = l_digit | r_digit;
        }
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::bitwiseXor(ProtoContext* context, const ProtoObject* left, const ProtoObject* right)
    {
        if (!isInteger(left) || !isInteger(right)) return PROTO_NONE;
        TempBignum l = toTempBignum(left);
        TempBignum r = toTempBignum(right);

        if (l.is_negative || r.is_negative) return PROTO_NONE; // Placeholder for negative numbers

        TempBignum result;
        result.is_negative = false;
        size_t max_size = std::max(l.magnitude.size(), r.magnitude.size());
        result.magnitude.resize(max_size);
        for(size_t i = 0; i < max_size; ++i) {
            unsigned long l_digit = (i < l.magnitude.size()) ? l.magnitude[i] : 0;
            unsigned long r_digit = (i < r.magnitude.size()) ? r.magnitude[i] : 0;
            result.magnitude[i] = l_digit ^ r_digit;
        }
        return fromTempBignum(context, result);
    }

    const ProtoObject* Integer::shiftLeft(ProtoContext* context, const ProtoObject* object, int amount)
    {
        if (!isInteger(object) || amount < 0) return PROTO_NONE;
        if (amount == 0) return const_cast<ProtoObject*>(object);
        
        TempBignum temp = toTempBignum(object);
        if (temp.magnitude.empty()) return context->fromInteger(0);

        int word_shift = amount / 64;
        int bit_shift = amount % 64;

        size_t new_size = temp.magnitude.size() + word_shift + (bit_shift > 0 ? 1 : 0);
        temp.magnitude.resize(new_size, 0);

        if (word_shift > 0) {
            for(int i = temp.magnitude.size() - 1; i >= word_shift; --i) {
                temp.magnitude[i] = temp.magnitude[i - word_shift];
            }
            for(int i = 0; i < word_shift; ++i) {
                temp.magnitude[i] = 0;
            }
        }

        if (bit_shift > 0) {
            unsigned __int128 carry = 0;
            for(size_t i = 0; i < temp.magnitude.size(); ++i) {
                unsigned __int128 val = ((unsigned __int128)temp.magnitude[i] << bit_shift) | carry;
                temp.magnitude[i] = static_cast<unsigned long>(val);
                carry = val >> 64;
            }
        }
        return fromTempBignum(context, temp);
    }

    const ProtoObject* Integer::shiftRight(ProtoContext* context, const ProtoObject* object, int amount)
    {
        if (!isInteger(object) || amount < 0) return PROTO_NONE;
        // Requires division for negative numbers, complex. Placeholder for now.
        return PROTO_NONE;
    }

    //================================================================================
    // Internal Helper Implementations
    //================================================================================
    
    static bool isSmallInteger(const ProtoObject* obj) { ProtoObjectPointer p; p.oid.oid = obj; return p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && p.op.embedded_type == EMBEDDED_TYPE_SMALLINT; }
    static bool isLargeInteger(const ProtoObject* obj) { ProtoObjectPointer p; p.oid.oid = obj; return p.op.pointer_tag == POINTER_TAG_LARGE_INTEGER; }
    static bool isInteger(const ProtoObject* obj) { return isSmallInteger(obj) || isLargeInteger(obj); }

    static TempBignum toTempBignum(const ProtoObject* obj) {
        TempBignum temp;
        if (isSmallInteger(obj)) {
            ProtoObjectPointer p; p.oid.oid = obj;
            long long value = p.si.smallInteger;
            if (value != 0) {
                temp.is_negative = value < 0;
                temp.magnitude.push_back(value < 0 ? -value : value);
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

    // Simplified long division for single-digit divisor.
    static std::pair<TempBignum, TempBignum> internal_divmod_single_digit(TempBignum dividend, unsigned long divisor) {
        TempBignum quotient;
        unsigned __int128 remainder = 0;
        quotient.magnitude.resize(dividend.magnitude.size());

        for (int i = dividend.magnitude.size() - 1; i >= 0; --i) {
            unsigned __int128 current_val = (remainder << 64) | dividend.magnitude[i];
            quotient.magnitude[i] = static_cast<unsigned long>(current_val / divisor);
            remainder = current_val % divisor;
        }

        TempBignum rem_bignum;
        rem_bignum.magnitude.push_back(static_cast<unsigned long>(remainder));
        
        while (quotient.magnitude.size() > 1 && quotient.magnitude.back() == 0) { quotient.magnitude.pop_back(); }

        return {quotient, rem_bignum};
    }

    static std::pair<TempBignum, TempBignum> internal_divmod_mag(TempBignum dividend, TempBignum divisor) {
        if (internal_compare_mag(dividend, divisor) < 0) {
            return {TempBignum(), dividend}; // Quotient 0, Remainder = dividend
        }
        if (divisor.magnitude.size() == 1) {
            return internal_divmod_single_digit(dividend, divisor.magnitude[0]);
        }
        // Placeholder for multi-digit division (Knuth's Algorithm D)
        // This is a highly non-trivial algorithm.
        // For now, we return 0 as quotient and dividend as remainder.
        return {TempBignum(), dividend};
    }
} // namespace proto
