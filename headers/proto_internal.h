/*
 * proto_internal.h
 *
 *  Created on: November 2017 - Redesign January 2024
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 */

#ifndef PROTO_INTERNAL_H
#define PROTO_INTERNAL_H

#include "../headers/proto.h"

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

    // Union for pointer tagging
    union ProtoObjectPointer
    {
        struct
        {
            ProtoObject* oid;
        } oid;

        void* voidPointer;

        ProtoMethod method;
        ProtoObjectCell* objectCell;
        ProtoSparseList* sparseList;
        ProtoSparseListIterator* sparseListIterator;
        ProtoList* list;
        ProtoListIterator* listIterator;
        ProtoTuple* tuple;
        ProtoTupleIterator* tupleIterator;
        ProtoString* string;
        ProtoStringIterator* stringIterator;
        ProtoByteBuffer* byteBuffer;
        ProtoExternalPointer* externalPointer;
        ProtoThread* thread;

        ProtoMethodCell* methodCellImplementation;
        ProtoObjectCell* objectCellImplementation;
        ProtoSparseListImplementation* sparseListImplementation;
        ProtoSparseListIteratorImplementation *sparseListIteratorImplementation;
        ProtoListImplementation* listImplementation;
        ProtoListIteratorImplementation* listIteratorImplementation;
        ProtoTupleImplementation* tupleImplementation;
        ProtoTupleIteratorImplementation* tupleIteratorImplementation;
        ProtoStringImplementation* stringImplementation;
        ProtoStringIteratorImplementation* stringIteratorImplementation;
        ProtoByteBufferImplementation* byteBufferImplementation;
        ProtoExternalPointerImplementation* externalPointerImplementation;
        ProtoThreadImplementation* threadImplementation;

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

    // Embedded types
#define EMBEDDED_TYPE_SMALLINT               0
#define EMBEDDED_TYPE_FLOAT                  1
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
        p.oid.oid = reinterpret_cast<ProtoObject *>(ptr);
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
        virtual ~Cell() = default;
        // ...
        virtual unsigned long getHash(ProtoContext* context) const;
        virtual ProtoObject* asObject(ProtoContext* context) const;
        // ...
    };

    class ParentLinkImplementation final : public Cell, public ParentLink
    {
    public:
        // ...
    };

    class ProtoObjectCell final : public Cell, public ProtoObject
    {
    public:
        ProtoObjectCell(
            ProtoContext* context,
            ParentLinkImplementation* parent,
            ProtoSparseListImplementation* attributes,
            unsigned long mutable_ref
        );
        ~ProtoObjectCell() override;

        ProtoObjectCell* addParent(
            ProtoContext* context,
            ProtoObject* newParentToAdd
        ) const;

        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, Cell* cell)
        );
        long unsigned getHash(ProtoContext* context) const override;

        ParentLinkImplementation* parent;
        unsigned long mutable_ref;
        ProtoSparseListImplementation* attributes;
    };

    class ProtoListIteratorImplementation final : public Cell, public ProtoListIterator
    {
    public:
        ProtoListIteratorImplementation(
            ProtoContext* context,
            const ProtoListImplementation* base,
            unsigned long currentIndex
        );
        ~ProtoListIteratorImplementation() override;

        int implHasNext(ProtoContext* context) const;
        ProtoObject* implNext(ProtoContext* context);
        ProtoListIteratorImplementation* implAdvance(ProtoContext* context);
        ProtoObject* implAsObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const override;

        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, Cell*)
        );

    private:
        const ProtoListImplementation* base;
        unsigned long currentIndex;
    };

    class ProtoListImplementation final : public Cell, public ProtoList
    {
    public:
        explicit ProtoListImplementation(
            ProtoContext* context,
            const ProtoObject* value = PROTO_NONE,
            const ProtoListImplementation* previous = nullptr,
            const ProtoListImplementation* next = nullptr
        );
        ~ProtoListImplementation() override;

        ProtoObject* implGetAt(ProtoContext* context, int index) const;
        ProtoObject* implGetFirst(ProtoContext* context) const;
        ProtoObject* implGetLast(ProtoContext* context) const;
        ProtoListImplementation* implGetSlice(ProtoContext* context, int from, int to) const;
        unsigned long implGetSize(ProtoContext* context) const;
        bool implHas(ProtoContext* context, const ProtoObject* targetValue) const;
        ProtoListImplementation* implSetAt(ProtoContext* context, int index, ProtoObject* value = PROTO_NONE) const;
        ProtoListImplementation* implInsertAt(ProtoContext* context, int index, ProtoObject* newValue) const;
        ProtoListImplementation* implAppendFirst(ProtoContext* context, ProtoObject* newValue) const;
        ProtoListImplementation* implAppendLast(ProtoContext* context, ProtoObject* newValue) const;
        ProtoListImplementation* implExtend(ProtoContext* context, const ProtoListImplementation* other) const;
        ProtoListImplementation* implSplitFirst(ProtoContext* context, int index) const;
        ProtoListImplementation* implSplitLast(ProtoContext* context, int index) const;
        ProtoListImplementation* implRemoveFirst(ProtoContext* context) const;
        ProtoListImplementation* implRemoveLast(ProtoContext* context) const;
        ProtoListImplementation* implRemoveAt(ProtoContext* context, int index) const;
        ProtoListImplementation* implRemoveSlice(ProtoContext* context, int from, int to) const;
        ProtoObject* implAsObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const override;
        ProtoListIteratorImplementation* implGetIterator(ProtoContext* context) const;

        // ...
    };

    class ProtoSparseListIteratorImplementation final : public Cell, public ProtoSparseListIterator
    {
    public:
        // ...
        int implHasNext(ProtoContext* context) const;
        unsigned long implNextKey(ProtoContext* context) const;
        ProtoObject* implNextValue(ProtoContext* context) const;
        ProtoSparseListIteratorImplementation* implAdvance(ProtoContext* context);
        ProtoObject* implAsObject(ProtoContext* context) const;
        // ...
    };

    class ProtoSparseListImplementation final : public Cell, public ProtoSparseList
    {
    public:
        // ...
        bool implHas(ProtoContext* context, unsigned long offset) const;
        ProtoObject* implGetAt(ProtoContext* context, unsigned long offset) const;
        ProtoSparseListImplementation* implSetAt(ProtoContext* context, unsigned long offset, ProtoObject* newValue) const;
        ProtoSparseListImplementation* implRemoveAt(ProtoContext* context, unsigned long offset) const;
        bool implIsEqual(ProtoContext* context, const ProtoSparseListImplementation* otherDict) const;
        unsigned long implGetSize(ProtoContext* context) const;
        ProtoObject* implAsObject(ProtoContext* context) const;
        ProtoSparseListIteratorImplementation* implGetIterator(ProtoContext* context) const;
        void implProcessElements(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, unsigned long, ProtoObject*)
        ) const;
        void implProcessValues(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, ProtoObject*)
        ) const;
        // ...
    };

    class ProtoTupleIteratorImplementation final : public Cell, public ProtoTupleIterator
    {
    public:
        // ...
        int implHasNext(ProtoContext* context) const;
        ProtoObject* implNext(ProtoContext* context);
        ProtoTupleIteratorImplementation* implAdvance(ProtoContext* context);
        ProtoObject* implAsObject(ProtoContext* context) const;
        // ...
    };

    class ProtoTupleImplementation final : public Cell, public ProtoTuple
    {
    public:
        // ...
        ProtoObject* implGetAt(ProtoContext* context, int index) const;
        ProtoObject* implGetFirst(ProtoContext* context) const;
        ProtoObject* implGetLast(ProtoContext* context) const;
        ProtoTupleImplementation* implGetSlice(ProtoContext* context, int from, int to) const;
        unsigned long implGetSize(ProtoContext* context) const;
        ProtoList* implAsList(ProtoContext* context) const;
        static ProtoTupleImplementation* tupleFromList(ProtoContext* context, const ProtoList* list);
        ProtoTupleIteratorImplementation* implGetIterator(ProtoContext* context) const;
        ProtoTupleImplementation* implSetAt(ProtoContext* context, int index, ProtoObject* value) const;
        bool implHas(ProtoContext* context, ProtoObject* value) const;
        ProtoTupleImplementation* implInsertAt(ProtoContext* context, int index, ProtoObject* value) const;
        ProtoTupleImplementation* implAppendFirst(ProtoContext* context, const ProtoTuple* otherTuple) const;
        ProtoTupleImplementation* implAppendLast(ProtoContext* context, const ProtoTuple* otherTuple) const;
        ProtoTupleImplementation* implSplitFirst(ProtoContext* context, int count) const;
        ProtoTupleImplementation* implSplitLast(ProtoContext* context, int count) const;
        ProtoTupleImplementation* implRemoveFirst(ProtoContext* context, int count) const;
        ProtoTupleImplementation* implRemoveLast(ProtoContext* context, int count) const;
        ProtoTupleImplementation* implRemoveAt(ProtoContext* context, int index) const;
        ProtoTupleImplementation* implRemoveSlice(ProtoContext* context, int from, int to) const;
        ProtoObject* implAsObject(ProtoContext* context) const;
        // ...
    };

    class ProtoStringIteratorImplementation final : public Cell, public ProtoStringIterator
    {
    public:
        // ...
        int implHasNext(ProtoContext* context) const;
        ProtoObject* implNext(ProtoContext* context);
        ProtoStringIteratorImplementation* implAdvance(ProtoContext* context);
        ProtoObject* implAsObject(ProtoContext* context) const;
        // ...
    };

    class ProtoStringImplementation final : public Cell, public ProtoString
    {
    public:
        // ...
        int implCmpToString(ProtoContext* context, const ProtoString* otherString) const;
        ProtoObject* implGetAt(ProtoContext* context, int index) const;
        unsigned long implGetSize(ProtoContext* context) const;
        ProtoStringImplementation* implGetSlice(ProtoContext* context, int from, int to) const;
        ProtoStringImplementation* implSetAt(ProtoContext* context, int index, ProtoObject* value) const;
        ProtoStringImplementation* implInsertAt(ProtoContext* context, int index, ProtoObject* value) const;
        ProtoStringImplementation* implAppendLast(ProtoContext* context, const ProtoString* otherString) const;
        ProtoStringImplementation* implAppendFirst(ProtoContext* context, const ProtoString* otherString) const;
        ProtoStringImplementation* implRemoveSlice(ProtoContext* context, int from, int to) const;
        ProtoListImplementation* implAsList(ProtoContext* context) const;
        ProtoStringIteratorImplementation* implGetIterator(ProtoContext* context) const;
        ProtoStringImplementation* implSetAtString(ProtoContext* context, int index, const ProtoString* otherString) const;
        ProtoStringImplementation* implInsertAtString(ProtoContext* context, int index, const ProtoString* otherString) const;
        ProtoStringImplementation* implSplitFirst(ProtoContext* context, int count) const;
        ProtoStringImplementation* implSplitLast(ProtoContext* context, int count) const;
        ProtoStringImplementation* implRemoveFirst(ProtoContext* context, int count) const;
        ProtoStringImplementation* implRemoveLast(ProtoContext* context, int count) const;
        ProtoStringImplementation* implRemoveAt(ProtoContext* context, int index) const;
        ProtoObject* implAsObject(ProtoContext* context) const;
        // ...
    };

    class ProtoByteBufferImplementation final : public Cell, public ProtoByteBuffer
    {
    public:
        // ...
        char implGetAt(ProtoContext* context, int index) const;
        void implSetAt(ProtoContext* context, int index, char value);
        unsigned long implGetSize(ProtoContext* context) const;
        char* implGetBuffer(ProtoContext* context) const;
        ProtoObject* implAsObject(ProtoContext* context) const;
        // ...
    };

    class ProtoMethodCell final : public Cell
    {
    public:
        // ...
        ProtoObject* implAsObject(ProtoContext* context) const;
        ProtoObject* implGetSelf(ProtoContext* context) const;
        ProtoMethod implGetMethod(ProtoContext* context) const;
        // ...
    };

    class ProtoExternalPointerImplementation final : public Cell, public ProtoExternalPointer
    {
    public:
        // ...
        void* implGetPointer(ProtoContext* context) const;
        ProtoObject* implAsObject(ProtoContext* context) const;
        // ...
    };

    class ProtoThreadImplementation final : public Cell, public ProtoThread
    {
    public:
        // ...
        void implDetach(ProtoContext* context) const;
        void implJoin(ProtoContext* context) const;
        [[nodiscard]] ProtoContext* implGetCurrentContext() const;
        ProtoObject* implAsObject(ProtoContext* context) const;
        static ProtoThread* implGetCurrentThread(const ProtoContext* context);
        // ...
    };
}

#endif /* PROTO_INTERNAL_H */
