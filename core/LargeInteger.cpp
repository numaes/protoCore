/*
 * LargeInteger.cpp
 *
 *  Created on: 2024-05-20
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 *
 *  This file implements the heap-allocated LargeInteger cell and the tag
 *  predicates shared by the numeric code.  The arithmetic itself (the static
 *  `Integer` helper class and its sign-and-magnitude TempBignum helpers) is in
 *  Integer.cpp.
 */

#include "../headers/proto_internal.h"
#include <stdexcept>
#include <algorithm>
#include <vector>

namespace proto
{
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
    proto_ulong LargeIntegerImplementation::getHash(ProtoContext* context) const {
        // Hash every digit of every chunk. Hashing only digits[0] gave every
        // multiple of 2^64 the same hash, so 2**64, 2**65 and 2**70 collided
        // and hash-keyed structures kept only one of them. The representation
        // is canonical (fromTempBignum normalizes the magnitude and zeroes the
        // unused digits of the last chunk), so equal values hash equally.
        uint64_t h = is_negative ? 0x9E3779B97F4A7C15ULL : 0x6A09E667F3BCC909ULL;
        for (const LargeIntegerImplementation* chunk = this; chunk; chunk = chunk->next) {
            for (int i = 0; i < DIGIT_COUNT; ++i) {
                h ^= chunk->digits[i];
                h *= 0xFF51AFD7ED558CCDULL;
                h ^= h >> 32;
            }
        }
        return static_cast<proto_ulong>(h);
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
            method(context, self, next);
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

     bool isSmallInteger(const ProtoObject* obj) { if (!obj) return false; ProtoObjectPointer p; p.oid = obj; return p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && p.op.embedded_type == EMBEDDED_TYPE_SMALLINT; }
     bool isLargeInteger(const ProtoObject* obj) { if (!obj) return false; ProtoObjectPointer p; p.oid = obj; return p.op.pointer_tag == POINTER_TAG_LARGE_INTEGER; }
     bool isInteger(const ProtoObject* obj) { if (!obj) return false; return isSmallInteger(obj) || isLargeInteger(obj); }
     bool isCell(const ProtoObject* obj) {
         if (!obj) return false;
         ProtoObjectPointer pa{}; pa.oid = obj;
         switch (pa.op.pointer_tag) {
             case POINTER_TAG_OBJECT:
             case POINTER_TAG_LIST:
             case POINTER_TAG_LIST_SMALL:
              case POINTER_TAG_METHOD:
             case POINTER_TAG_LARGE_INTEGER:
             case POINTER_TAG_DOUBLE:
             case POINTER_TAG_STRING:
             case POINTER_TAG_BYTE_BUFFER:
             case POINTER_TAG_TUPLE:
             case POINTER_TAG_SET:
                 return true;
             default:
                 return false;
         }
     }
     bool isObject(const ProtoObject* obj) {
         if (!obj) return false;
         ProtoObjectPointer pa{}; pa.oid = obj;
         if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return false;
         return toImpl<const Cell>(obj)->getType() == CellType::Object;
     }

} // namespace proto
