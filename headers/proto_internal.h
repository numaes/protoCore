/*
 * proto_internal.h
 *
 *  Created on: November 2017 - Redesign January 2024
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 */

#ifndef PROTO_INTERNAL_H
#define PROTO_INTERNAL_H

#include "../headers/protoCore.h"
#include <thread>
#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include <climits>


// Method cache per thread. It should be a power of 2
#define THREAD_CACHE_DEPTH 1024
// a Tuple maximum count of pointers
#define TUPLE_SIZE 5

namespace proto
{
    // Forward declarations for implementation classes
    class Cell;
    class BigCell;
    class ProtoObjectCell;
    class ParentLinkImplementation;
    class ProtoListImplementation;
    class ProtoListIteratorImplementation;
    class ProtoSparseListImplementation;
    class ProtoSparseListIteratorImplementation;
    class TupleDictionary;
    class ProtoTupleImplementation;
    class ProtoTupleIteratorImplementation;
    class ProtoStringImplementation;
    class ProtoStringIteratorImplementation;
    class ProtoByteBufferImplementation;
    class ProtoExternalPointerImplementation;
    class ProtoThreadImplementation;
    class ProtoMethodCell;
    class ProtoExternalPointerImplementation;
    class ProtoByteBufferImplementation;
    class ProtoThreadImplementation;
    class ProtoThreadExtension;
    class LargeIntegerImplementation;
    class DoubleImplementation;

    // Union for pointer tagging
    union ProtoObjectPointer
    {
        struct
        {
            const ProtoObject* oid;
        } oid;

        void* voidPointer;

        ProtoMethod method;
        const ProtoObjectCell* objectCell;
        const ProtoSparseList* sparseList;
        const ProtoSparseListIterator* sparseListIterator;
        const ProtoList* list;
        const ProtoListIterator* listIterator;
        const ProtoTuple* tuple;
        const ProtoTupleIterator* tupleIterator;
        const ProtoString* string;
        const ProtoStringIterator* stringIterator;
        const ProtoByteBuffer* byteBuffer;
        const ProtoExternalPointer* externalPointer;
        const ProtoThread* thread;
        const LargeIntegerImplementation* largeInteger;
        const DoubleImplementation* protoDouble;

        const ProtoMethodCell* methodCellImplementation;
        const ProtoObjectCell* objectCellImplementation;
        const ProtoSparseListImplementation* sparseListImplementation;
        const ProtoSparseListIteratorImplementation*sparseListIteratorImplementation;
        const ProtoListImplementation* listImplementation;
        const ProtoListIteratorImplementation* listIteratorImplementation;
        const ProtoTupleImplementation* tupleImplementation;
        const ProtoTupleIteratorImplementation* tupleIteratorImplementation;
        const ProtoStringImplementation* stringImplementation;
        const ProtoStringIteratorImplementation* stringIteratorImplementation;
        const ProtoByteBufferImplementation* byteBufferImplementation;
        const ProtoExternalPointerImplementation* externalPointerImplementation;
        const ProtoThreadImplementation* threadImplementation;
        const LargeIntegerImplementation* largeIntegerImplementation;
        const DoubleImplementation* doubleImplementation;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned long value : 56;
        } op;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            long smallInteger : 56;
        } si;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned long floatValue : 32;
        } sd;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned long unicodeValue : 56;
        } unicodeChar;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned long booleanValue : 1;
            unsigned long padding : 55;
        } booleanValue;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            char byteData;
        } byteValue;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned long year : 16;
            unsigned long month : 8;
            unsigned long day : 8;
            unsigned long padding : 24;
        } date;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned long timestamp : 56;
        } timestampValue;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            long timedelta : 56;
        } timedeltaValue;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long hash : 60;
        } asHash;

        struct
        {
            unsigned long pointer_tag : 4;
            Cell* cell;
        } cell;
    };

    class AllocatedSegment
    {
    public:
        BigCell* memoryBlock;
        int cellsCount;
        AllocatedSegment* nextBlock;
    };

    class DirtySegment
    {
    public:
        BigCell* cellChain;
        DirtySegment* nextSegment;
    };

    // Pointer tags
#define POINTER_TAG_OBJECT                  0
#define POINTER_TAG_EMBEDDED_VALUE          1
#define POINTER_TAG_LIST                    2
#define POINTER_TAG_LIST_ITERATOR           3
#define POINTER_TAG_TUPLE                   4
#define POINTER_TAG_TUPLE_ITERATOR          5
#define POINTER_TAG_STRING                  6
#define POINTER_TAG_STRING_ITERATOR         7
#define POINTER_TAG_SPARSE_LIST             8
#define POINTER_TAG_SPARSE_LIST_ITERATOR    9
#define POINTER_TAG_BYTE_BUFFER             10
#define POINTER_TAG_EXTERNAL_POINTER        11
#define POINTER_TAG_METHOD                  12
#define POINTER_TAG_THREAD                  13
#define POINTER_TAG_LARGE_INTEGER           14
#define POINTER_TAG_DOUBLE                  15


    // Embedded types
#define EMBEDDED_TYPE_SMALLINT               0
#define EMBEDDED_TYPE_FLOAT                  1 // This remains for the 32-bit embedded float
#define EMBEDDED_TYPE_UNICODE_CHAR           2
#define EMBEDDED_TYPE_BOOLEAN                3
#define EMBEDDED_TYPE_BYTE                   4
#define EMBEDDED_TYPE_DATE                   5
#define EMBEDDED_TYPE_TIMESTAMP              6
#define EMBEDDED_TYPE_TIMEDELTA              7

    // Iterator states
#define ITERATOR_NEXT_PREVIOUS              0
#define ITERATOR_NEXT_THIS                  1
#define ITERATOR_NEXT_NEXT                  2

    // Space states
#define SPACE_STATE_RUNNING                 0
#define SPACE_STATE_STOPPING_WORLD          1
#define SPACE_STATE_WORLD_TO_STOP           2
#define SPACE_STATE_WORLD_STOPPED           3
#define SPACE_STATE_ENDING                  4

    // Thread states
#define THREAD_STATE_UNMANAGED              0
#define THREAD_STATE_MANAGED                1
#define THREAD_STATE_STOPPING               2
#define THREAD_STATE_STOPPED                3
#define THREAD_STATE_ENDED                  4

#define TYPE_SHIFT                          4

    // Template to convert from a public API pointer to an implementation pointer
    template <typename Impl, typename Api>
    inline Impl* toImpl(Api* ptr)
    {
        ProtoObjectPointer p{};
        p.oid.oid = reinterpret_cast<const ProtoObject*>(ptr);
        p.op.pointer_tag = 0;
        return static_cast<Impl*>(p.voidPointer);
    }

    // Template to convert from a constant public API pointer
    template <typename Impl, typename Api>
    inline const Impl* toImpl(const Api* ptr)
    {
        return reinterpret_cast<const Impl*>(ptr);
    }

    unsigned long generate_mutable_ref();

    class Cell
    {
    public:
        explicit Cell(ProtoContext* context, Cell* next = nullptr);
        virtual ~Cell() = default;

        virtual void finalize(ProtoContext* context) const;
        virtual void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const;
        virtual unsigned long getHash(ProtoContext* context) const;
        virtual const ProtoObject* implAsObject(ProtoContext* context) const;
        static void* operator new(unsigned long size, ProtoContext* context);
        virtual const Cell* asCell(ProtoContext* context) const;
        Cell* next;
    };

    class LargeIntegerImplementation final : public Cell
    {
    public:
        static const int DIGIT_COUNT = 5;

        bool is_negative;
        const LargeIntegerImplementation* next;
        unsigned long digits[DIGIT_COUNT];

        LargeIntegerImplementation(ProtoContext* context);
        ~LargeIntegerImplementation() override;

        unsigned long getHash(ProtoContext* context) const override;
        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
    };

    class DoubleImplementation final : public Cell
    {
    public:
        double value;

        DoubleImplementation(ProtoContext* context, double val);
        ~DoubleImplementation() override;

        unsigned long getHash(ProtoContext* context) const override;
        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
    };


    class Integer
    {
    public:
        //- Factory Methods
        static const ProtoObject* fromLong(ProtoContext* context, long long value);
        static const ProtoObject* fromString(ProtoContext* context, const char* str, int base = 10);

        //- Conversion & Inspection
        static long long asLong(ProtoContext* context, const ProtoObject* object);
        static const ProtoString* toString(ProtoContext* context, const ProtoObject* object, int base = 10);
        static int sign(ProtoContext* context, const ProtoObject* object);

        //- Unary Operations
        static const ProtoObject* negate(ProtoContext* context, const ProtoObject* object);
        static const ProtoObject* abs(ProtoContext* context, const ProtoObject* object);

        //- Arithmetic Operations
        static const ProtoObject* add(ProtoContext* context, const ProtoObject* left, const ProtoObject* right);
        static const ProtoObject* subtract(ProtoContext* context, const ProtoObject* left, const ProtoObject* right);
        static const ProtoObject* multiply(ProtoContext* context, const ProtoObject* left, const ProtoObject* right);
        static const ProtoObject* divide(ProtoContext* context, const ProtoObject* left, const ProtoObject* right);
        static const ProtoObject* modulo(ProtoContext* context, const ProtoObject* left, const ProtoObject* right);

        //- Comparison
        static int compare(ProtoContext* context, const ProtoObject* left, const ProtoObject* right);

        //- Bitwise Operations
        static const ProtoObject* bitwiseAnd(ProtoContext* context, const ProtoObject* left, const ProtoObject* right);
        static const ProtoObject* bitwiseOr(ProtoContext* context, const ProtoObject* left, const ProtoObject* right);
        static const ProtoObject* bitwiseXor(ProtoContext* context, const ProtoObject* left, const ProtoObject* right);
        static const ProtoObject* bitwiseNot(ProtoContext* context, const ProtoObject* object);
        static const ProtoObject* shiftLeft(ProtoContext* context, const ProtoObject* object, int amount);
        static const ProtoObject* shiftRight(ProtoContext* context, const ProtoObject* object, int amount);
    };


    class ParentLinkImplementation final : public Cell { /* ... */ };
    class ProtoObjectCell final : public Cell { /* ... */ };
    class ProtoListIteratorImplementation final : public Cell { /* ... */ };
    class ProtoListImplementation final : public Cell { /* ... */ };
    class ProtoSparseListIteratorImplementation final : public Cell { /* ... */ };
    class ProtoSparseListImplementation final : public Cell { /* ... */ };
    class TupleDictionary final : public Cell { /* ... */ };
    class ProtoTupleIteratorImplementation final : public Cell { /* ... */ };
    class ProtoTupleImplementation final : public Cell { /* ... */ };
    class ProtoStringIteratorImplementation final : public Cell { /* ... */ };
    class ProtoStringImplementation final : public Cell { /* ... */ };
    class ProtoByteBufferImplementation final : public Cell { /* ... */ };
    class ProtoMethodCell : public Cell { /* ... */ };
    class ProtoExternalPointerImplementation final : public Cell { /* ... */ };
    class ProtoThreadImplementation final : public Cell { /* ... */ };
    class ProtoThreadExtension final : public Cell { /* ... */ };


    // Update BigCell to include the new type
    class BigCell final
    {
        union {
            char byteData[64] = {};
            ProtoObjectCell objectCell;
            ProtoListIteratorImplementation listIteratorCell;
            ProtoListImplementation listCell;
            ProtoSparseListIteratorImplementation sparseListIteratorCell;
            ProtoSparseListImplementation sparseListCell;
            ProtoTupleIteratorImplementation tupleIteratorCell;
            ProtoTupleImplementation tupleCell;
            ProtoStringIteratorImplementation stringIteratorCell;
            ProtoStringImplementation stringCell;
            ProtoByteBufferImplementation byteBufferCell;
            ProtoMethodCell methodCell;
            ProtoExternalPointerImplementation externalPointerCell;
            ProtoThreadImplementation threadCell;
            ProtoThreadExtension threadExtensionCell;
            LargeIntegerImplementation largeIntegerCell;
            DoubleImplementation doubleCell;
        };
    };

    static_assert(sizeof(BigCell) <= 64, "BigCell exceeds 64 bytes!!!!");
    static_assert(sizeof(LargeIntegerImplementation) <= 64, "LargeIntegerImplementation exceeds 64 bytes!");
    static_assert(sizeof(DoubleImplementation) <= 64, "DoubleImplementation exceeds 64 bytes!");

} // namespace proto

#endif /* PROTO_INTERNAL_H */
