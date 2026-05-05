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
#include <unordered_set>
#include <memory>
#include <string>
#include <algorithm>
#include <climits>
#include <iostream> // For std::cerr and std::abort
#include <vector>
#include <functional>

#ifdef PROTO_GC_LOCK_TRACE
#include <chrono>
#define GC_LOCK_TRACE(msg) do { \
    auto t = std::chrono::steady_clock::now(); \
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count(); \
    std::cerr << "[gc-lock] " << us << " " << std::this_thread::get_id() << " " << (msg) << "\n"; \
} while(0)
#else
#define GC_LOCK_TRACE(msg) do {} while(0)
#endif

#define THREAD_CACHE_DEPTH 1024
#define MUTABLE_VALUE_CACHE_DEPTH 1024
#define TUPLE_SIZE 4

#define SPACE_STATE_RUNNING 0
#define SPACE_STATE_ENDING 1


namespace proto {
    // Forward Declarations
    class Cell;
    class BigCell;
    class DirtySegment;
    class ProtoContext;
    class ProtoObjectCell;
    class ParentLinkImplementation;
    class ProtoListImplementation;
    class ProtoListSmallImplementation;
    class ProtoListIteratorImplementation;
    class ProtoSparseListImplementation;
    class ProtoSparseListIteratorImplementation;
    class ProtoSetImplementation;
    class ProtoSetIteratorImplementation;
    class ProtoMultisetImplementation;
    class ProtoMultisetIteratorImplementation;
    class ProtoTupleImplementation;
    class ProtoStringImplementation;
    class StringLeafNode;
    class StringInternalNode;
    class ProtoRangeIteratorImplementation;
    class ProtoByteBufferImplementation;
    class ProtoExternalPointerImplementation;
    class ProtoExternalBufferImplementation;
    class ProtoMethodCell;
    class ProtoThreadImplementation;
    class ProtoThreadExtension;
    class LargeIntegerImplementation;
    class DoubleImplementation;
    class Integer;
    class ReturnReference;
    class ProtoTupleIteratorImplementation;
    class ProtoStringIteratorImplementation;
    class TupleDictionary;

    // Pointer Tagging
    union ProtoObjectPointer {
        const ProtoObject *oid;
        void *voidPointer;
        ProtoMethod method;
        const ProtoObjectCell *objectCell;
        const ProtoSparseList *sparseList;
        const ProtoSparseListIterator *sparseListIterator;
        const ProtoList *list;
        const ProtoListIterator *listIterator;
        const ProtoTuple *tuple;
        const ProtoTupleIterator *tupleIterator;
        const ProtoString *string;
        const ProtoStringIterator *stringIterator;
        const ProtoByteBuffer *byteBuffer;
        const ProtoExternalPointer *externalPointer;
        const ProtoExternalBuffer *externalBuffer;
        const ProtoThread *thread;
        const ProtoSet *set;
        const ProtoSetIterator *setIterator;
        const ProtoMultiset *multiset;
        const ProtoMultisetIterator *multisetIterator;
        const LargeIntegerImplementation *largeInteger;
        const DoubleImplementation *protoDouble;
        const ProtoMethodCell *methodCellImplementation;
        const ProtoObjectCell *objectCellImplementation;
        const ProtoSparseListImplementation *sparseListImplementation;
        const ProtoSparseListIteratorImplementation *sparseListIteratorImplementation;
        const ProtoListImplementation *listImplementation;
        const ProtoListSmallImplementation *listSmallImplementation;
        const ProtoListIteratorImplementation *listIteratorImplementation;
        const ProtoTupleImplementation *tupleImplementation;
        const ProtoStringImplementation *stringImplementation;
        const ProtoStringImplementation *symbolImplementation;  // POINTER_TAG_SYMBOL — same impl class, distinguished only by pointer tag
        const StringLeafNode *stringLeafNode;                   // POINTER_TAG_STRING_LEAF_NODE
        const StringInternalNode *stringInternalNode;           // POINTER_TAG_STRING_INTERNAL_NODE
        const ProtoRangeIteratorImplementation *rangeIteratorImplementation;
        const ProtoStringIteratorImplementation* stringIteratorImplementation;
        const ProtoTupleIteratorImplementation* tupleIteratorImplementation;
        const ProtoByteBufferImplementation *byteBufferImplementation;
        const ProtoExternalPointerImplementation *externalPointerImplementation;
        const ProtoExternalBufferImplementation *externalBufferImplementation;
        const ProtoThreadImplementation *threadImplementation;
        const ProtoSetImplementation *setImplementation;
        const ProtoSetIteratorImplementation *setIteratorImplementation;
        const ProtoMultisetImplementation *multisetImplementation;
        const ProtoMultisetIteratorImplementation *multisetIteratorImplementation;
        const LargeIntegerImplementation *largeIntegerImplementation;
        const DoubleImplementation *doubleImplementation;

        struct {
            unsigned long pointer_tag: 6;
            unsigned long embedded_type: 4;
            unsigned long value: 54;
        } op;
        struct {
            unsigned long pointer_tag: 6;
            unsigned long embedded_type: 4;
            long smallInteger: 54;
        } si;
        struct {
            unsigned long pointer_tag: 6;
            unsigned long embedded_type: 4;
            unsigned long unicodeValue: 54;
        } unicodeChar;
        struct {
            unsigned long pointer_tag: 6;
            unsigned long embedded_type: 4;
            unsigned long booleanValue: 1;
        } booleanValue;
        struct {
            unsigned long pointer_tag: 6;
            unsigned long embedded_type: 4;
            unsigned long byteData: 8;
        } byteValue;
        struct {
            unsigned long pointer_tag: 6;
            unsigned long embedded_type: 4;
            unsigned long year: 16;
            unsigned long month: 8;
            unsigned long day: 8;
        } date;
        struct {
            unsigned long pointer_tag: 6;
            unsigned long embedded_type: 4;
            unsigned long timestamp: 54;
        } timestampValue;
        struct {
            unsigned long pointer_tag: 6;
            unsigned long embedded_type: 4;
            long timedelta: 54;
        } timedeltaValue;
        struct {
            unsigned long pointer_tag: 6;
            unsigned long hash: 58;
        } asHash;
        /** Inline string UTF-8: byte_count (0..6) in bits 12..10; up to 6 UTF-8 bytes in bits 60..13. */
        struct {
            unsigned long pointer_tag: 6;
            unsigned long embedded_type: 4;
            unsigned long inline_byte_count: 3;   // 0..6 bytes
            unsigned long inline_utf8_bytes: 48;  // 6 bytes packed (LSB = first byte)
            unsigned long reserved: 3;
        } inlineString;
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
#define POINTER_TAG_EXTERNAL_BUFFER 20
#define POINTER_TAG_RANGE_ITERATOR 21
#define POINTER_TAG_SYMBOL            22   // Interned ProtoStringImplementation
#define POINTER_TAG_STRING_LEAF_NODE  23   // StringLeafNode (internal AVL leaf)
#define POINTER_TAG_STRING_INTERNAL_NODE 24 // StringInternalNode (internal AVL node)
#define POINTER_TAG_LIST_SMALL          25 // ProtoListSmallImplementation — inline-slot list (size ≤ 5)

#define EMBEDDED_TYPE_SMALLINT 0
#define EMBEDDED_TYPE_UNICODE_CHAR 2
#define EMBEDDED_TYPE_BOOLEAN 3
#define EMBEDDED_TYPE_INLINE_STRING 4
#define EMBEDDED_TYPE_NONE 5

#define INLINE_STRING_MAX_BYTES 6       // max UTF-8 bytes in embedded pointer
#define INLINE_STRING_BYTE_COUNT_BITS 3

    /** Returns byte count of an inline string pointer (0..6). */
    inline unsigned long inlineStringByteCount(const ProtoObject* o) {
        ProtoObjectPointer pa{}; pa.oid = o;
        return pa.inlineString.inline_byte_count;
    }

    /** Reads the i-th byte of an inline string (0-indexed, i < inlineStringByteCount). */
    inline uint8_t inlineStringByte(const ProtoObject* o, unsigned long i) {
        ProtoObjectPointer pa{}; pa.oid = o;
        return static_cast<uint8_t>((pa.inlineString.inline_utf8_bytes >> (i * 8)) & 0xFF);
    }

    /** Creates an inline string from up to 6 UTF-8 bytes. bytes must be valid UTF-8. */
    const ProtoObject* createInlineStringUTF8(ProtoContext* context,
                                               const uint8_t* bytes,
                                               uint8_t byte_count);

    bool isInlineString(const ProtoObject* o);
    unsigned long getProtoStringHash(ProtoContext* context, const ProtoObject* o);
    /** Builds inline string (no allocation). codepoints must be 0..127, len 0..6. */
    const ProtoObject* createInlineString(ProtoContext* context, int len, const unsigned int* codepoints);

#define ITERATOR_NEXT_PREVIOUS 0
#define ITERATOR_NEXT_THIS 1
#define ITERATOR_NEXT_NEXT 2

    // Helper to get the name of the type for error messages (compiler-specific, but useful for debugging)
    template<typename T>
    std::string getTypeName() {
        std::string name = __PRETTY_FUNCTION__;
        size_t start = name.find("T = ") + 4;
        size_t end = name.find("]", start);
        return name.substr(start, end - start);
    }

    // Trait to map implementation types to their expected pointer tags
    template<typename T>
    struct ExpectedTag;

    // Specializations for each implementation type
    template<> struct ExpectedTag<const Cell> { static constexpr unsigned long value = POINTER_TAG_OBJECT; };
    template<> struct ExpectedTag<Cell> { static constexpr unsigned long value = POINTER_TAG_OBJECT; };

    template<> struct ExpectedTag<const ParentLinkImplementation> { static constexpr unsigned long value = POINTER_TAG_OBJECT; };
    template<> struct ExpectedTag<ParentLinkImplementation> { static constexpr unsigned long value = POINTER_TAG_OBJECT; };

    template<> struct ExpectedTag<const ProtoListImplementation> { static constexpr unsigned long value = POINTER_TAG_LIST; };
    template<> struct ExpectedTag<ProtoListImplementation> { static constexpr unsigned long value = POINTER_TAG_LIST; };

    template<> struct ExpectedTag<const ProtoListSmallImplementation> { static constexpr unsigned long value = POINTER_TAG_LIST_SMALL; };
    template<> struct ExpectedTag<ProtoListSmallImplementation> { static constexpr unsigned long value = POINTER_TAG_LIST_SMALL; };

    template<> struct ExpectedTag<const ProtoSparseListImplementation> { static constexpr unsigned long value = POINTER_TAG_SPARSE_LIST; };
    template<> struct ExpectedTag<ProtoSparseListImplementation> { static constexpr unsigned long value = POINTER_TAG_SPARSE_LIST; };

    template<> struct ExpectedTag<const ProtoSetImplementation> { static constexpr unsigned long value = POINTER_TAG_SET; };
    template<> struct ExpectedTag<ProtoSetImplementation> { static constexpr unsigned long value = POINTER_TAG_SET; };

    template<> struct ExpectedTag<const ProtoMultisetImplementation> { static constexpr unsigned long value = POINTER_TAG_MULTISET; };
    template<> struct ExpectedTag<ProtoMultisetImplementation> { static constexpr unsigned long value = POINTER_TAG_MULTISET; };

    template<> struct ExpectedTag<const ProtoObjectCell> { static constexpr unsigned long value = POINTER_TAG_OBJECT; };
    template<> struct ExpectedTag<ProtoObjectCell> { static constexpr unsigned long value = POINTER_TAG_OBJECT; };

    template<> struct ExpectedTag<const ProtoStringImplementation> { static constexpr unsigned long value = POINTER_TAG_STRING; };
    template<> struct ExpectedTag<ProtoStringImplementation> { static constexpr unsigned long value = POINTER_TAG_STRING; };

    template<> struct ExpectedTag<const ProtoTupleImplementation> { static constexpr unsigned long value = POINTER_TAG_TUPLE; };
    template<> struct ExpectedTag<ProtoTupleImplementation> { static constexpr unsigned long value = POINTER_TAG_TUPLE; };

    template<> struct ExpectedTag<const ProtoMethodCell> { static constexpr unsigned long value = POINTER_TAG_METHOD; };
    template<> struct ExpectedTag<ProtoMethodCell> { static constexpr unsigned long value = POINTER_TAG_METHOD; };

    template<> struct ExpectedTag<const ProtoThreadImplementation> { static constexpr unsigned long value = POINTER_TAG_THREAD; };
    template<> struct ExpectedTag<ProtoThreadImplementation> { static constexpr unsigned long value = POINTER_TAG_THREAD; };
    
    template<> struct ExpectedTag<const ProtoRangeIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_RANGE_ITERATOR; };
    template<> struct ExpectedTag<ProtoRangeIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_RANGE_ITERATOR; };

    template<> struct ExpectedTag<const DoubleImplementation> { static constexpr unsigned long value = POINTER_TAG_DOUBLE; };
    template<> struct ExpectedTag<DoubleImplementation> { static constexpr unsigned long value = POINTER_TAG_DOUBLE; };

    template<> struct ExpectedTag<const LargeIntegerImplementation> { static constexpr unsigned long value = POINTER_TAG_LARGE_INTEGER; };
    template<> struct ExpectedTag<LargeIntegerImplementation> { static constexpr unsigned long value = POINTER_TAG_LARGE_INTEGER; };

    template<> struct ExpectedTag<const ProtoByteBufferImplementation> { static constexpr unsigned long value = POINTER_TAG_BYTE_BUFFER; };
    template<> struct ExpectedTag<ProtoByteBufferImplementation> { static constexpr unsigned long value = POINTER_TAG_BYTE_BUFFER; };

    template<> struct ExpectedTag<const ProtoExternalPointerImplementation> { static constexpr unsigned long value = POINTER_TAG_EXTERNAL_POINTER; };
    template<> struct ExpectedTag<ProtoExternalPointerImplementation> { static constexpr unsigned long value = POINTER_TAG_EXTERNAL_POINTER; };
    template<> struct ExpectedTag<const ProtoExternalBufferImplementation> { static constexpr unsigned long value = POINTER_TAG_EXTERNAL_BUFFER; };
    template<> struct ExpectedTag<ProtoExternalBufferImplementation> { static constexpr unsigned long value = POINTER_TAG_EXTERNAL_BUFFER; };

    template<> struct ExpectedTag<const ProtoListIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_LIST_ITERATOR; };
    template<> struct ExpectedTag<ProtoListIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_LIST_ITERATOR; };

    template<> struct ExpectedTag<const ProtoSparseListIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_SPARSE_LIST_ITERATOR; };
    template<> struct ExpectedTag<ProtoSparseListIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_SPARSE_LIST_ITERATOR; };

    template<> struct ExpectedTag<const ProtoSetIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_SET_ITERATOR; };
    template<> struct ExpectedTag<ProtoSetIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_SET_ITERATOR; };

    template<> struct ExpectedTag<const ProtoMultisetIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_MULTISET_ITERATOR; };
    template<> struct ExpectedTag<ProtoMultisetIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_MULTISET_ITERATOR; };

    template<> struct ExpectedTag<const ProtoTupleIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_TUPLE_ITERATOR; };
    template<> struct ExpectedTag<ProtoTupleIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_TUPLE_ITERATOR; };

    template<> struct ExpectedTag<const ProtoStringIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_STRING_ITERATOR; };
    template<> struct ExpectedTag<ProtoStringIteratorImplementation> { static constexpr unsigned long value = POINTER_TAG_STRING_ITERATOR; };

    template<> struct ExpectedTag<const StringLeafNode> {
        static constexpr unsigned long value = POINTER_TAG_STRING_LEAF_NODE;
    };
    template<> struct ExpectedTag<StringLeafNode> {
        static constexpr unsigned long value = POINTER_TAG_STRING_LEAF_NODE;
    };
    template<> struct ExpectedTag<const StringInternalNode> {
        static constexpr unsigned long value = POINTER_TAG_STRING_INTERNAL_NODE;
    };
    template<> struct ExpectedTag<StringInternalNode> {
        static constexpr unsigned long value = POINTER_TAG_STRING_INTERNAL_NODE;
    };


    // The new, safe toImpl implementation (non-const)
    template<typename Impl, typename Api>
    inline Impl *toImpl(Api *ptr) {
        if (!ptr || reinterpret_cast<const ProtoObject*>(ptr) == PROTO_NONE) {
            return nullptr;
        }

        ProtoObjectPointer p{};
        p.oid = reinterpret_cast<const ProtoObject*>(ptr); // Cast to const ProtoObject* for union access

        unsigned long actual_tag = p.op.pointer_tag;
        unsigned long expected_tag = ExpectedTag<Impl>::value;

        // Check for embedded values first, as they are not Cell-derived objects
        if (actual_tag == POINTER_TAG_EMBEDDED_VALUE) {
            std::cerr << "Error: Attempted to convert an embedded value (e.g., small int, boolean) to a Cell-derived type ("
                      << getTypeName<Impl>() << "). Embedded values are not Cell objects." << std::endl;
            std::abort();
        }

        // Check if the actual tag matches the expected tag
        if (actual_tag != expected_tag) {
            std::cerr << "Error: Type mismatch in toImpl conversion. Expected tag " << expected_tag
                      << " for type " << getTypeName<Impl>() << ", but found tag " << actual_tag
                      << " for ProtoObject* " << p.oid << "." << std::endl;
            std::abort();
        }

        // Clear the tag bits (lower 6 bits) to get the raw pointer to the Cell
        uintptr_t raw_ptr_value = reinterpret_cast<uintptr_t>(p.oid) & ~0x3FUL;

        // Check for 64-byte alignment (lowest 6 bits should be 0)
        if ((raw_ptr_value & 0x3FUL) != 0) {
            std::cerr << "Error: toImpl conversion resulted in an unaligned pointer for type "
                      << getTypeName<Impl>() << ". Raw pointer value: 0x" << std::hex << raw_ptr_value << std::dec
                      << ". Expected 64-byte alignment." << std::endl;
            std::abort();
        }

        return reinterpret_cast<Impl *>(raw_ptr_value);
    }

    // Overload for const pointers
    template<typename Impl, typename Api>
    inline const Impl *toImpl(const Api *ptr) {
        if (!ptr || reinterpret_cast<const ProtoObject*>(ptr) == PROTO_NONE) {
            return nullptr;
        }

        ProtoObjectPointer p{};
        p.oid = reinterpret_cast<const ProtoObject*>(ptr);

        unsigned long actual_tag = p.op.pointer_tag;
        unsigned long expected_tag = ExpectedTag<const Impl>::value; // Use const Impl for expected tag

        // Check for embedded values first, as they are not Cell-derived objects
        if (actual_tag == POINTER_TAG_EMBEDDED_VALUE) {
            std::cerr << "Error: Attempted to convert an embedded value (e.g., small int, boolean) to a Cell-derived type ("
                      << getTypeName<const Impl>() << "). Embedded values are not Cell objects." << std::endl;
            std::abort();
        }

        // Check if the actual tag matches the expected tag
        if (actual_tag != expected_tag) {
            std::cerr << "Error: Type mismatch in toImpl conversion. Expected tag " << expected_tag
                      << " for type " << getTypeName<const Impl>() << ", but found tag " << actual_tag
                      << " for ProtoObject* " << p.oid << "." << std::endl;
            std::abort();
        }

        // Check for 64-byte alignment of the original pointer when tag is OBJECT
        if (actual_tag == POINTER_TAG_OBJECT && (reinterpret_cast<uintptr_t>(p.oid) & 0x3FUL) != 0) {
            std::cerr << "Error: toImpl conversion found an unaligned pointer for type "
                      << getTypeName<const Impl>() << ". Pointer value: 0x" << std::hex << (uintptr_t)p.oid << std::dec
                      << ". Expected 64-byte alignment for tag 0." << std::endl;
            std::abort();
        }

        // Clear the tag bits (lower 6 bits) to get the raw pointer to the Cell
        uintptr_t raw_ptr_value = reinterpret_cast<uintptr_t>(p.oid) & ~0x3FUL;

        return reinterpret_cast<const Impl *>(raw_ptr_value);
    }

    unsigned long generate_mutable_ref(ProtoContext* context);
    bool isInteger(const ProtoObject* obj);
    bool isObject(const ProtoObject* obj);
    bool isCell(const ProtoObject* obj);

    // isObjectFast is defined later (after class Cell), once Cell's
    // member layout — in particular getCellTypeRaw() — is visible.

    enum class CellType {
        None,
        Object,
        List,
        Tuple,
        String,
        SparseList,
        Method,
        ExternalPointer,
        ExternalBuffer,
        Thread,
        LargeInteger,
        Double,
        Set,
        Multiset,
        MethodCell,
        SparseListIterator,
        ListIterator,
        TupleIterator,
        StringIterator,
        RangeIterator,
        SetIterator,
        MultisetIterator,
        ReturnReference,
        ParentLink,
        ThreadExtension,
        ByteBuffer,
        TupleDictionary,
        StringLeafNode,
        StringInternalNode,
        ListSmall
    };

    class Cell {
    public:
        // Layout:
        //   bit 0       : mark (GC tricolour) — owned EXCLUSIVELY by
        //                 the GC thread.  No mutator path may modify
        //                 it.  This single-writer invariant lets
        //                 setNext below stay a simple load+store
        //                 (no CAS loop) and lets sweep + post-sweep
        //                 unmark be plain atomic ops with no race.
        //   bits 1..5   : reserved, ALWAYS ZERO.  Previously held a
        //                 lazy-filled cached CellType (the now-removed
        //                 getCellTypeRaw scheme); that path fired a
        //                 fetch_or from ANY caller — mutator threads
        //                 included — and raced against sweep's
        //                 load+store setNext on the same word.  The
        //                 lost flag-bit write occasionally clobbered
        //                 the GC's mark bit on a live cell, producing
        //                 the survivor-pen UAF visible as
        //                 "'object' object has no attribute 'check'".
        //                 Removed entirely; isObjectFast falls back to
        //                 the pure tag check, which is sufficient
        //                 because every caller already filters by tag.
        //   bits 6..63  : Cell pointer (low 6 bits zero from 64-byte
        //                 alignment).
        std::atomic<uintptr_t> next_and_flags;

        explicit Cell(ProtoContext *context, Cell *n = nullptr);
        virtual ~Cell() = default;

        virtual CellType getType() const { return CellType::None; }


        virtual void finalize(ProtoContext *context) const {}

        virtual void
        processReferences(ProtoContext *context, void *self, void (*method)(ProtoContext *, void *, const Cell *)) const;

        virtual unsigned long getHash(ProtoContext *context) const;

        virtual const ProtoObject* implAsObject(ProtoContext *context) const = 0;

        static void* operator new(size_t size, ProtoContext *context);

        // Flags: bit 0 is Mark
        inline void mark() { next_and_flags.fetch_or(0x1UL); }
        inline void unmark() { next_and_flags.fetch_and(~0x1UL); }
        inline bool isMarked() const { return (next_and_flags.load() & 0x1UL) != 0; }

        inline Cell* getNext() const { return reinterpret_cast<Cell*>(next_and_flags.load() & ~0x3FUL); }
        inline void setNext(Cell* n) {
            // Plain load+store.  The mark bit is set/cleared via
            // fetch_or / fetch_and (atomic), and after the
            // getCellTypeRaw lazy-fill removal (commit 51459ce7) the
            // GC is the only writer to flag bits — and only during
            // STW (mark) or after STW with no concurrent setNext on
            // the same cell (sweep operates on segments captured at
            // STW; the cells inside are not touched by mutators
            // until they exit the sweep loop).  Verified empirically:
            // 100/100 pass on bench_binary_trees(10) without the CAS.
            // The 30 % flake the CAS originally protected against
            // (commit bdb63a26) was actually the lazy-fill cross-
            // thread fetch_or, removed two commits later.  See task
            // #28 of the perf investigation plan.
            uintptr_t newPtr = reinterpret_cast<uintptr_t>(n) & ~0x3FUL;
            uintptr_t current = next_and_flags.load(std::memory_order_relaxed);
            next_and_flags.store(newPtr | (current & 0x3FUL),
                                 std::memory_order_release);
        }
        // Use this for uninitialized memory or to reset everything
        inline void internalSetNextRaw(Cell* n) {
            // Even in raw mode, we should ensure the pointer part is clean if it carries flags
            next_and_flags.store(reinterpret_cast<uintptr_t>(n) & ~0x3FUL);
        }
    };

    /** Tag-check + virtual `getType()` to identify a ProtoObjectCell.
     *
     *  Several Cell subclasses share POINTER_TAG_OBJECT (low 6 bits
     *  == 0): the base Cell, ParentLinkImplementation, and
     *  ProtoObjectCell itself.  In hot user-code paths only
     *  ProtoObjectCell reaches isObjectFast, but tests and internal
     *  GC-callback paths sometimes hand it a ParentLink, so the
     *  tag-only check is too permissive — falsely identifying a
     *  ParentLink as an Object causes the caller to dereference its
     *  `attributes` field at the wrong offset.
     *
     *  Replaces the prior `(tag check) + (cached cellType bit
     *  comparison)` two-step.  The cached cellType bits in
     *  next_and_flags were lazy-filled via a fetch_or from any
     *  caller — including mutator threads — which violated the
     *  GC-only flag-bit invariant on next_and_flags and produced
     *  the survivor-pen UAF tracked under path #1.  The cache is
     *  removed; we now pay one virtual `getType()` per call instead.
     *  In hot getAttribute-shaped paths the call is replaced inline
     *  with `(ptr & 0x3F) == POINTER_TAG_OBJECT` because that path
     *  already invariantly visits ProtoObjectCells (see the
     *  getAttribute chain-walk loop in ProtoObject.cpp). */
    inline bool isObjectFast(const ProtoObject* obj) {
        if (!obj) return false;
        ProtoObjectPointer pa{}; pa.oid = obj;
        if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return false;
        return reinterpret_cast<const Cell*>(obj)->getType() == CellType::Object;
    }

    class ProtoRangeIteratorImplementation final : public Cell {
    public:
        long long current;
        long long stop;
        long long step;

        CellType getType() const override { return CellType::RangeIterator; }

        ProtoRangeIteratorImplementation(ProtoContext* context, long long start, long long stop, long long step)
            : Cell(context), current(start), stop(stop), step(step) {}

        const ProtoObject* implAsObject(ProtoContext* context) const override {
            ProtoObjectPointer p{};
            p.rangeIteratorImplementation = this;
            p.op.pointer_tag = POINTER_TAG_RANGE_ITERATOR;
            return p.oid;
        }

        const ProtoObject* implNext(ProtoContext* context) {
            bool done = (step > 0 && current >= stop) || (step < 0 && current <= stop);
            if (done) return nullptr;
            long long val = current;
            current += step;
            return context->fromInteger(val);
        }
    };

    class ParentLinkImplementation : public Cell {
    public:
        const ParentLinkImplementation *parent;
        const ProtoObject *object;

        CellType getType() const override { return CellType::ParentLink; }

        ParentLinkImplementation(ProtoContext *context, const ParentLinkImplementation *parent, const ProtoObject *object);

        ~ParentLinkImplementation() override = default;

        void processReferences(ProtoContext *context, void *self,
                               void (*method)(ProtoContext *, void *, const Cell *)) const override;

        const ProtoObject *implAsObject(ProtoContext *context) const override;

        const ProtoObject *getObject(ProtoContext *context) const;

        const ParentLinkImplementation *getParent(ProtoContext *context) const;
    };

    class ProtoObjectCell : public Cell {
    public:
        const ParentLinkImplementation *parent;
        const ProtoSparseListImplementation *attributes;
        const unsigned long mutable_ref;

        CellType getType() const override { return CellType::Object; }

        ProtoObjectCell(ProtoContext *context, const ParentLinkImplementation *parent,
                        const ProtoSparseListImplementation *attributes, unsigned long mutable_ref);

        ~ProtoObjectCell() override = default;

        const ProtoObjectCell *addParent(ProtoContext *context, const ProtoObject *newParentToAdd) const;

        const ProtoObject *asObject(ProtoContext *context) const;

        void finalize(ProtoContext *context) const;

        void processReferences(ProtoContext *context, void *self,
                               void (*method)(ProtoContext *, void *, const Cell *)) const override;

        const ProtoObject *implAsObject(ProtoContext *context) const override;
    };

    class ProtoMethodCell : public Cell {
    public:
        const ProtoObject *self;
        ProtoMethod method;

        CellType getType() const override { return CellType::MethodCell; }

        ProtoMethodCell(ProtoContext *context, const ProtoObject *selfObject, ProtoMethod methodTarget);

        const ProtoObject *
        implInvoke(ProtoContext *context, const ProtoList *args, const ProtoSparseList *kwargs) const;

        const ProtoObject *implAsObject(ProtoContext *context) const override;

        unsigned long getHash(ProtoContext *context) const override;

        void finalize(ProtoContext *context) const;

        void processReferences(ProtoContext *context, void *self,
                               void (*method)(ProtoContext *context, void *self, const Cell *cell)) const override;

        const ProtoObject *implGetSelf(ProtoContext *context) const;

        ProtoMethod implGetMethod(ProtoContext *context) const;
    };

    class DoubleImplementation : public Cell {
    public:
        double doubleValue;

        CellType getType() const override { return CellType::Double; }

        DoubleImplementation(ProtoContext *context, double val);

        ~DoubleImplementation() override = default;

        unsigned long getHash(ProtoContext *context) const override;

        void finalize(ProtoContext *context) const;

        void processReferences(ProtoContext *context, void *self,
                               void (*method)(ProtoContext *context, void *self, const Cell *cell)) const override;

        const ProtoObject *implAsObject(ProtoContext *context) const override;
    };

    class LargeIntegerImplementation : public Cell {
    public:
        static const int DIGIT_COUNT = 4;
        bool is_negative;
        unsigned long long digits[DIGIT_COUNT];
        LargeIntegerImplementation *next;

        CellType getType() const override { return CellType::LargeInteger; }

        LargeIntegerImplementation(ProtoContext *context);

        ~LargeIntegerImplementation() override = default;

        unsigned long getHash(ProtoContext *context) const override;

        void finalize(ProtoContext *context) const;

        void processReferences(ProtoContext *context, void *self,
                               void (*method)(ProtoContext *, void *, const Cell *)) const override;

        const ProtoObject *implAsObject(ProtoContext *context) const override;
    };

    class ProtoByteBufferImplementation : public Cell {
    public:
        char *buffer;
        unsigned long size;
        bool freeOnExit;

        CellType getType() const override { return CellType::ByteBuffer; }

        ProtoByteBufferImplementation(ProtoContext *context, char *buffer, unsigned long size, bool freeOnExit);

        ~ProtoByteBufferImplementation() override;

        char implGetAt(ProtoContext *context, int index) const;

        void implSetAt(ProtoContext *context, int index, char value);

        void processReferences(ProtoContext *context, void *self,
                               void (*method)(ProtoContext *, void *, const Cell *)) const override;

        void finalize(ProtoContext *context) const;

        const ProtoObject *implAsObject(ProtoContext *context) const override;

        const ProtoByteBuffer *asByteBuffer(ProtoContext *context) const;

        unsigned long getHash(ProtoContext *context) const override;

        unsigned long implGetSize(ProtoContext *context) const;

        char *implGetBuffer(ProtoContext *context) const;
    };

    class ProtoStringImplementation final : public Cell {
    public:
        const ProtoObject* avl_root;  // StringLeafNode, StringInternalNode, or nullptr (empty)

        CellType getType() const override { return CellType::String; }

        explicit ProtoStringImplementation(ProtoContext* ctx, const ProtoObject* root);
        ~ProtoStringImplementation() override = default;

        // New AVL-based core API
        uint32_t implGetSize()  const;
        uint64_t implGetHash()  const;

        const ProtoObject* implAsObject(ProtoContext* ctx) const override;
        const ProtoString* asProtoString(ProtoContext* ctx) const;
        const ProtoStringImplementation* implAsSymbol(ProtoContext* ctx) const;

        static const ProtoStringImplementation* fromUTF8Bytes(ProtoContext* ctx,
                                                               const uint8_t* bytes,
                                                               size_t len);

        // Legacy compatibility methods (used by RopeCharacterIterator and ProtoString public API)
        unsigned long getHash(ProtoContext* context) const override;
        const ProtoObject* implGetAt(ProtoContext* context, int index) const;
        unsigned long implGetSizeCompat(ProtoContext* context) const;
        const ProtoList* implAsList(ProtoContext* context) const;
        const ProtoStringImplementation* implAppendLast(ProtoContext* context, const ProtoString* otherString) const;
        void finalize(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self,
                               void (*method)(ProtoContext*, void*, const Cell*)) const override;
        int implCompare(ProtoContext* context, const ProtoString* other) const;
        const ProtoStringIteratorImplementation* implGetIterator(ProtoContext* context) const;
    };

    // ---- SymbolTable ----------------------------------------------------------
    // 64-shard concurrent interning table. Replaces TupleDictionary for strings.
    // Strong symbols (from literals) are never collected.
    // Weak symbols (auto-interned) are removed by GC sweep before cell reclaim.
    class SymbolTable {
    public:
        static constexpr int SHARD_COUNT = 64;

        struct Bucket {
            uint64_t           content_hash;
            const ProtoObject* symbol;          // POINTER_TAG_SYMBOL pointer
            bool               is_strong;
            Bucket*            next;
        };

        struct Shard {
            std::mutex  mutex;
            Bucket*     head = nullptr;
        };

        Shard shards[SHARD_COUNT];

        SymbolTable()  = default;
        ~SymbolTable();

        const ProtoObject* intern(ProtoContext* ctx,
                                   const ProtoObject* strObj,
                                   bool is_strong = false);

        // Read-only lookup — returns existing symbol if found, nullptr if not interned.
        // Does NOT insert into the table. Safe to call on hot read paths.
        const ProtoObject* lookupByContent(ProtoContext* ctx,
                                            const ProtoObject* strObj) const;

        void removeWeak(uint64_t content_hash, const ProtoObject* symbol);

        static bool isSymbol(const ProtoObject* obj);

    private:
        int shardIndex(uint64_t hash) const {
            return static_cast<int>(hash & (SHARD_COUNT - 1));
        }
        static bool contentEqual(ProtoContext* ctx,
                                  const ProtoObject* a, const ProtoObject* b);
        // Build a fresh ProtoStringImplementation with the same content
        // as `strObj`.  `readCtx` traverses `strObj`; `allocCtx` owns
        // every new Cell.  Pass `allocCtx = nullptr` for strong-intern
        // (perpetual) symbols — see SymbolTable.cpp for the rationale.
        static const ProtoStringImplementation* normalizeForSymbol(
            ProtoContext* readCtx, ProtoContext* allocCtx,
            const ProtoObject* strObj);
    };

    // ---- StringLeafNode -------------------------------------------------------
    // 64-byte Cell. Stores up to 32 bytes of UTF-8 content in one contiguous chunk.
    // Layout (64 bytes total):
    //   Cell base (vtable 8 + next_and_flags 8) = 16 bytes
    //   byte_count      (1) : bytes used in utf8_payload (0..32)
    //   _pad_char_count (1) : explicit alignment padding
    //   char_count      (2) : Unicode codepoints in this leaf
    //   flags           (1) : bit 0 = is_partial
    //   _pad            (3) : explicit padding to 8-byte-align content_hash
    //   content_hash    (8) : FNV-1a of utf8_payload[0..byte_count)
    //   utf8_payload   (32) : UTF-8 bytes
    //   Total: 16 + 1+1+2+1+3+8+32 = 64 bytes
    //
    // NOTE: The spec §3.1 specifies utf8_payload[48] assuming a zero-size Cell base,
    // but Cell carries a vtable pointer (8) and next_and_flags (8) = 16 bytes, leaving
    // only 32 bytes for the payload. content_hash is present as a real field per spec.
    class StringLeafNode final : public Cell {
    public:
        uint8_t  byte_count;                   // bytes used in utf8_payload (0..32)
        uint8_t  _pad_char_count;              // explicit alignment padding
        uint16_t char_count;                   // Unicode codepoints in this leaf
        uint8_t  flags;                        // bit 0: is_partial
        uint8_t  _pad[3];                      // explicit padding to 8-byte-align content_hash
        uint64_t content_hash;                 // FNV-1a of utf8_payload[0..byte_count)
        uint8_t  utf8_payload[32];

        static constexpr uint8_t MAX_PAYLOAD        = 32;
        static constexpr uint8_t PARTIAL_THRESHOLD  = 8;
        static constexpr uint8_t MERGE_FILL         = 16;

        StringLeafNode(ProtoContext* ctx,
                       const uint8_t* bytes, uint8_t byte_cnt,
                       uint16_t char_cnt, bool partial = false);

        bool isPartial() const { return (flags & 1) != 0; }
        uint32_t charToByteOffset(uint32_t char_index) const;
        uint32_t codepointAt(uint32_t byte_pos) const;

        const ProtoObject* asObject() const;
        static const StringLeafNode* fromObject(const ProtoObject* obj);
        static bool isStringLeafNode(const ProtoObject* obj);

        CellType getType() const override { return CellType::StringLeafNode; }
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        void processReferences(ProtoContext* context, void* self,
                               void (*method)(ProtoContext*, void*, const Cell*)) const override {}

    private:
        static uint64_t computeHash(const uint8_t* bytes, uint8_t len);
    };
    static_assert(sizeof(StringLeafNode) == 64, "StringLeafNode must be exactly one 64-byte Cell");

    // ---- StringInternalNode ---------------------------------------------------
    // 64-byte Cell. AVL internal node for the string tree.
    // Layout (64 bytes total):
    //   Cell base (vtable 8 + next_and_flags 8) = 16 bytes
    //   left (8) + right (8) = 16 bytes
    //   total_chars (4) + left_chars (4) + total_bytes (4) + implicit pad (4) = 16 bytes
    //   subtree_hash (8) = 8 bytes
    //   height (1) + _pad[7] (7) = 8 bytes
    class StringInternalNode final : public Cell {
    public:
        const ProtoObject* left;
        const ProtoObject* right;
        uint32_t total_chars;
        uint32_t left_chars;
        uint32_t total_bytes;
        uint32_t _pad_align;        // explicit pad to 8-byte-align subtree_hash
        uint64_t subtree_hash;
        uint8_t  height;
        uint8_t  _pad[7];

        StringInternalNode(ProtoContext* ctx,
                           const ProtoObject* l, const ProtoObject* r);

        const ProtoObject* asObject() const;
        static const StringInternalNode* fromObject(const ProtoObject* obj);
        static bool isStringInternalNode(const ProtoObject* obj);

        static int      nodeHeight(const ProtoObject* n);
        static int      balance(const ProtoObject* n);
        static uint64_t subtreeHash(const ProtoObject* n);
        static uint32_t charCount(const ProtoObject* n);
        static uint32_t byteCount(const ProtoObject* n);

        CellType getType() const override { return CellType::StringInternalNode; }
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        void processReferences(ProtoContext* context, void* self,
                               void (*method)(ProtoContext*, void*, const Cell*)) const override;
    };
    static_assert(sizeof(StringInternalNode) == 64, "StringInternalNode must be exactly one 64-byte Cell");

    class ProtoTupleImplementation : public Cell {
    public:
        const ProtoObject *slot[TUPLE_SIZE];
        unsigned long actual_size : 63; // The actual number of elements this node (or its children) represents

        CellType getType() const override { return CellType::Tuple; }

        ProtoTupleImplementation(ProtoContext *context, const ProtoObject **slot_values,
                                 unsigned long size);
        ~ProtoTupleImplementation() override = default;
        static const ProtoTupleImplementation *
        tupleFromVector(ProtoContext *context, const std::vector<const ProtoObject *>& source);
        static const ProtoTupleImplementation *
        tupleFromList(ProtoContext *context, const ProtoListImplementation *sourceList);
        /** Builds a non-interned concat tuple for rope: slot[0]=left, slot[1]=right, actual_size=totalSize. O(1). */
        static const ProtoTupleImplementation *
        tupleConcat(ProtoContext *context, const ProtoObject *left, const ProtoObject *right, unsigned long totalSize);

        const ProtoObject *implAsObject(ProtoContext *context) const override;
        const ProtoTuple *asProtoTuple(ProtoContext *context) const;
        const ProtoObject* implGetAt(ProtoContext* context, int index) const;
        unsigned long implGetSize(ProtoContext* context) const;
        const ProtoList* implAsList(ProtoContext* context) const;
        void finalize(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        unsigned long getHash(ProtoContext* context) const override;
    };

    /** Compute a structure-independent content hash over all leaf bytes. Defined in ProtoString.cpp. */
    uint64_t computeContentHash(const ProtoObject* node);

    struct StringInternHash {
        size_t operator()(const ProtoStringImplementation* s) const {
            if (!s) return 0;
            return static_cast<size_t>(computeContentHash(s->avl_root));
        }
    };

    struct StringInternEqual {
        bool operator()(const ProtoStringImplementation* a, const ProtoStringImplementation* b) const {
            if (a == b) return true;
            if (!a || !b) return false;
            if (a->implGetSize() != b->implGetSize()) return false;
            uint64_t ha = computeContentHash(a->avl_root);
            uint64_t hb = computeContentHash(b->avl_root);
            return ha == hb;
        }
    };

    using StringInternSet = std::unordered_set<const ProtoStringImplementation*, StringInternHash, StringInternEqual>;

    class ProtoSetImplementation : public Cell {
    public:
        const ProtoSparseList *list;
        unsigned long size;

        CellType getType() const override { return CellType::Set; }

        ProtoSetImplementation(ProtoContext *context, const ProtoSparseList *list, unsigned long size);

        const ProtoObject *implAsObject(ProtoContext *context) const override;

        const ProtoSet *asProtoSet(ProtoContext *context) const;
        void processReferences(ProtoContext *context, void *self, void (*method)(ProtoContext *, void *, const Cell *)) const override;
    };

    class ProtoMultisetImplementation : public Cell {
    public:
        const ProtoSparseList *list;
        unsigned long size;

        CellType getType() const override { return CellType::Multiset; }

        ProtoMultisetImplementation(ProtoContext *context, const ProtoSparseList *list, unsigned long size);

        const ProtoObject *implAsObject(ProtoContext *context) const override;

        const ProtoMultiset *asProtoMultiset(ProtoContext *context) const;
        void processReferences(ProtoContext *context, void *self, void (*method)(ProtoContext *, void *, const Cell *)) const override;
    };

    struct AttributeCacheEntry {
        const ProtoObject* object;
        const ProtoObject* result;
        const ProtoString* name;
    };

    /**
     * @brief Per-thread cache entry for mutable-object snapshot resolution.
     *
     * The cache short-circuits the "load mutableRoot[shard] + AVL implGetAt(mutable_ref)"
     * sequence on the common own-thread read path. Validity is established by pointer
     * equality on shard_root: if the cached shard_root still matches the current shard
     * root pointer, the cached current_value is the live snapshot. Any successful CAS
     * by any thread (including this one) replaces shard_root and naturally invalidates
     * stale entries on the next lookup.
     *
     * Both shard_root and current_value are GC roots: ProtoThreadExtension::processReferences
     * traces them so the GC cannot reclaim a snapshot still referenced by a cached entry.
     */
    struct MutableValueCacheEntry {
        unsigned long       mutable_ref;     // 0 = empty entry
        ProtoSparseList*    shard_root;      // shard root pointer at the time we cached
        const ProtoObject*  current_value;   // resolved snapshot
    };

    class ProtoThreadExtension : public Cell {
    public:
        std::thread* osThread;
        Cell* freeCells;
        AttributeCacheEntry* attributeCache;
        MutableValueCacheEntry* mutableValueCache;

        CellType getType() const override { return CellType::ThreadExtension; }

        ProtoThreadExtension(ProtoContext* context);
        ~ProtoThreadExtension() override;
        void finalize(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        const ProtoObject *implAsObject(ProtoContext *context) const override;
    };

    class ProtoThreadImplementation : public Cell {
    public:
        CellType getType() const override { return CellType::Thread; }
        ProtoContext *context;
        ProtoThreadExtension* extension;
        const ProtoString* name;
        ProtoSpace* space;
        const ProtoList* args;
        const ProtoSparseList* kwargs;

        ProtoThreadImplementation(ProtoContext *context, const ProtoString* name, ProtoSpace* space, ProtoMethod main, const ProtoList* args, const ProtoSparseList* kwargs);

        /** Tag-dispatched constructor used by ProtoSpace for the OS process's
         *  main thread.  Adopts the supplied `mainContext` as `this->context`
         *  rather than allocating a new ProtoContext, and does NOT spawn an
         *  `std::thread` (we ARE the OS thread).  Builds the per-thread
         *  extension (attribute cache, mutable-value cache, free-cells
         *  pool) so that `mainContext->thread` is non-null and the
         *  caches are reachable on every getAttribute / setAttribute
         *  call from the main thread. */
        struct AdoptMainThreadTag {};
        ProtoThreadImplementation(AdoptMainThreadTag, ProtoContext* mainContext,
                                   const ProtoString* name, ProtoSpace* space);

        ~ProtoThreadImplementation() override;

        Cell *implAllocCell(ProtoContext *context);
        const ProtoObject *implAsObject(ProtoContext *context) const override;
        const ProtoThread* asThread(ProtoContext* context) const;
        void implSynchToGC();
        void implSetCurrentContext(ProtoContext* context);
        void finalize(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        unsigned long getHash(ProtoContext* context) const override;
    };


    class ReturnReference : public Cell {
    public:
        Cell *returnValue;

        CellType getType() const override { return CellType::ReturnReference; }

        ReturnReference(ProtoContext *context, Cell *rv);

        void
        processReferences(ProtoContext *context, void *self, void (*method)(ProtoContext *, void *, const Cell *)) const override;

        const ProtoObject *implAsObject(ProtoContext *context) const override;
    };

    class ProtoListImplementation final : public Cell {
    public:
        const ProtoObject *value;
        const ProtoListImplementation *previousNode;
        const ProtoListImplementation *nextNode;
        const unsigned long hash;
        const unsigned long size;
        const unsigned char height;
        const bool isEmpty;

        CellType getType() const override { return CellType::List; }

        explicit ProtoListImplementation(ProtoContext *context,
                                         const ProtoObject *v = nullptr,
                                         bool empty = true,
                                         const ProtoListImplementation *prev = nullptr,
                                         const ProtoListImplementation *next = nullptr);

        const ProtoObject *implGetAt(ProtoContext *context, int index) const;

        bool implHas(ProtoContext *context, const ProtoObject *targetValue) const;

        const ProtoListImplementation *implSetAt(ProtoContext *context, int index, const ProtoObject *newValue) const;

        const ProtoListImplementation *
        implInsertAt(ProtoContext *context, int index, const ProtoObject *newValue) const;

        const ProtoListImplementation *implAppendLast(ProtoContext *context, const ProtoObject *newValue) const;

        const ProtoListImplementation *implRemoveAt(ProtoContext *context, int index) const;

        const ProtoObject *implAsObject(ProtoContext *context) const override;

        const ProtoList *asProtoList(ProtoContext *context) const;

        const ProtoListIteratorImplementation *implGetIterator(ProtoContext *context) const;
        void processReferences(ProtoContext *context, void *self, void (*method)(ProtoContext *, void *, const Cell *)) const override;
    };

    class ProtoListSmallImplementation final : public Cell {
    public:
        // Inline-storage list: holds up to MAX_INLINE elements in a single
        // 64-byte Cell. The dense slot array avoids per-element AVL
        // allocations (1 + N → 1 cells for N ≤ MAX_INLINE).
        // Output form selection (not promotion) is done at every mutator:
        // operations whose result fits stay in this form; operations whose
        // result exceeds MAX_INLINE produce an AVL ProtoListImplementation.
        // Iteration is handled by retyping ProtoListIteratorImplementation::base
        // to const Cell* and dispatching by tag in implNext / implAdvance.
        static constexpr unsigned MAX_INLINE = 5;

        unsigned long size;                    // 0..MAX_INLINE
        const ProtoObject *slots[MAX_INLINE];  // dense; [0..size) valid

        CellType getType() const override { return CellType::ListSmall; }

        explicit ProtoListSmallImplementation(ProtoContext *context, unsigned n,
                                              const ProtoObject *const *items);

        const ProtoObject *implGetAt(ProtoContext *context, int index) const;
        bool implHas(ProtoContext *context, const ProtoObject *targetValue) const;
        const ProtoObject *implAsObject(ProtoContext *context) const override;
        const ProtoList *asProtoList(ProtoContext *context) const;
        void processReferences(ProtoContext *context, void *self,
                               void (*method)(ProtoContext *, void *, const Cell *)) const override;
    };

    class ProtoListIteratorImplementation final : public Cell {
    public:
        // base is stored as a tagged ProtoObject* so the iterator can carry
        // either an AVL ProtoListImplementation (POINTER_TAG_LIST) or a
        // ProtoListSmallImplementation (POINTER_TAG_LIST_SMALL); implNext /
        // implAdvance dispatch on the pointer tag, no virtual call.
        const ProtoObject *base;
        unsigned long currentIndex;

        CellType getType() const override { return CellType::ListIterator; }

        ProtoListIteratorImplementation(ProtoContext *context, const ProtoObject *b, unsigned long index);

        int implHasNext() const;

        const ProtoObject *implNext(ProtoContext *context) const;

        const ProtoListIteratorImplementation *implAdvance(ProtoContext *context) const;

        const ProtoObject *implAsObject(ProtoContext *context) const override;

        const ProtoListIterator *asProtoListIterator(ProtoContext *context) const;

        void
        processReferences(ProtoContext *context, void *self, void (*method)(ProtoContext *, void *, const Cell *)) const override;
    };

    class ProtoSparseListImplementation final : public Cell {
    public:
        const unsigned long key;
        const ProtoObject *value;
        const ProtoSparseListImplementation *previous;
        const ProtoSparseListImplementation *next;
        const unsigned long hash;
        unsigned long size: 24;
        unsigned long height: 8;
        unsigned long isEmpty: 1;

        CellType getType() const override { return CellType::SparseList; }

        ProtoSparseListImplementation(ProtoContext *context, unsigned long k, const ProtoObject *v,
                                      const ProtoSparseListImplementation *p, const ProtoSparseListImplementation *n,
                                      bool empty);

        bool implHas(ProtoContext *context, unsigned long offset) const;

        const ProtoObject *implGetAt(ProtoContext *context, unsigned long offset) const;

        const ProtoSparseListImplementation *
        implSetAt(ProtoContext *context, unsigned long offset, const ProtoObject *newValue) const;

        const ProtoSparseListImplementation *implRemoveAt(ProtoContext *context, unsigned long offset) const;

        const ProtoObject *implAsObject(ProtoContext *context) const override;

        const ProtoSparseList *asSparseList(ProtoContext *context) const;

        const ProtoSparseListIteratorImplementation *implGetIterator(ProtoContext *context) const;
        const ProtoSparseListIteratorImplementation *implGetIteratorWithQueue(ProtoContext *context, const ProtoSparseListIteratorImplementation* queue) const;
        void processReferences(ProtoContext *context, void *self, void (*method)(ProtoContext *, void *, const Cell *)) const override;
    };

    class ProtoSparseListIteratorImplementation final : public Cell {
        friend class ProtoSparseListImplementation;

    private:
        const int state;
        const ProtoSparseListImplementation *current;
        const ProtoSparseListIteratorImplementation *queue;
    public:
        CellType getType() const override { return CellType::SparseListIterator; }

        ProtoSparseListIteratorImplementation(ProtoContext *context, int s, const ProtoSparseListImplementation *c,
                                              const ProtoSparseListIteratorImplementation *q);

        int implHasNext() const;

        unsigned long implNextKey() const;

        const ProtoObject *implNextValue() const;

        const ProtoSparseListIteratorImplementation *implAdvance(ProtoContext *context) const;

        const ProtoObject *implAsObject(ProtoContext *context) const override;

        void
        processReferences(ProtoContext *context, void *self, void (*method)(ProtoContext *, void *, const Cell *)) const override;
    };

    class Integer {
    public:
        static const ProtoObject *fromLong(ProtoContext *context, long long value);

        static const ProtoObject *fromString(ProtoContext *context, const char *str, int base);

        static long long asLong(ProtoContext *context, const ProtoObject *object);

        static const ProtoObject *negate(ProtoContext *context, const ProtoObject *object);

        static const ProtoObject *abs(ProtoContext *context, const ProtoObject *object);

        static int sign(ProtoContext *context, const ProtoObject *object);

        static int compare(ProtoContext *context, const ProtoObject *left, const ProtoObject *right);

        static const ProtoObject *add(ProtoContext *context, const ProtoObject *left, const ProtoObject *right);

        static const ProtoObject *subtract(ProtoContext *context, const ProtoObject *left, const ProtoObject *right);

        static const ProtoObject *multiply(ProtoContext *context, const ProtoObject *left, const ProtoObject *right);

        static const ProtoObject *divide(ProtoContext *context, const ProtoObject *left, const ProtoObject *right);

        static const ProtoObject *modulo(ProtoContext *context, const ProtoObject *left, const ProtoObject *right);

        static const ProtoString *toString(ProtoContext *context, const ProtoObject *object, int base);

        static const ProtoObject *bitwiseNot(ProtoContext *context, const ProtoObject *object);

        static const ProtoObject *
        bitwiseAnd(ProtoContext *context, const ProtoObject *left, const ProtoObject *right);

        static const ProtoObject *bitwiseOr(ProtoContext *context, const ProtoObject *left, const ProtoObject *right);

        static const ProtoObject *bitwiseXor(ProtoContext *context, const ProtoObject *left, const ProtoObject *right);

        static const ProtoObject *shiftLeft(ProtoContext *context, const ProtoObject *object, int amount);

        static const ProtoObject *shiftRight(ProtoContext *context, const ProtoObject *object, int amount);
    };

    class ProtoSetIteratorImplementation : public Cell {
    public:
        const ProtoSparseListIteratorImplementation* iterator;

        CellType getType() const override { return CellType::SetIterator; }

        ProtoSetIteratorImplementation(ProtoContext* context, const ProtoSparseListIteratorImplementation* it);
        int implHasNext(ProtoContext* context) const;
        const ProtoObject* implNext(ProtoContext* context);
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        inline const ProtoSetIterator* asSetIterator(ProtoContext* context) const {
            ProtoObjectPointer p;
            p.setIteratorImplementation = this;
            p.op.pointer_tag = POINTER_TAG_SET_ITERATOR;
            return p.setIterator;
        }
    };

    class ProtoMultisetIteratorImplementation : public Cell {
    public:
        const ProtoSparseListIteratorImplementation* iterator;

        CellType getType() const override { return CellType::MultisetIterator; }

        ProtoMultisetIteratorImplementation(ProtoContext* context, const ProtoSparseListIteratorImplementation* it);
        int implHasNext(ProtoContext* context) const;
        const ProtoObject* implNext(ProtoContext* context);
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        inline const ProtoMultisetIterator* asMultisetIterator(ProtoContext* context) const {
            ProtoObjectPointer p;
            p.multisetIteratorImplementation = this;
            p.op.pointer_tag = POINTER_TAG_MULTISET_ITERATOR;
            return p.multisetIterator;
        }
    };

    class ProtoTupleIteratorImplementation : public Cell {
    public:
        const ProtoTupleImplementation* base;
        int currentIndex;

        CellType getType() const override { return CellType::TupleIterator; }

        ProtoTupleIteratorImplementation(ProtoContext* context, const ProtoTupleImplementation* t, int i);
        ~ProtoTupleIteratorImplementation() override = default;
        int implHasNext(ProtoContext* context) const;
        const ProtoObject* implNext(ProtoContext* context);
        const ProtoTupleIteratorImplementation* implAdvance(ProtoContext* context) const;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        const ProtoTupleIterator* asProtoTupleIterator(ProtoContext* context) const;
        void finalize(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoStringIteratorImplementation : public Cell {
    public:
        // Cell layout (64 bytes total):
        //   Cell base (vtable 8 + next_and_flags 8) = 16 bytes
        //   base*         8 bytes — root string ProtoObject* (inline or ProtoStringImplementation)
        //   totalSize     4 bytes — cached codepoint count, enables O(1) implHasNext()
        //   charIndex     4 bytes — current codepoint position
        //   currentLeaf*  8 bytes — active StringLeafNode* (nullptr = needs descent)
        //   leafBytePos   1 byte  — byte offset within currentLeaf->utf8_payload
        //                           uint8_t is sufficient: utf8_payload is 32 bytes and
        //                           byte_count is already uint8_t, so max value is 32.
        //   _pad         15 bytes — explicit padding to reach 64 bytes
        //   Total: 16 + 8 + 4 + 4 + 8 + 1 + 15 = 56... pad to 64: remaining 8 more bytes covered by alignment
        //
        // Traversal strategy: O(1) amortized per codepoint.
        //   - Within a leaf: decode UTF-8 at leafBytePos, advance leafBytePos and charIndex. O(1).
        //   - Leaf boundary: descend AVL tree using charIndex to locate the next leaf. O(log N)
        //     but amortised over the leaf's codepoints (~10-32 chars), giving O(1) amortised overall.
        //   - implHasNext(): charIndex < totalSize — always O(1).

        const ProtoObject*       base;         // Root string (inline or ProtoStringImplementation).
        uint32_t                 totalSize;    // Cached total codepoint count.
        uint32_t                 charIndex;    // Current codepoint position.
        const StringLeafNode*    currentLeaf;  // Active leaf, or nullptr when descent is needed.
        uint8_t                  leafBytePos;  // Byte offset within currentLeaf->utf8_payload (max 32).
        uint8_t                  _pad[15];     // Explicit padding — do not use.

        CellType getType() const override { return CellType::StringIterator; }

        ProtoStringIteratorImplementation(ProtoContext* context, const ProtoObject* stringObj, uint32_t i);
        ~ProtoStringIteratorImplementation() override = default;
        int implHasNext(ProtoContext* context) const;
        const ProtoObject* implNext(ProtoContext* context);
        const ProtoStringIteratorImplementation* implAdvance(ProtoContext* context) const;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        const ProtoStringIterator* asProtoStringIterator(ProtoContext* context) const;
        void finalize(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        unsigned long getHash(ProtoContext* context) const;

    private:
        /** Descend the AVL tree rooted at avl_root to find the leaf containing
         *  codepoint at position charIndex, then set currentLeaf and leafBytePos.
         *  Called once per leaf boundary — O(log N) — amortised O(1) per codepoint.
         */
        void locateLeaf(const ProtoObject* avl_root);
    };

    class ProtoExternalPointerImplementation : public Cell {
    public:
        void* pointer;
        void (*finalizer)(void*);

        CellType getType() const override { return CellType::ExternalPointer; }

        ProtoExternalPointerImplementation(ProtoContext* context, void* p, void (*f)(void*));
        ~ProtoExternalPointerImplementation() override;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        void* implGetPointer(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        void finalize(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const override;
    };

    /** 64-byte header cell; segment allocated with aligned_alloc. Shadow GC: finalize() frees segment when cell is collected. */
    class ProtoExternalBufferImplementation : public Cell {
    public:
        mutable void* segment;
        unsigned long size;

        CellType getType() const override { return CellType::ExternalBuffer; }

        ProtoExternalBufferImplementation(ProtoContext* context, unsigned long bufferSize);
        ~ProtoExternalBufferImplementation() override;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        void* implGetRawPointer(ProtoContext* context) const;
        unsigned long implGetSize(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        void finalize(ProtoContext* context) const override;
        unsigned long getHash(ProtoContext* context) const override;
    };

    class TupleDictionary : public Cell {
    public:
        CellType getType() const override { return CellType::TupleDictionary; }
        const ProtoTupleImplementation* key;
        TupleDictionary* previous;
        TupleDictionary* next;
        int height;

        TupleDictionary(ProtoContext* context, const ProtoTupleImplementation* key, TupleDictionary* previous, TupleDictionary* next);
        void finalize(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
    };

    class BigCell final {
        union {
            char byteData[64] = {};
            ProtoObjectCell objectCell;
            ProtoListIteratorImplementation listIteratorCell;
            ProtoListImplementation listCell;
            ProtoListSmallImplementation listSmallCell;
            ProtoSparseListIteratorImplementation sparseListIteratorCell;
            ProtoSparseListImplementation sparseListCell;
            ProtoTupleIteratorImplementation tupleIteratorCell;
            ProtoTupleImplementation tupleCell;
            ProtoStringIteratorImplementation stringIteratorCell;
            ProtoStringImplementation stringCell;
            ProtoByteBufferImplementation byteBufferCell;
            ProtoMethodCell methodCell;
            ProtoExternalPointerImplementation externalPointerCell;
            ProtoExternalBufferImplementation externalBufferCell;
            ProtoThreadImplementation threadCell;
            ProtoThreadExtension threadExtensionCell;
            LargeIntegerImplementation largeIntegerCell;
            DoubleImplementation doubleCell;
            ProtoSetIteratorImplementation setIteratorCell;
            ProtoMultisetIteratorImplementation multisetIteratorCell;
            TupleDictionary tupleDictionary;
        };
    };

    static_assert(sizeof(BigCell) <= 64, "BigCell exceeds 64 bytes!!!!");
    static_assert(sizeof(ProtoObjectCell) <= 64, "ProtoObjectCell exceeds 64 bytes!");
    static_assert(sizeof(ProtoListIteratorImplementation) <= 64, "ProtoListIteratorImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoListImplementation) <= 64, "ProtoListImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoListSmallImplementation) <= 64, "ProtoListSmallImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoSparseListIteratorImplementation) <= 64, "ProtoSparseListIteratorImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoSparseListImplementation) <= 64, "ProtoSparseListImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoTupleIteratorImplementation) <= 64, "ProtoTupleIteratorImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoTupleImplementation) <= 64, "ProtoTupleImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoStringIteratorImplementation) <= 64, "ProtoStringIteratorImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoStringImplementation) <= 64, "ProtoStringImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoByteBufferImplementation) <= 64, "ProtoByteBufferImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoMethodCell) <= 64, "ProtoMethodCell exceeds 64 bytes!");
    static_assert(sizeof(ProtoExternalPointerImplementation) <= 64, "ProtoExternalPointerImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoExternalBufferImplementation) <= 64, "ProtoExternalBufferImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoThreadImplementation) <= 64, "ProtoThreadImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoThreadExtension) <= 64, "ProtoThreadExtension exceeds 64 bytes!");
    static_assert(sizeof(LargeIntegerImplementation) <= 64, "LargeIntegerImplementation exceeds 64 bytes!");
    static_assert(sizeof(DoubleImplementation) <= 64, "DoubleImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoSetIteratorImplementation) <= 64, "ProtoSetIteratorImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoMultisetIteratorImplementation) <= 64, "ProtoMultisetIteratorImplementation exceeds 64 bytes!");
    static_assert(sizeof(TupleDictionary) <= 64, "TupleDictionary exceeds 64 bytes!");

    // UMD: internal implementation called by ProtoSpace::getImportModule
    const ProtoObject* getImportModuleImpl(ProtoSpace* space, ProtoContext* context, const char* logicalPath, const char* attrName2create);

    // String Interning Helpers
    void initStringInternMap(ProtoSpace* space);
    void freeStringInternMap(ProtoSpace* space);
    const ProtoStringImplementation* internString(ProtoContext* context, const ProtoStringImplementation* newString);

    struct DirtySegment {
        Cell* cellChain;
        DirtySegment* next;
    };
}

#endif //PROTO_INTERNAL_H
