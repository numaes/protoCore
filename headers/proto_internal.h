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

//! @brief Defines the depth of the method cache for each thread. Must be a power of 2.
#define THREAD_CACHE_DEPTH 1024

namespace proto
{
    // Forward declarations for internal implementation classes
    class Cell;
    class BigCell;
    class ProtoObjectCellImplementation;
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
    class ProtoMethodCellImplementation;

    /**
     * @union ProtoObjectPointer
     * @brief A 64-bit union for implementing tagged pointers.
     *
     * This union allows a single 64-bit word to represent either a pointer to a
     * heap-allocated object (a `Cell`) or an immediate, embedded value (like a
     * small integer or a boolean). This technique, known as pointer tagging,
     * avoids memory allocation for simple, common types, significantly improving
     * performance and reducing memory pressure.
     *
     * The lowest 4 bits (`pointer_tag`) identify the type of data stored.
     */
    union ProtoObjectPointer
    {
        // Generic Pointers
        struct { ProtoObject* oid; } oid;
        void* voidPointer;

        // Pointers to Public API Types
        ProtoMethod method;
        ProtoObjectCell* objectCell;
        ProtoSparseList* sparseList;
        ProtoList* list;
        ProtoTuple* tuple;
        ProtoString* string;
        ProtoByteBuffer* byteBuffer;
        ProtoExternalPointer* externalPointer;
        ProtoThread* thread;
        ProtoListIterator* listIterator;
        ProtoTupleIterator* tupleIterator;
        ProtoStringIterator* stringIterator;
        ProtoSparseListIterator* sparseListIterator;

        // Pointers to Internal Implementation Types
        ProtoMethodCellImplementation* methodCellImplementation;
        ProtoObjectCellImplementation* objectCellImplementation;
        ProtoSparseListImplementation* sparseListImplementation;
        ProtoListImplementation* listImplementation;
        ProtoTupleImplementation* tupleImplementation;
        ProtoStringImplementation* stringImplementation;
        ProtoByteBufferImplementation* byteBufferImplementation;
        ProtoExternalPointerImplementation* externalPointerImplementation;
        ProtoThreadImplementation* threadImplementation;
        ProtoListIteratorImplementation* listIteratorImplementation;
        ProtoTupleIteratorImplementation* tupleIteratorImplementation;
        ProtoStringIteratorImplementation* stringIteratorImplementation;
        ProtoSparseListIteratorImplementation *sparseListIteratorImplementation;

        // Tagged Pointer Structures
        struct { unsigned long pointer_tag : 4; unsigned long embedded_type : 4; unsigned long value : 56; } op;
        struct { unsigned long pointer_tag : 4; unsigned long embedded_type : 4; long smallInteger : 56; } si;
        struct { unsigned long pointer_tag : 4; unsigned long embedded_type : 4; unsigned long floatValue : 32; } sd;
        struct { unsigned long pointer_tag : 4; unsigned long embedded_type : 4; unsigned long unicodeValue : 56; } unicodeChar;
        struct { unsigned long pointer_tag : 4; unsigned long embedded_type : 4; unsigned long booleanValue : 1; unsigned long padding : 55; } booleanValue;
        struct { unsigned long pointer_tag : 4; unsigned long embedded_type : 4; char byteData; } byteValue;
        struct { unsigned long pointer_tag : 4; unsigned long embedded_type : 4; unsigned long year : 16; unsigned long month : 8; unsigned long day : 8; unsigned long padding : 24; } date;
        struct { unsigned long pointer_tag : 4; unsigned long embedded_type : 4; unsigned long timestamp : 56; } timestampValue;
        struct { unsigned long pointer_tag : 4; unsigned long embedded_type : 4; long timedelta : 56; } timedeltaValue;

        // Utility Views
        struct { unsigned long pointer_tag : 4; unsigned long hash : 60; } asHash;
        struct { unsigned long pointer_tag : 4; Cell* cell; } cell;
    };

    //! @brief Represents a large, contiguous block of memory allocated from the OS.
    class AllocatedSegment
    {
    public:
        BigCell* memoryBlock;       //!< Pointer to the start of the memory block.
        int cellsCount;             //!< The number of `BigCell` units in this block.
        AllocatedSegment* nextBlock; //!< Pointer to the next segment in the chain.
    };

    //! @brief Tracks a chain of modified (`dirty`) cells that need to be scanned by the GC.
    class DirtySegment
    {
    public:
        BigCell* cellChain;         //!< A linked list of dirty cells.
        DirtySegment* nextSegment;  //!< Pointer to the next dirty segment.
    };

    // --- Pointer Tag Definitions ---
#define POINTER_TAG_OBJECT                  0  //!< A standard heap-allocated object.
#define POINTER_TAG_EMBEDDED_VALUE          1  //!< An immediate value (e.g., integer, bool).
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

    // --- Embedded Type Definitions (for POINTER_TAG_EMBEDDED_VALUE) ---
#define EMBEDDED_TYPE_SMALLINT               0  //!< A 56-bit signed integer.
#define EMBEDDED_TYPE_FLOAT                  1  //!< A 32-bit float.
#define EMBEDDED_TYPE_UNICODECHAR            2  //!< A Unicode character.
#define EMBEDDED_TYPE_BOOLEAN                3  //!< A boolean value.
#define EMBEDDED_TYPE_BYTE                   4  //!< An 8-bit byte.
#define EMBEDDED_TYPE_DATE                   5  //!< A date (year, month, day).
#define EMBEDDED_TYPE_TIMESTAMP              6  //!< A timestamp.
#define EMBEDDED_TYPE_TIMEDELTA              7  //!< A time delta.

    // --- Iterator States ---
#define ITERATOR_NEXT_PREVIOUS              0 //!< Iterator is positioned before the current element.
#define ITERATOR_NEXT_THIS                  1 //!< Iterator is positioned at the current element.
#define ITERATOR_NEXT_NEXT                  2 //!< Iterator is positioned after the current element.

    // --- ProtoSpace States (for GC coordination) ---
#define SPACE_STATE_RUNNING                 0 //!< Normal operation.
#define SPACE_STATE_STOPPING_WORLD          1 //!< GC is requesting all threads to pause.
#define SPACE_STATE_WORLD_TO_STOP           2 //!< All threads have been signaled to stop.
#define SPACE_STATE_WORLD_STOPPED           3 //!< All threads are paused; GC can run.
#define SPACE_STATE_ENDING                  4 //!< The space is shutting down.

    // --- ProtoThread States (for GC coordination) ---
#define THREAD_STATE_UNMANAGED              0 //!< Thread is running native code and will not be paused by GC.
#define THREAD_STATE_MANAGED                1 //!< Thread is running managed code and can be paused by GC.
#define THREAD_STATE_STOPPING               2 //!< Thread has been requested to pause.
#define THREAD_STATE_STOPPED                3 //!< Thread is paused.
#define THREAD_STATE_ENDED                  4 //!< Thread has finished execution.

#define TYPE_SHIFT                          4

    //! @brief Utility to cast a public API pointer to its internal implementation pointer.
    template <typename Impl, typename Api>
    inline Impl* toImpl(Api* ptr)
    {
        ProtoObjectPointer p{};
        p.oid.oid = reinterpret_cast<ProtoObject *>(ptr);
        p.op.pointer_tag = 0;
        return static_cast<Impl*>(p.voidPointer);
    }

    //! @brief Const-correct utility to cast a public API pointer to its internal implementation pointer.
    template <typename Impl, typename Api>
    inline const Impl* toImpl(const Api* ptr)
    {
        return reinterpret_cast<const Impl*>(ptr);
    }

    //! @brief Generates a unique reference ID for mutable objects.
    unsigned long generate_mutable_ref();

    /**
     * @class Cell
     * @brief The fundamental unit of memory allocation for all heap-based objects.
     *
     * Every object that is not an immediate value (tagged pointer) is allocated
     * as a `Cell` or a class derived from it. This provides a common interface
     * for the garbage collector.
     */
    class Cell
    {
    public:
        static void* operator new(unsigned long size, ProtoContext* context);

        explicit Cell(ProtoContext* context);
        virtual ~Cell();

        //! @brief Called by the GC before an object is destroyed. Used for cleanup.
        virtual void finalize(ProtoContext* context);

        //! @brief Traverses all references within this cell, applying the given visitor function.
        virtual void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, Cell*));

        virtual unsigned long getHash(ProtoContext* context);
        virtual ProtoObject* asObject(ProtoContext* context);

        Cell* nextCell{}; //!< Links cells together in memory management lists.
    };

    /**
     * @class BigCell
     * @brief A specialized cell fixed at 64 bytes.
     *
     * `BigCell` is the standard allocation size, ensuring that all cells can be
     * managed uniformly in page-sized memory blocks. This alignment is beneficial
     * for performance and simplifies the memory manager.
     */
    class BigCell final : public Cell
    {
    public:
        explicit BigCell(ProtoContext* context);
        ~BigCell() override;

        void finalize(ProtoContext* context) override { /* No-op */ };
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, Cell*)) override { /* No-op */ };
        unsigned long getHash(ProtoContext* context) override { return Cell::getHash(context); };
        ProtoObject* asObject(ProtoContext* context) override { return Cell::asObject(context); };

        void* undetermined[6]{}; //!< Padding to ensure the 64-byte size.
    };

    static_assert(sizeof(BigCell) == 64, "The size of the BigCell class must be exactly 64 bytes.");

    /**
     * @class ParentLinkImplementation
     * @brief Internal implementation of a link in an object's prototype chain.
     */
    class ParentLinkImplementation final : public Cell, public ParentLink
    {
    public:
        explicit ParentLinkImplementation(ProtoContext* context, ParentLinkImplementation* parent, ProtoObjectCellImplementation* object);
        ~ParentLinkImplementation() override;

        ProtoObject* asObject(ProtoContext* context) override;
        unsigned long getHash(ProtoContext* context) override;
        void finalize(ProtoContext* context) override;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, Cell*)) override;

        ParentLinkImplementation* parent; //!< The next link in the prototype chain.
        ProtoObjectCellImplementation* object; //!< The object this link points to.
    };

    /**
     * @class ProtoObjectCellImplementation
     * @brief Internal implementation of a standard Proto object.
     */
    class ProtoObjectCellImplementation final : public Cell, public ProtoObject
    {
    public:
        ProtoObjectCellImplementation(ProtoContext* context, ParentLinkImplementation* parent, unsigned long mutable_ref, ProtoSparseListImplementation* attributes);
        ~ProtoObjectCellImplementation() override;

        ProtoObjectCellImplementation* implAddParent(ProtoContext* context, ProtoObjectCell* newParentToAdd) const;
        ProtoObject* implAsObject(ProtoContext* context);

        void finalize(ProtoContext* context) override;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, Cell*)) override;
        long unsigned getHash(ProtoContext* context) override;

        ParentLinkImplementation* parent;       //!< The head of the prototype chain.
        unsigned long mutable_ref;              //!< Non-zero if the object is mutable.
        ProtoSparseListImplementation* attributes; //!< The list of the object's own attributes.
    };

    // --- ProtoList & ProtoListIterator Implementations ---
    class ProtoListIteratorImplementation final : public Cell, public ProtoListIterator { /* Omitted for brevity */ };
    class ProtoListImplementation final : public Cell, public ProtoList { /* Omitted for brevity */ };

    // --- ProtoSparseList & ProtoSparseListIterator Implementations ---
    class ProtoSparseListIteratorImplementation final : public Cell, public ProtoSparseListIterator { /* Omitted for brevity */ };
    class ProtoSparseListImplementation final : public Cell, public ProtoSparseList { /* Omitted for brevity */ };

    /**
     * @class TupleDictionary
     * @brief A balanced binary tree used for interning immutable tuples.
     *
     * To save memory, all identical immutable tuples point to the same underlying
     * `ProtoTupleImplementation`. This class implements a dictionary (via a
     * rebalancing tree) to find and reuse existing tuples efficiently.
     */
    class TupleDictionary final : public Cell { /* Omitted for brevity */ };

    // --- ProtoTuple & ProtoTupleIterator Implementations ---
    class ProtoTupleIteratorImplementation final : public Cell, public ProtoTupleIterator { /* Omitted for brevity */ };

    /**
     * @class ProtoTupleImplementation
     * @brief Internal implementation of an immutable tuple, often using a rope-like structure.
     *
     * For efficiency with concatenation and slicing, large tuples are represented
     * as a tree (a rope), where leaf nodes contain arrays of `ProtoObject*` and
     * internal nodes point to other tuple segments.
     */
    class ProtoTupleImplementation final : public Cell, public ProtoTuple
    {
    public:
        /* Omitted for brevity */
    private:
        unsigned long elementCount : 56;
        unsigned long height : 8; //!< Height of this node in the rope tree.
        union
        {
            ProtoObject* data[TUPLE_SIZE];              //!< Direct data for leaf nodes.
            ProtoTupleImplementation* indirect[TUPLE_SIZE]; //!< Pointers to sub-tuples for internal nodes.
        } pointers;
    };

    // --- ProtoString & ProtoStringIterator Implementations ---
    class ProtoStringIteratorImplementation final : public Cell, public ProtoStringIterator { /* Omitted for brevity */ };

    /**
     * @class ProtoStringImplementation
     * @brief Internal implementation of an immutable string.
     *
     * A `ProtoString` is implemented as a wrapper around a `ProtoTuple` that is
     * guaranteed to contain only character objects. This reuses the efficient
     * rope implementation of tuples for string operations.
     */
    class ProtoStringImplementation final : public Cell, public ProtoString
    {
        /* Omitted for brevity */
    private:
        ProtoTupleImplementation* baseTuple; //!< The underlying tuple of characters.
    };

    /**
     * @class ProtoByteBufferImplementation
     * @brief Internal implementation of a mutable byte buffer.
     */
    class ProtoByteBufferImplementation final : public Cell, public ProtoByteBuffer
    {
        /* Omitted for brevity */
    private:
        unsigned long size; //!< The size of the buffer in bytes.
        char* buffer;       //!< Pointer to the buffer's memory.
        bool freeOnExit;    //!< True if this object owns the buffer and must free it.
    };

    /**
     * @class ProtoMethodCellImplementation
     * @brief Internal implementation of a cell holding a native C++ function pointer.
     */
    class ProtoMethodCellImplementation final : public Cell, public ProtoMethodCell { /* Omitted for brevity */ };

    //! @brief Entry for the per-thread attribute access cache.
    struct AttributeCacheEntry
    {
        ProtoObject* object;         //!< The object the attribute was accessed on.
        ProtoString* attribute_name; //!< The name of the attribute.
        ProtoObject* value;          //!< The resulting value of the lookup.
    };

    /**
     * @class ProtoExternalPointerImplementation
     * @brief Internal implementation of a cell containing an opaque `void*`.
     *
     * This allows native C/C++ pointers to be handled by the Proto object model
     * without being tracked by the garbage collector.
     */
    class ProtoExternalPointerImplementation final : public Cell, public ProtoExternalPointer
    {
        /* Omitted for brevity */
    private:
        void* pointer; //!< The opaque, unmanaged pointer to external data.
    };

    /**
     * @class ProtoThreadImplementation
     * @brief Internal implementation of a Proto thread of execution.
     */
    class ProtoThreadImplementation final : public Cell, public ProtoThread
    {
    public:
        /* Omitted for brevity */

        // --- Member Data ---
        int state;                      //!< Current state of the thread (Managed, Unmanaged, Stopped, etc.).
        ProtoString* name;              //!< Name of the thread, for debugging.
        ProtoSpace* space;              //!< The ProtoSpace this thread belongs to.
        std::thread* osThread;          //!< The underlying OS thread handle.
        BigCell* freeCells;             //!< A list of free memory cells local to this thread.
        ProtoContext* currentContext;   //!< The current execution context (call stack).
        unsigned int unmanagedCount;    //!< Nesting count for unmanaged sections.
        AttributeCacheEntry* attribute_cache; //!< Per-thread cache for attribute lookups.
    };

} // namespace proto

#endif /* PROTO_INTERNAL_H */
