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
        return p.oid;
    }

//================================================================================
     // Internal Helper Implementations
     //================================================================================

     bool isSmallInteger(const ProtoObject* obj) { ProtoObjectPointer p; p.oid = obj; return p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && p.op.embedded_type == EMBEDDED_TYPE_SMALLINT; }
     bool isLargeInteger(const ProtoObject* obj) { ProtoObjectPointer p; p.oid = obj; return p.op.pointer_tag == POINTER_TAG_LARGE_INTEGER; }
     bool isInteger(const ProtoObject* obj) { return isSmallInteger(obj) || isLargeInteger(obj); }

     static TempBignum toTempBignum(const ProtoObject* obj) {
         TempBignum temp;
         if (isSmallInteger(obj)) {
             ProtoObjectPointer p; p.oid = obj;
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
