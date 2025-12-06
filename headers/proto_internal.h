/*
 * proto_internal.h
 *
 *  Created on: November 2017 - Redesign January 2024
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 */

#ifndef PROTO_INTERNAL_H
#define PROTO_INTERNAL_H

#include "protoCore.h"
#include <thread>
#include <memory>
#include <string>
#include <algorithm>
#include <climits>
#include <vector>
#include <functional>

#define THREAD_CACHE_DEPTH 1024
#define TUPLE_SIZE 5

namespace proto
{
    // Forward Declarations
    class Cell;
    class BigCell;
    class ProtoObjectCell;
    class ParentLinkImplementation;
    class ProtoListImplementation;
    class ProtoListIteratorImplementation;
    class ProtoSparseListImplementation;
    class ProtoSparseListIteratorImplementation;
    class ProtoSetImplementation;
    class ProtoSetIteratorImplementation;
    class ProtoMultisetImplementation;
    class ProtoMultisetIteratorImplementation;
    class ProtoTupleImplementation;
    class ProtoStringImplementation;
    class ProtoByteBufferImplementation;
    class ProtoExternalPointerImplementation;
    class ProtoMethodCell;
    class ProtoThreadImplementation;
    class ProtoThreadExtension;
    class LargeIntegerImplementation;
    class DoubleImplementation;
    class Integer;
    class ReturnReference;

    // Pointer Tagging
    union ProtoObjectPointer {
        const ProtoObject* oid;
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
        const ProtoSet* set;
        const ProtoSetIterator* setIterator;
        const ProtoMultiset* multiset;
        const ProtoMultisetIterator* multisetIterator;
        const LargeIntegerImplementation* largeInteger;
        const DoubleImplementation* protoDouble;
        const ProtoMethodCell* methodCellImplementation;
        const ProtoObjectCell* objectCellImplementation;
        const ProtoSparseListImplementation* sparseListImplementation;
        const ProtoSparseListIteratorImplementation*sparseListIteratorImplementation;
        const ProtoListImplementation* listImplementation;
        const ProtoListIteratorImplementation* listIteratorImplementation;
        const ProtoTupleImplementation* tupleImplementation;
        const ProtoStringImplementation* stringImplementation;
        const ProtoByteBufferImplementation* byteBufferImplementation;
        const ProtoExternalPointerImplementation* externalPointerImplementation;
        const ProtoThreadImplementation* threadImplementation;
        const ProtoSetImplementation* setImplementation;
        const ProtoSetIteratorImplementation* setIteratorImplementation;
        const ProtoMultisetImplementation* multisetImplementation;
        const ProtoMultisetIteratorImplementation* multisetIteratorImplementation;
        const LargeIntegerImplementation* largeIntegerImplementation;
        const DoubleImplementation* doubleImplementation;

        struct { unsigned long pointer_tag : 6; unsigned long embedded_type : 4; unsigned long value : 54; } op;
        struct { unsigned long pointer_tag : 6; unsigned long embedded_type : 4; long smallInteger : 54; } si;
        struct { unsigned long pointer_tag : 6; unsigned long embedded_type : 4; unsigned long unicodeValue : 54; } unicodeChar;
        struct { unsigned long pointer_tag : 6; unsigned long embedded_type : 4; unsigned long booleanValue : 1; } booleanValue;
        struct { unsigned long pointer_tag : 6; unsigned long embedded_type : 4; unsigned long byteData : 8; } byteValue;
        struct { unsigned long pointer_tag : 6; unsigned long embedded_type : 4; unsigned long year : 16; unsigned long month : 8; unsigned long day : 8; } date;
        struct { unsigned long pointer_tag : 6; unsigned long embedded_type : 4; unsigned long timestamp : 54; } timestampValue;
        struct { unsigned long pointer_tag : 6; unsigned long embedded_type : 4; long timedelta : 54; } timedeltaValue;
        struct { unsigned long pointer_tag : 6; unsigned long hash : 58; } asHash;
    };

    #define POINTER_TAG_OBJECT 0
    #define POINTER_TAG_EMBEDDED_VALUE 1
    #define POINTER_TAG_LIST 2
    #define POINTER_TAG_LIST_ITERATOR 3
    #define POINTER_TAG_TUPLE 4
    #define POINTER_TAG_TUPLE_ITERATOR 5
    #define POINTER_TAG_STRING 6
    #define POINTER_TAG_STRING_ITERATOR 7
    #define POINTER_TAG_SPARSE_LIST 8
    #define POINTER_TAG_SPARSE_LIST_ITERATOR 9
    #define POINTER_TAG_BYTE_BUFFER 10
    #define POINTER_TAG_EXTERNAL_POINTER 11
    #define POINTER_TAG_METHOD 12
    #define POINTER_TAG_THREAD 13
    #define POINTER_TAG_LARGE_INTEGER 14
    #define POINTER_TAG_DOUBLE 15
    #define POINTER_TAG_SET 16
    #define POINTER_TAG_MULTISET 17
    #define POINTER_TAG_SET_ITERATOR 18
    #define POINTER_TAG_MULTISET_ITERATOR 19

    #define EMBEDDED_TYPE_SMALLINT 0
    #define EMBEDDED_TYPE_UNICODE_CHAR 2
    #define EMBEDDED_TYPE_BOOLEAN 3

    #define ITERATOR_NEXT_PREVIOUS 0
    #define ITERATOR_NEXT_THIS 1
    #define ITERATOR_NEXT_NEXT 2

    template <typename Impl, typename Api> inline const Impl* toImpl(const Api* ptr) { return reinterpret_cast<const Impl*>(ptr); }
    unsigned long generate_mutable_ref();

    class Cell {
    public:
        Cell* next;
        explicit Cell(ProtoContext* context, Cell* n = nullptr);
        virtual ~Cell() = default;
        virtual void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const;
        virtual unsigned long getHash(ProtoContext* context) const;
        virtual const ProtoObject* implAsObject(ProtoContext* context) const = 0;
        static void* operator new(size_t size, ProtoContext* context);
    };

    class ReturnReference : public Cell {
    public:
        Cell* returnValue;
        ReturnReference(ProtoContext* context, Cell* rv);
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
    };

    class ProtoListImplementation final : public Cell {
    public:
        const ProtoObject* value;
        const ProtoListImplementation* previousNode;
        const ProtoListImplementation* nextNode;
        const unsigned long hash;
        const unsigned long size;
        const unsigned char height;
        const bool isEmpty;

        explicit ProtoListImplementation(ProtoContext* context, const ProtoObject* v, bool empty, const ProtoListImplementation* prev, const ProtoListImplementation* next);
        const ProtoObject* implGetAt(ProtoContext* context, int index) const;
        bool implHas(ProtoContext* context, const ProtoObject* targetValue) const;
        const ProtoListImplementation* implSetAt(ProtoContext* context, int index, const ProtoObject* newValue) const;
        const ProtoListImplementation* implInsertAt(ProtoContext* context, int index, const ProtoObject* newValue) const;
        const ProtoListImplementation* implAppendLast(ProtoContext* context, const ProtoObject* newValue) const;
        const ProtoListImplementation* implRemoveAt(ProtoContext* context, int index) const;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        const ProtoList* asProtoList(ProtoContext* context) const;
        const ProtoListIteratorImplementation* implGetIterator(ProtoContext* context) const;
    };

    class ProtoListIteratorImplementation final : public Cell {
    public:
        const ProtoListImplementation* base;
        unsigned long currentIndex;
        ProtoListIteratorImplementation(ProtoContext* context, const ProtoListImplementation* b, unsigned long index);
        int implHasNext() const;
        const ProtoObject* implNext(ProtoContext* context) const;
        const ProtoListIteratorImplementation* implAdvance(ProtoContext* context) const;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        const ProtoListIterator* asProtoListIterator(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
    };

    class ProtoSparseListImplementation final : public Cell {
    public:
        const unsigned long key;
        const ProtoObject* value;
        const ProtoSparseListImplementation* previous;
        const ProtoSparseListImplementation* next;
        const unsigned long hash;
        const unsigned long size;
        const unsigned char height;
        const bool isEmpty;

        ProtoSparseListImplementation(ProtoContext* context, unsigned long k, const ProtoObject* v, const ProtoSparseListImplementation* p, const ProtoSparseListImplementation* n, bool empty);
        bool implHas(ProtoContext* context, unsigned long offset) const;
        const ProtoObject* implGetAt(ProtoContext* context, unsigned long offset) const;
        const ProtoSparseListImplementation* implSetAt(ProtoContext* context, unsigned long offset, const ProtoObject* newValue) const;
        const ProtoSparseListImplementation* implRemoveAt(ProtoContext* context, unsigned long offset) const;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        const ProtoSparseList* asSparseList(ProtoContext* context) const;
        const ProtoSparseListIteratorImplementation* implGetIterator(ProtoContext* context) const;
    };

    class ProtoSparseListIteratorImplementation final : public Cell {
        friend class ProtoSparseListImplementation;
    private:
        const int state;
        const ProtoSparseListImplementation* current;
        const ProtoSparseListIteratorImplementation* queue;
    public:
        ProtoSparseListIteratorImplementation(ProtoContext* context, int s, const ProtoSparseListImplementation* c, const ProtoSparseListIteratorImplementation* q);
        int implHasNext() const;
        unsigned long implNextKey() const;
        const ProtoObject* implNextValue() const;
        const ProtoSparseListIteratorImplementation* implAdvance(ProtoContext* context) const;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
    };

    class Integer {
    public:
        static const ProtoObject* fromLong(ProtoContext* context, long long value);
        static const ProtoObject* fromString(ProtoContext* context, const char* str, int base);
        static long long asLong(ProtoContext* context, const ProtoObject* object);
    };
}

#endif //PROTO_INTERNAL_H
