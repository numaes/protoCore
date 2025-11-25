/*
 * proto_internal.h
 *
 *  Created on: November 2017 - Redesign January 2024
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 */

#ifndef PROTO_INTERNAL_H
#define PROTO_INTERNAL_H

#include "../headers/proto.h"
#include <thread>
#include <memory>

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
    class ProtoThreadExtension;

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

        // ...
        virtual void finalize(ProtoContext* context) const;
        virtual void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const;
        // ...
        virtual unsigned long getHash(ProtoContext* context) const;
        virtual const ProtoObject* implAsObject(ProtoContext* context) const; // Already declared
        static void* operator new(unsigned long size, ProtoContext* context);
        virtual const Cell* asCell(ProtoContext* context) const;
        // ...
        Cell* next;
    };

    class ParentLinkImplementation final : public Cell
    {
    public:
        ParentLinkImplementation(ProtoContext* context, const ParentLinkImplementation* parent, const ProtoObject* object);
        ~ParentLinkImplementation() override;
        const ProtoObject* getObject(ProtoContext* context) const;
        const ParentLinkImplementation* getParent(ProtoContext* context) const;

        void finalize(ProtoContext* context) const override;
        virtual void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;
        // ...

        const ParentLinkImplementation* parent;
        const ProtoObject* object;
    };

    class ProtoObjectCell final : public Cell
    {
    public:
        ProtoObjectCell(
            ProtoContext* context,
            const ParentLinkImplementation* parent,
            const ProtoSparseListImplementation* attributes,
            unsigned long mutable_ref
        );
        ~ProtoObjectCell() override;

        const ProtoObjectCell* addParent(
            ProtoContext* context,
            const ProtoObject* newParentToAdd
        ) const;

        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;
        const ProtoObject* asObject(ProtoContext* context) const;

        const ParentLinkImplementation* parent;
        unsigned long mutable_ref;
        const ProtoSparseListImplementation* attributes;
    };

    class ProtoListIteratorImplementation final : public Cell
    {
    public:
        ProtoListIteratorImplementation(
            ProtoContext* context,
            const ProtoListImplementation* base,
            unsigned long currentIndex
        );
        ~ProtoListIteratorImplementation() override;

        int implHasNext(ProtoContext* context) const;
        const ProtoObject* implNext(ProtoContext* context) const;
        const ProtoListIteratorImplementation* implAdvance(ProtoContext* context) const;
        const ProtoObject* implAsObject(ProtoContext* context) const;
        const ProtoListIterator* asProtoListIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const override;

        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;

    private:
        const ProtoListImplementation* base;
        unsigned long currentIndex;
    };

    class ProtoListImplementation final : public Cell
    {
    public:
        explicit ProtoListImplementation(
            ProtoContext* context,
            const ProtoObject* value = PROTO_NONE,
            bool isEmpty = false,
            const ProtoListImplementation* previousNode = nullptr,
            const ProtoListImplementation* nextNode = nullptr
        );
        ~ProtoListImplementation() override;

        const ProtoObject* implGetAt(ProtoContext* context, int index) const;
        const ProtoObject* implGetFirst(ProtoContext* context) const;
        const ProtoObject* implGetLast(ProtoContext* context) const;
        const ProtoListImplementation* implGetSlice(ProtoContext* context, int from, int to) const;
        unsigned long implGetSize(ProtoContext* context) const;
        bool implHas(ProtoContext* context, const ProtoObject* targetValue) const;
        const ProtoListImplementation* implSetAt(ProtoContext* context, int index, const ProtoObject* newValue) const;
        const ProtoListImplementation* implInsertAt(ProtoContext* context, int index, ProtoObject* newValue) const;
        const ProtoListImplementation* implAppendFirst(ProtoContext* context, ProtoObject* newValue) const;
        const ProtoListImplementation* implAppendLast(ProtoContext* context, ProtoObject* newValue) const;
        const ProtoListImplementation* implExtend(ProtoContext* context, const ProtoListImplementation* other) const;
        const ProtoListImplementation* implSplitFirst(ProtoContext* context, int index) const;
        const ProtoListImplementation* implSplitLast(ProtoContext* context, int index) const;
        const ProtoListImplementation* implRemoveFirst(ProtoContext* context) const;
        const ProtoListImplementation* implRemoveLast(ProtoContext* context) const;
        const ProtoListImplementation* implRemoveAt(ProtoContext* context, int index) const;
        const ProtoListImplementation* implRemoveSlice(ProtoContext* context, int from, int to) const;
        const ProtoObject* implAsObject(ProtoContext* context) const;
        const ProtoList* asProtoList(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const override;
        const ProtoListIteratorImplementation* implGetIterator(ProtoContext* context) const;

        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;

        // ...
        const ProtoObject* value;
        const ProtoListImplementation* previousNode;
        const ProtoListImplementation* nextNode;
        unsigned long hash;
        unsigned long size:56;
        unsigned long height:7;
        bool isEmpty:1;
    };

    class ProtoSparseListIteratorImplementation final : public Cell
    {
    public:
        // ...
        ProtoSparseListIteratorImplementation(
            ProtoContext* context,
            const ProtoSparseListImplementation* base,
            int currentIndex = 0
        );
        ~ProtoSparseListIteratorImplementation() override;
        // ...
        bool implHas(ProtoContext* context, unsigned long offset) const;
        const ProtoObject* implGetAt(ProtoContext* context, unsigned long offset) const;
        const ProtoSparseListImplementation* implSetAt(ProtoContext* context, unsigned long offset, const ProtoObject* newValue) const;

        int implHasNext(ProtoContext* context) const;
        unsigned long implNextKey(ProtoContext* context) const;
        const ProtoObject* implNextValue(ProtoContext* context) const;
        const ProtoSparseListIteratorImplementation* implAdvance(ProtoContext* context);
        const ProtoObject* implAsObject(ProtoContext* context) const;
        const ProtoSparseListIterator* asProtoSparseListIterator(ProtoContext* context) const;
        // ...
        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;


    };

    class ProtoSparseListImplementation final : public Cell
    {
    public:
        // ...
        ProtoSparseListImplementation(
            ProtoContext* context,
            unsigned long key = 0,
            const ProtoObject* value = PROTO_NONE,
            const ProtoSparseListImplementation* previous = nullptr,
            const ProtoSparseListImplementation* next = nullptr
        );
        ~ProtoSparseListImplementation() override;
        // ...
        // ...
        bool implHas(ProtoContext* context, unsigned long offset) const;
        const ProtoObject* implGetAt(ProtoContext* context, unsigned long offset) const;
        const ProtoSparseListImplementation* implSetAt(ProtoContext* context, unsigned long offset, const ProtoObject* newValue) const;
        const ProtoSparseListImplementation* implRemoveAt(ProtoContext* context, unsigned long offset) const;
        bool implIsEqual(ProtoContext* context, const ProtoSparseListImplementation* otherDict) const;
        unsigned long implGetSize(ProtoContext* context) const;
        const ProtoObject* implAsObject(ProtoContext* context) const;
        const ProtoSparseList* asSparseList(ProtoContext* context) const;
        const ProtoSparseListIteratorImplementation* implGetIterator(ProtoContext* context) const;
        void implProcessElements(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, unsigned long, const ProtoObject*)
        ) const;
        void implProcessValues(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, const ProtoObject*)
        ) const;
        // ...

        unsigned long getHash(ProtoContext* context) const override;
        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;

        unsigned long key;
        const ProtoObject* value;
        const ProtoSparseListImplementation* previous;
        const ProtoSparseListImplementation* next;
    };

    class ProtoTupleIteratorImplementation final : public Cell
    {
    public:
        ProtoTupleIteratorImplementation(
            ProtoContext* context,
            const ProtoTupleImplementation* base,
            int currentIndex = 0);
        ~ProtoTupleIteratorImplementation() override;
        // ...
        int implHasNext(ProtoContext* context) const;
        const ProtoObject* implNext(ProtoContext* context) const;
        const ProtoTupleIteratorImplementation* implAdvance(ProtoContext* context);
        const ProtoObject* implAsObject(ProtoContext* context) const;
        const ProtoTupleIterator* asProtoTupleIterator(ProtoContext* context) const;
        // ...
        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;

        ProtoTupleImplementation* base;
        unsigned long currentIndex;
    };

    class ProtoTupleImplementation final : public Cell
    {
    public:
        ProtoTupleImplementation(
            ProtoContext* context,
            unsigned long count = 0,
            ProtoObject** data = nullptr,
            ProtoTupleImplementation** indirect = nullptr
        );
        ~ProtoTupleImplementation() override;

        // ...
        const ProtoObject* implGetAt(ProtoContext* context, int index) const;
        const ProtoObject* implGetFirst(ProtoContext* context) const;
        const ProtoObject* implGetLast(ProtoContext* context) const;
        const ProtoTupleImplementation* implGetSlice(ProtoContext* context, int from, int to) const;
        unsigned long implGetSize(ProtoContext* context) const;
        const ProtoList* implAsList(ProtoContext* context) const;
        static const ProtoTupleImplementation* tupleFromList(ProtoContext* context, const ProtoList* list);
        const ProtoTupleIteratorImplementation* implGetIterator(ProtoContext* context) const;
        const ProtoTupleImplementation* implSetAt(ProtoContext* context, int index, const ProtoObject* value) const;
        bool implHas(ProtoContext* context, const ProtoObject* value) const;
        const ProtoTupleImplementation* implInsertAt(ProtoContext* context, int index, const ProtoObject* value) const;
        const ProtoTupleImplementation* implAppendFirst(ProtoContext* context, const ProtoTuple* otherTuple) const;
        const ProtoTupleImplementation* implAppendLast(ProtoContext* context, const ProtoTuple* otherTuple) const;
        const ProtoTupleImplementation* implSplitFirst(ProtoContext* context, int count) const;
        const ProtoTupleImplementation* implSplitLast(ProtoContext* context, int count) const;
        const ProtoTupleImplementation* implRemoveFirst(ProtoContext* context, int count) const;
        const ProtoTupleImplementation* implRemoveLast(ProtoContext* context, int count) const;
        const ProtoTupleImplementation* implRemoveAt(ProtoContext* context, int index) const;
        const ProtoTupleImplementation* implRemoveSlice(ProtoContext* context, int from, int to) const;
        const ProtoObject* implAsObject(ProtoContext* context) const;
        const ProtoTuple* asProtoTuple(ProtoContext* context) const;
        // ...

        // ...
        unsigned long getHash(ProtoContext* context) const override;
        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;

        // ...
        unsigned long count;
        union
        {
            ProtoObject* data[TUPLE_SIZE];
            ProtoTupleImplementation* indirect[TUPLE_SIZE];
        } pointers;
    };

    class ProtoStringIteratorImplementation final : public Cell
    {
    public:
        ProtoStringIteratorImplementation(
            ProtoContext* context,
            ProtoString* base,
            int currentIndex = 0
        );

        ~ProtoStringIteratorImplementation() override;

        // ...
        int implHasNext(ProtoContext* context) const;
        const ProtoObject* implNext(ProtoContext* context) const;
        const ProtoStringIteratorImplementation* implAdvance(ProtoContext* context);
        const ProtoObject* implAsObject(ProtoContext* context) const;
        const ProtoStringIterator* asProtoStringIterator(ProtoContext* context) const;
        // ...

        // ...
        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;

        ProtoString* base;
        unsigned long currentIndex;
    };

    class ProtoStringImplementation final : public Cell
    {
    public:
        ProtoStringImplementation(
            ProtoContext* context,
            const ProtoTupleImplementation* base);
        ~ProtoStringImplementation() override;
        // ...
        int implCmpToString(ProtoContext* context, const ProtoString* otherString) const;
        const ProtoObject* implGetAt(ProtoContext* context, int index) const;
        unsigned long implGetSize(ProtoContext* context) const;
        const ProtoStringImplementation* implGetSlice(ProtoContext* context, int from, int to) const;
        const ProtoStringImplementation* implSetAt(ProtoContext* context, int index, const ProtoObject* value) const;
        const ProtoStringImplementation* implInsertAt(ProtoContext* context, int index, const ProtoObject* value) const;
        const ProtoStringImplementation* implAppendLast(ProtoContext* context, const ProtoString* otherString) const;
        const ProtoStringImplementation* implAppendFirst(ProtoContext* context, const ProtoString* otherString) const;
        const ProtoStringImplementation* implRemoveSlice(ProtoContext* context, int from, int to) const;
        const ProtoList* implAsList(ProtoContext* context) const;
        const ProtoStringIteratorImplementation* implGetIterator(ProtoContext* context) const;
        const ProtoStringImplementation* implSetAtString(ProtoContext* context, int index, const ProtoString* otherString) const;
        const ProtoStringImplementation* implInsertAtString(ProtoContext* context, int index, const ProtoString* otherString) const;
        const ProtoStringImplementation* implSplitFirst(ProtoContext* context, int count) const;
        const ProtoStringImplementation* implSplitLast(ProtoContext* context, int count) const;
        const ProtoStringImplementation* implRemoveFirst(ProtoContext* context, int count) const;
        const ProtoStringImplementation* implRemoveLast(ProtoContext* context, int count) const;
        const ProtoStringImplementation* implRemoveAt(ProtoContext* context, int index) const;
        const ProtoObject* implAsObject(ProtoContext* context) const;
        const ProtoString* asProtoString(ProtoContext* context) const;

        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;

        // ...
        // ...
        ProtoTupleImplementation* base;
    };

    class ProtoByteBufferImplementation final : public Cell
    {
    public:
        ProtoByteBufferImplementation(
            ProtoContext* context,
            char* buffer,
            unsigned long size,
            bool freeOnExit = false
        );
        ~ProtoByteBufferImplementation() override;
        // ...
        char implGetAt(ProtoContext* context, int index) const;
        void implSetAt(ProtoContext* context, int index, char value);
        unsigned long implGetSize(ProtoContext* context) const;
        char* implGetBuffer(ProtoContext* context) const;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        const ProtoByteBuffer* asByteBuffer(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const override;

        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;

        // ...
        // ...
        char* buffer;
        unsigned long size;
        bool freeOnExit;
    };

    class ProtoMethodCell : public Cell
    {
    public:
        ProtoMethodCell(
            ProtoContext* context,
            const ProtoObject* selfObject,
            ProtoMethod methodTarget
        );
        ~ProtoMethodCell() override;
        // ...
        const ProtoObject* implInvoke(ProtoContext* context, const ProtoList* args, const ProtoSparseList* kwargs) const;
        const ProtoObject* implAsObject(ProtoContext* context) const;
        const ProtoObject* implGetSelf(ProtoContext* context) const;
        ProtoMethod implGetMethod(ProtoContext* context) const;

        unsigned long getHash(ProtoContext* context) const override;
        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* target,
            void (*auxMethod)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;

        // ...
        const ProtoObject* self;
        ProtoMethod method;
    };

    class ProtoExternalPointerImplementation final : public Cell
    {
    public:
        ProtoExternalPointerImplementation(
            ProtoContext* context,
            void* pointer
        );
        ~ProtoExternalPointerImplementation() override;
        // ...
        void* implGetPointer(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const override;
        const ProtoObject* implAsObject(ProtoContext* context) const override;

        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;

        // ...
        void* pointer;
    };

    struct AttributeCacheEntry
    {
        const ProtoObject* object;
        const ProtoString* attributeName;
        const ProtoObject* value;
    };

    // The main thread class, now smaller
    class ProtoThreadImplementation final : public Cell
    {
    public:
        ProtoThreadImplementation(
            ProtoContext* context,
            const ProtoString* name,
            ProtoSpace* space,
            ProtoMethod method = nullptr,
            const ProtoList* unnamedArgList = nullptr,
            const ProtoSparseList* kwargs = nullptr
        );
        ~ProtoThreadImplementation() override;

        // Methods remain the same
        void implSetUnmanaged();
        void implSetManaged();
        void implDetach(ProtoContext* context) const;
        void implJoin(ProtoContext* context) const;
        void implSynchToGC();
        ProtoContext* implGetCurrentContext() const;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        void implSetCurrentContext(ProtoContext* context);
        const ProtoThread* asThread(ProtoContext* context) const;
        static ProtoThreadImplementation* implGetCurrentThread();
        Cell* implAllocCell();
        unsigned long getHash(ProtoContext* context) const override;
        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*callBackMethod)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;

        // --- Members kept in the main cell ---
        ProtoSpace* space;
        ProtoContext* currentContext;
        int state;
        std::atomic<int> unmanagedCount;
        const ProtoString* name;
        mutable ProtoThreadExtension* extension; // Pointer to the extension
    };

    // The new extension class to hold large members
    class ProtoThreadExtension final : public Cell
    {
    public:
        ProtoThreadExtension(ProtoContext* context);
        ~ProtoThreadExtension() override;

        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*callBackMethod)(ProtoContext* context, void* self, const Cell* cell)
        ) const override;

        // --- Large members moved here ---
        std::unique_ptr<std::thread> osThread;
        mutable AttributeCacheEntry* attributeCache;
        Cell* freeCells;
    };

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
            ProtoThreadExtension threadExtensionCell; // Add the new type
        };
    };

    static_assert(sizeof(BigCell) <= 64, "BigCell exceeds 64 bytes!!!!");
    static_assert(sizeof(ProtoObjectCell) <= 64, "ProtoObjectCell exceeds 64 bytes!");
    static_assert(sizeof(ProtoListIteratorImplementation) <= 64, "ProtoListIteratorImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoListImplementation) <= 64, "ProtoListImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoSparseListIteratorImplementation) <= 64, "ProtoSparseListIteratorImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoSparseListImplementation) <= 64, "ProtoSparseListImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoTupleIteratorImplementation) <= 64, "ProtoTupleIteratorImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoTupleImplementation) <= 64, "ProtoTupleImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoStringIteratorImplementation) <= 64, "ProtoStringIteratorImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoStringImplementation) <= 64, "ProtoStringImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoByteBufferImplementation) <= 64, "ProtoByteBufferImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoMethodCell) <= 64, "ProtoMethodCell exceeds 64 bytes!");
    static_assert(sizeof(ProtoExternalPointerImplementation) <= 64, "ProtoExternalPointerImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoThreadImplementation) <= 64, "ProtoThreadImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoThreadExtension) <= 64, "ProtoThreadExtension exceeds 64 bytes!");

} // namespace proto

#endif /* PROTO_INTERNAL_H */

