/* 
 * proto_internal.h
 *
 *  Created on: November, 2017 - Redesign January, 2024
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 */


#ifndef PROTO_INTERNAL_H
#define PROTO_INTERNAL_H

#include "../headers/proto.h"
#include <thread>

// Method cache per thread. It should be a power of 2
#define THREAD_CACHE_DEPTH 1024

namespace proto
{
    // Forward declarations for implementation classes
    class Cell;
    class BigCell;
    class ProtoObjectCellImplementation;
    class ParentLinkImplementation;
    class ProtoListImplementation;
    class ProtoSparseListImplementation;
    class ProtoTupleImplementation;
    class ProtoStringImplementation;
    class ProtoMethodCellImplementation;
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

        struct
        {
            ProtoObjectCellImplementation* objectCell;
        } oc;

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
            unsigned char byteData;
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
#define POINTER_TAG_EMBEDEDVALUE            1
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
#define EMBEDED_TYPE_SMALLINT               0
#define EMBEDED_TYPE_FLOAT                  1
#define EMBEDED_TYPE_UNICODECHAR            2
#define EMBEDED_TYPE_BOOLEAN                3
#define EMBEDED_TYPE_BYTE                   4
#define EMBEDED_TYPE_DATE                   5
#define EMBEDED_TYPE_TIMESTAMP              6
#define EMBEDED_TYPE_TIMEDELTA              7

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
        return reinterpret_cast<Impl*>(ptr);
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
        static void* operator new(unsigned long size, ProtoContext* context);

        explicit Cell(ProtoContext* context);
        virtual ~Cell();

        virtual void finalize(ProtoContext* context);
        virtual void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

        virtual unsigned long getHash(ProtoContext* context);
        virtual ProtoObject* asObject(ProtoContext* context);

        Cell* nextCell;
    };

    class BigCell : public Cell
    {
    public:
        BigCell(ProtoContext* context);
        ~BigCell();

        void finalize(ProtoContext* context)
        {
            /* No special finalization needed for BigCell */
        };

        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        )
        {
            /* BigCell does not hold references to other Cells */
        };
        unsigned long getHash(ProtoContext* context) { return Cell::getHash(context); };
        ProtoObject* asObject(ProtoContext* context) { return Cell::asObject(context); };

        void * undetermined[6];
    };

    static_assert(sizeof(BigCell) == 64, "The size of the BigCell class must be exactly 64 bytes.");


    class ParentLinkImplementation : public Cell, public ParentLink
    {
    public:
        explicit ParentLinkImplementation(
            ProtoContext* context,
            ParentLinkImplementation* parent,
            ProtoObjectCellImplementation* object
        );
        ~ParentLinkImplementation();

        ProtoObject* asObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);

        void finalize(ProtoContext* context);

        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

        ParentLinkImplementation* parent;
        ProtoObjectCellImplementation* object;
    };

    /**
     * @class ProtoObjectCellImplementation
     * @brief Implementation of a cell representing a Proto object.
     *
     * Inherits from 'Cell' for memory management and from 'ProtoObjectCell'
     * for the public object interface. It contains a reference to its
     * inheritance chain (parents) and a list of attributes.
     */
    class ProtoObjectCellImplementation : public Cell, public ProtoObject
    {
    public:
        /**
         * @brief Constructor.
         * @param context The current execution context.
         * @param parent Pointer to the first link in the inheritance chain.
         * @param mutable_ref A flag indicating if the object is mutable.
         * @param attributes The sparse list of object attributes.
         */
        ProtoObjectCellImplementation(
            ProtoContext* context,
            ParentLinkImplementation* parent,
            unsigned long mutable_ref,
            ProtoSparseListImplementation* attributes
        );

        /**
         * @brief Virtual destructor.
         */
        ~ProtoObjectCellImplementation();

        /**
         * @brief Creates a new object cell with an additional parent in its inheritance chain.
         * @param context The current execution context.
         * @param newParentToAdd The parent object to be added.
         * @return A *new* ProtoObjectCellImplementation with the updated inheritance chain.
         */
        ProtoObjectCell* implAddParent(
            ProtoContext* context,
            ProtoObjectCell* newParentToAdd
        );

        /**
         * @brief Returns the representation of this cell as a ProtoObject.
         * @param context The current execution context.
         * @return A ProtoObject representing this object.
         */
        ProtoObject* implAsObject(ProtoContext* context);

        /**
         * @brief Finalizer for the garbage collector.
         *
         * This method is called by the GC before freeing the cell's memory.
         * @param context The current execution context.
         */
        void finalize(ProtoContext* context);

        /**
         * @brief Processes internal references for the garbage collector.
         *
         * Traverses references to the parent and attributes so that the GC
         * can mark reachable objects.
         */
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, Cell* cell)
        );
        long unsigned getHash(ProtoContext* context);

        ParentLinkImplementation* parent;
        unsigned long mutable_ref;
        ProtoSparseListImplementation* attributes;
    };

    // --- ProtoListIterator ---
    // Concrete implementation for ProtoObject*
    class ProtoListIteratorImplementation : public Cell, public ProtoListIterator
    {
    public:
        ProtoListIteratorImplementation(
            ProtoContext* context,
            ProtoListImplementation* base,
            unsigned long currentIndex
        );
        ~ProtoListIteratorImplementation();

        int implHasNext(ProtoContext* context);
        ProtoObject* implNext(ProtoContext* context);
        ProtoListIteratorImplementation* implAdvance(ProtoContext* context);

        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);

        void finalize(ProtoContext* context);

        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

    private:
        ProtoListImplementation* base;
        unsigned long currentIndex;
    };

    // --- ProtoList ---
    // Concrete implementation for ProtoObject*
    class ProtoListImplementation : public Cell, public ProtoList
    {
    public:
        explicit ProtoListImplementation(
            ProtoContext* context,
            ProtoObject* value = PROTO_NONE,
            ProtoListImplementation* previous = nullptr,
            ProtoListImplementation* next = nullptr
        );
        ~ProtoListImplementation();

        ProtoObject* implGetAt(ProtoContext* context, int index);
        ProtoObject* implGetFirst(ProtoContext* context);
        ProtoObject* implGetLast(ProtoContext* context);
        ProtoListImplementation* implGetSlice(ProtoContext* context, int from, int to);
        unsigned long implGetSize(ProtoContext* context);

        bool implHas(ProtoContext* context, ProtoObject* value);
        ProtoListImplementation* implSetAt(ProtoContext* context, int index, ProtoObject* value = PROTO_NONE);
        ProtoListImplementation* implInsertAt(ProtoContext* context, int index, ProtoObject* value);

        ProtoListImplementation* implAppendFirst(ProtoContext* context, ProtoObject* value);
        ProtoListImplementation* implAppendLast(ProtoContext* context, ProtoObject* value);

        ProtoListImplementation* implExtend(ProtoContext* context, ProtoList* other);

        ProtoListImplementation* implSplitFirst(ProtoContext* context, int index);
        ProtoListImplementation* implSplitLast(ProtoContext* context, int index);

        ProtoListImplementation* implRemoveFirst(ProtoContext* context);
        ProtoListImplementation* implRemoveLast(ProtoContext* context);
        ProtoListImplementation* implRemoveAt(ProtoContext* context, int index);
        ProtoListImplementation* implRemoveSlice(ProtoContext* context, int from, int to);

        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);
        ProtoListIteratorImplementation* implGetIterator(ProtoContext* context);

        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

        ProtoListImplementation* previous;
        ProtoListImplementation* next;

        ProtoObject* value;
        unsigned long hash{};

        unsigned long count : 52{};
        unsigned long height : 8;
        unsigned long type : 4{};
    };

    // --- ProtoSparseList ---
    // Concrete implementation for ProtoObject*
    class ProtoSparseListIteratorImplementation : public Cell, public ProtoSparseListIterator
    {
    public:
        ProtoSparseListIteratorImplementation(
            ProtoContext* context,
            int state,
            ProtoSparseListImplementation* current,
            ProtoSparseListIteratorImplementation* queue = NULL
        );
        ~ProtoSparseListIteratorImplementation();

        int implHasNext(ProtoContext* context);
        unsigned long implNextKey(ProtoContext* context);
        ProtoObject* implNextValue(ProtoContext* context);

        ProtoSparseListIteratorImplementation* implAdvance(ProtoContext* context);
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);

        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

    private:
        int state;
        ProtoSparseListImplementation* current;
        ProtoSparseListIteratorImplementation* queue;
    };

    class ProtoSparseListImplementation : public Cell, public ProtoSparseList
    {
    public:
        explicit ProtoSparseListImplementation(
            ProtoContext* context,
            unsigned long index = 0,
            ProtoObject* value = PROTO_NONE,
            ProtoSparseListImplementation* previous = NULL,
            ProtoSparseListImplementation* next = NULL
        );
        ~ProtoSparseListImplementation();

        bool implHas(ProtoContext* context, unsigned long index);
        ProtoObject* implGetAt(ProtoContext* context, unsigned long index);
        ProtoSparseListImplementation* implSetAt(ProtoContext* context, unsigned long index, ProtoObject* value);
        ProtoSparseListImplementation* implRemoveAt(ProtoContext* context, unsigned long index);
        int implIsEqual(ProtoContext* context, ProtoSparseList* otherDict);
        ProtoObject* implGetAtOffset(ProtoContext* context, int offset);

        unsigned long implGetSize(ProtoContext* context);
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);
        virtual ProtoSparseListIteratorImplementation* implGetIterator(ProtoContext* context);

        void implProcessElements(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                unsigned long index,
                ProtoObject* value
            )
        );

        void implProcessValues(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                ProtoObject* value
            )
        );

        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

        ProtoSparseListImplementation* previous;
        ProtoSparseListImplementation* next;

        unsigned long index;
        ProtoObject* value;
        unsigned long hash;

        unsigned long count : 52;
        unsigned long height : 8;
        unsigned long type : 4{};
    };

    // --- Tuple Interning Dictionary ---
#define TUPLE_SIZE 5

    class TupleDictionary : public Cell
    {
    public:
        TupleDictionary* next;
        TupleDictionary* previous;
        ProtoTupleImplementation* key;
        int count;
        int height;

        int compareTuple(ProtoContext* context, ProtoTuple* tuple);
        TupleDictionary* rightRotate(ProtoContext* context);
        TupleDictionary* leftRotate(ProtoContext* context);
        TupleDictionary* rebalance(ProtoContext* context);

        TupleDictionary(
            ProtoContext* context,
            ProtoTupleImplementation* key = nullptr,
            TupleDictionary* next = nullptr,
            TupleDictionary* previous = nullptr
        );

        long unsigned int getHash(proto::ProtoContext*);
        ProtoObject* asObject(proto::ProtoContext*);
        void finalize(ProtoContext* context);

        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

        int compareList(ProtoContext* context, ProtoList* list);
        bool hasList(ProtoContext* context, ProtoList* list);
        bool has(ProtoContext* context, ProtoTuple* tuple);
        ProtoTupleImplementation* getAt(ProtoContext* context, ProtoTupleImplementation* tuple);
        TupleDictionary* set(ProtoContext* context, ProtoTupleImplementation* tuple);
    };

    // Concrete implementation for ProtoTupleIterator
    class ProtoTupleIteratorImplementation : public Cell, public ProtoTupleIterator
    {
    public:
        // Constructor
        ProtoTupleIteratorImplementation(
            ProtoContext* context,
            ProtoTupleImplementation* base,
            unsigned long currentIndex
        );

        // Destructor
        ~ProtoTupleIteratorImplementation();

        // --- ProtoTupleIterator interface methods ---
        int implHasNext(ProtoContext* context);
        ProtoObject* implNext(ProtoContext* context);
        ProtoTupleIteratorImplementation* implAdvance(ProtoContext* context);

        // --- Cell interface methods ---
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);
        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, Cell*)
        );

    private:
        ProtoTupleImplementation* base; // The tuple being iterated over
        unsigned long currentIndex; // The current position in the tuple
    };

    // --- ProtoTuple ---
    // Implementation of tuples, potentially using a "rope" structure for efficiency.
    class ProtoTupleImplementation : public Cell, public ProtoTuple
    {
    public:
        // Constructor
        ProtoTupleImplementation(
            ProtoContext* context,
            unsigned long elementCount,
            unsigned long heigh,
            ProtoObject** data
        );

        ProtoTupleImplementation(
            ProtoContext* context,
            unsigned long elementCount,
            unsigned long height,
            ProtoTupleImplementation** indirect
        );

        // Destructor
        ~ProtoTupleImplementation();

        // --- ProtoTuple interface methods ---
        ProtoObject* implGetAt(ProtoContext* context, int index);
        ProtoObject* implGetFirst(ProtoContext* context);
        ProtoObject* implGetLast(ProtoContext* context);
        ProtoTupleImplementation* implGetSlice(ProtoContext* context, int from, int to);
        unsigned long implGetSize(ProtoContext* context);
        ProtoList* implAsList(ProtoContext* context);
        static ProtoTupleImplementation* tupleFromList(ProtoContext* context, ProtoList* list);
        ProtoTupleIteratorImplementation* implGetIterator(ProtoContext* context);
        ProtoTupleImplementation* implSetAt(ProtoContext* context, int index, ProtoObject* value);
        bool implHas(ProtoContext* context, ProtoObject* value);
        ProtoTupleImplementation* implInsertAt(ProtoContext* context, int index, ProtoObject* value);
        ProtoTupleImplementation* implAppendFirst(ProtoContext* context, ProtoTuple* otherTuple);
        ProtoTupleImplementation* implAppendLast(ProtoContext* context, ProtoTuple* otherTuple);
        ProtoTupleImplementation* implSplitFirst(ProtoContext* context, int count);
        ProtoTupleImplementation* implSplitLast(ProtoContext* context, int count);
        ProtoTupleImplementation* implRemoveFirst(ProtoContext* context, int count);
        ProtoTupleImplementation* implRemoveLast(ProtoContext* context, int count);
        ProtoTupleImplementation* implRemoveAt(ProtoContext* context, int index);
        ProtoTupleImplementation* implRemoveSlice(ProtoContext* context, int from, int to);

        // --- Cell interface methods ---
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);
        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, Cell*)
        );

    private:
        unsigned long elementCount:56;
        unsigned long height:8;
        union {
            ProtoObject   *data[TUPLE_SIZE];
            ProtoTupleImplementation    *indirect[TUPLE_SIZE];
        } pointers;
    };

    // --- ProtoStringIterator ---
    // Concrete implementation for the ProtoString iterator.
    class ProtoStringIteratorImplementation : public Cell, public ProtoStringIterator
    {
    public:
        // Constructor
        ProtoStringIteratorImplementation(
            ProtoContext* context,
            ProtoStringImplementation* base,
            unsigned long currentIndex
        );

        // Destructor
        ~ProtoStringIteratorImplementation();

        // --- ProtoStringIterator interface methods ---
        int implHasNext(ProtoContext* context);
        ProtoObject* implNext(ProtoContext* context);
        ProtoStringIteratorImplementation* implAdvance(ProtoContext* context);

        // --- Cell interface methods ---
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context); // Inherited from Cell, important for consistency.
        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, Cell*)
        );

    private:
        ProtoStringImplementation* base; // The string being iterated over.
        unsigned long currentIndex; // The current position in the string.
    };

    // --- ProtoString ---
    // Implementation of an immutable string, based on a tuple of characters.
    class ProtoStringImplementation : public Cell, public ProtoString
    {
    public:
        // Constructor
        ProtoStringImplementation(
            ProtoContext* context,
            ProtoTupleImplementation* baseTuple
        );

        // Destructor
        ~ProtoStringImplementation();

        // --- ProtoString interface methods ---
        int implCmpToString(ProtoContext* context, ProtoString* otherString);
        ProtoObject* implGetAt(ProtoContext* context, int index);
        unsigned long implGetSize(ProtoContext* context);
        ProtoStringImplementation* implGetSlice(ProtoContext* context, int from, int to);
        ProtoStringImplementation* implSetAt(ProtoContext* context, int index, ProtoObject* value);
        ProtoStringImplementation* implInsertAt(ProtoContext* context, int index, ProtoObject* value);
        ProtoStringImplementation* implAppendLast(ProtoContext* context, ProtoString* otherString);
        ProtoStringImplementation* implAppendFirst(ProtoContext* context, ProtoString* otherString);
        ProtoStringImplementation* implRemoveSlice(ProtoContext* context, int from, int to);
        ProtoListImplementation* implAsList(ProtoContext* context);
        ProtoStringIteratorImplementation* implGetIterator(ProtoContext* context);
        ProtoStringImplementation* implSetAtString(ProtoContext* context, int index, ProtoString* otherString);
        ProtoStringImplementation* implInsertAtString(ProtoContext* context, int index, ProtoString* otherString);
        ProtoStringImplementation* implSplitFirst(ProtoContext* context, int count);
        ProtoStringImplementation* implSplitLast(ProtoContext* context, int count);
        ProtoStringImplementation* implRemoveFirst(ProtoContext* context, int count);
        ProtoStringImplementation* implRemoveLast(ProtoContext* context, int count);
        ProtoStringImplementation* implRemoveAt(ProtoContext* context, int index);

        // --- Cell interface methods ---
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);
        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, Cell*)
        );

    private:
        ProtoTupleImplementation* baseTuple; // The underlying tuple that stores the characters.
    };


    // --- ProtoByteBufferImplementation ---
    // Implementation of a byte buffer that can manage its own memory
    // or wrap an existing buffer.
    class ProtoByteBufferImplementation : public Cell, public ProtoByteBuffer
    {
    public:
        // Constructor: creates or wraps a memory buffer.
        // If 'buffer' is null, new memory will be allocated.
        ProtoByteBufferImplementation(
            ProtoContext* context,
            unsigned long size,
            char* buffer = nullptr
        );

        // Destructor: frees the memory if the class owns it.
        ~ProtoByteBufferImplementation();

        // --- ProtoByteBuffer interface methods ---
        char implGetAt(ProtoContext* context, int index);
        void implSetAt(ProtoContext* context, int index, char value);
        unsigned long implGetSize(ProtoContext* context);
        char* implGetBuffer(ProtoContext* context);

        // --- Cell interface methods ---
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, Cell*)
        );
        void finalize(ProtoContext* context);
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);

    private:
        // Helper function to validate and normalize indices.
        bool normalizeIndex(int& index);

        unsigned long size; // The size of the buffer in bytes.
        char* buffer; // Pointer to the buffer's memory.
        bool freeOnExit; // Flag indicating if the destructor should free `buffer`.
    };

    // --- ProtoMethodCellImplementation ---
    // Implementation of a pointer to a C method
    class ProtoMethodCellImplementation : public Cell, public ProtoMethodCell
    {
    public:
        ProtoMethodCellImplementation(ProtoContext* context, ProtoMethod method);

        ProtoObject* implInvoke(ProtoContext* context, ProtoList* args, ProtoSparseList* kwargs);
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);
        void finalize(ProtoContext* context);
        void processReferences(ProtoContext* context, void* self,
                               void (*method)(ProtoContext* context, void* self, Cell* cell));
        ProtoObject* implGetSelf(ProtoContext* context);
        ProtoMethod implGetMethod(ProtoContext* context);

    private:
        ProtoMethod method{};
    };

    struct MethodCacheEntry {
        ProtoObject* object;
        ProtoObject* method_name;
        ProtoMethod* method;
    };

    /**
 * @class ProtoExternalPointerImplementation
 * @brief Implementation of a cell containing an opaque pointer to external data.
 *
 * This class encapsulates a `void*` pointer that is not managed by the Proto
 * garbage collector. It is useful for integrating Proto with external C/C++
 * libraries or data, allowing these pointers to be passed as first-class objects.
 */
    class ProtoExternalPointerImplementation : public Cell, public ProtoExternalPointer
    {
    public:
        /**
         * @brief Constructor.
         * @param context The current execution context.
         * @param pointer The external pointer (void*) to be encapsulated.
         */
        ProtoExternalPointerImplementation(ProtoContext* context, void* pointer);

        /**
         * @brief Destructor.
         */
        ~ProtoExternalPointerImplementation();

        /**
         * @brief Gets the encapsulated external pointer.
         * @param context The current execution context.
         * @return The stored (void*) pointer.
         */
        void* implGetPointer(ProtoContext* context);

        /**
         * @brief Returns the representation of this cell as a ProtoObject.
         * @param context The current execution context.
         * @return A ProtoObject representing this external pointer.
         */
        ProtoObject* implAsObject(ProtoContext* context);

        /**
         * @brief Finalizer for the garbage collector.
         *
         * Performs no action, as the external pointer is not managed by the GC.
         * @param context The current execution context.
         */
        void finalize(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);

        /**
         * @brief Processes references for the garbage collector.
         *
         * The body is empty because the external pointer is not a reference
         * that the garbage collector should follow.
         */
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, Cell* cell)
        );

    private:
        void* pointer; // The opaque pointer to external data.
    };

    // --- ProtoThreadImplementation ---
    // The internal implementation of a thread managed by the Proto runtime.
    // Inherits from 'Cell' to be managed by the garbage collector.
    class ProtoThreadImplementation : public Cell, public ProtoThread
    {
    public:
        // --- Constructor and Destructor ---

        // Creates a new thread instance.
        ProtoThreadImplementation(
            ProtoContext* context,
            ProtoString* name,
            ProtoSpace* space,
            ProtoMethod targetCode,
            ProtoList* args,
            ProtoSparseList* kwargs);

        // Virtual destructor to ensure proper cleanup.
        virtual ~ProtoThreadImplementation();

        unsigned long getHash(ProtoContext* context);

        // --- GC Management Control ---

        // Marks the thread as "unmanaged" so the GC does not stop it.
        void implSetUnmanaged();

        // Returns the thread to the "managed" state by the GC.
        void implSetManaged();

        // --- Thread Lifecycle Control ---

        // Detaches the thread from the object, allowing it to run independently.
        void implDetach(ProtoContext* context);

        // Blocks the current thread until this thread finishes its execution.
        void implJoin(ProtoContext* context);

        // Requests the termination of the thread.
        void implExit(ProtoContext* context);

        // --- Memory Allocation and Synchronization ---

        // Allocates a new memory cell for the thread.
        Cell* implAllocCell();

        // Synchronizes the thread with the garbage collector.
        void implSynchToGC();

        // --- Type System Interface ---

        // Sets the current execution context for the thread.
        void implSetCurrentContext(ProtoContext* context);
        ProtoContext* implGetCurrentContext();

        // Converts the implementation to a generic ProtoObject*.
        ProtoObject* implAsObject(ProtoContext* context);

        // --- Garbage Collector Methods (Inherited from Cell) ---

        // Finalizer called by the GC before freeing the memory.
        void finalize(ProtoContext* context);

        // Processes references to other cells for the GC sweep.
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, Cell* cell));

        static ProtoThread* implGetCurrentThread(ProtoContext* context);

        // --- Member Data ---
        int state; // Current state of the thread with respect to the GC.
        ProtoString* name; // Name of the thread (for debugging).
        ProtoSpace* space; // The memory space to which the thread belongs.
        std::thread* osThread; // The actual operating system thread.
        BigCell* freeCells; // List of free memory cells local to the thread.
        ProtoContext* currentContext; // Current call stack of the thread.
        unsigned int unmanagedCount; // Counter for nested calls to setUnmanaged/setManaged.
        struct MethodCacheEntry *method_cache;
    };


} // namespace proto

#endif /* PROTO_INTERNAL_H */