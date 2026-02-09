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
        const ProtoListIteratorImplementation *listIteratorImplementation;
        const ProtoTupleImplementation *tupleImplementation;
        const ProtoStringImplementation *stringImplementation;
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
        /** Inline string: length (0..7) in low 3 bits; 7 chars at 7 bits each (bits 3..51). */
        struct {
            unsigned long pointer_tag: 6;
            unsigned long embedded_type: 4;
            unsigned long inline_len: 3;
            unsigned long inline_chars: 49;
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

#define EMBEDDED_TYPE_SMALLINT 0
#define EMBEDDED_TYPE_UNICODE_CHAR 2
#define EMBEDDED_TYPE_BOOLEAN 3
#define EMBEDDED_TYPE_INLINE_STRING 4

/** Inline string: up to 7 UTF-32 code units in 54 bits. Length in bits 0-2; chars in 7-bit each at bits 3+7*i (0..6). */
#define INLINE_STRING_MAX_LEN 7
#define INLINE_STRING_LEN_BITS 3
#define INLINE_STRING_CHAR_BITS 7

    bool isInlineString(const ProtoObject* o);
    unsigned long getProtoStringHash(ProtoContext* context, const ProtoObject* o);
    /** Builds inline string (no allocation). codepoints must be 0..127, len 0..7. */
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


    // The new, safe toImpl implementation (non-const)
    template<typename Impl, typename Api>
    inline Impl *toImpl(Api *ptr) {
        if (reinterpret_cast<const ProtoObject*>(ptr) == PROTO_NONE) {
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
        if (reinterpret_cast<const ProtoObject*>(ptr) == PROTO_NONE) {
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

        // Clear the tag bits (lower 6 bits) to get the raw pointer to the Cell
        uintptr_t raw_ptr_value = reinterpret_cast<uintptr_t>(p.oid) & ~0x3FUL;

        // Check for 64-byte alignment (lowest 6 bits should be 0)
        if ((raw_ptr_value & 0x3FUL) != 0) {
            std::cerr << "Error: toImpl conversion resulted in an unaligned pointer for type "
                      << getTypeName<const Impl>() << ". Raw pointer value: 0x" << std::hex << raw_ptr_value << std::dec
                      << ". Expected 64-byte alignment." << std::endl;
            std::abort();
        }

        return reinterpret_cast<const Impl *>(raw_ptr_value);
    }

    unsigned long generate_mutable_ref(ProtoContext* context);
    bool isInteger(const ProtoObject* obj);

    class Cell {
    public:
        std::atomic<uintptr_t> next_and_flags;

        explicit Cell(ProtoContext *context, Cell *n = nullptr);

        virtual ~Cell() = default;

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
            uintptr_t current = next_and_flags.load();
            uintptr_t flags = current & 0x3FUL;
            // Crucial: Mask 'n' to avoid leaking flags from the successor into 'this'
            next_and_flags.store((reinterpret_cast<uintptr_t>(n) & ~0x3FUL) | flags);
        }
        // Use this for uninitialized memory or to reset everything
        inline void internalSetNextRaw(Cell* n) {
            // Even in raw mode, we should ensure the pointer part is clean if it carries flags
            next_and_flags.store(reinterpret_cast<uintptr_t>(n) & ~0x3FUL);
        }
    };

    class ParentLinkImplementation : public Cell {
    public:
        const ParentLinkImplementation *parent;
        const ProtoObject *object;

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

    class ProtoStringImplementation : public Cell {
    public:
        const ProtoTupleImplementation *tuple;

        ProtoStringImplementation(ProtoContext *context, const ProtoTupleImplementation *tuple);
        ~ProtoStringImplementation() override = default;
        const ProtoObject *implAsObject(ProtoContext *context) const override;
        const ProtoString *asProtoString(ProtoContext *context) const;
        unsigned long getHash(ProtoContext *context) const override;
        const ProtoObject* implGetAt(ProtoContext* context, int index) const;
        unsigned long implGetSize(ProtoContext* context) const;
        const ProtoList* implAsList(ProtoContext* context) const;
        const ProtoStringImplementation* implAppendLast(ProtoContext* context, const ProtoString* otherString) const;
        void finalize(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        int implCompare(ProtoContext* context, const ProtoString* other) const;
        const ProtoStringIteratorImplementation* implGetIterator(ProtoContext* context) const;
    };

    class ProtoTupleImplementation : public Cell {
    public:
        const ProtoObject *slot[TUPLE_SIZE];
        unsigned long actual_size : 63; // The actual number of elements this node (or its children) represents

        ProtoTupleImplementation(ProtoContext *context, const ProtoObject **slot_values,
                                 unsigned long size);
        ~ProtoTupleImplementation() override = default;
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

    class ProtoSetImplementation : public Cell {
    public:
        const ProtoSparseList *list;
        unsigned long size;

        ProtoSetImplementation(ProtoContext *context, const ProtoSparseList *list, unsigned long size);

        const ProtoObject *implAsObject(ProtoContext *context) const override;

        const ProtoSet *asProtoSet(ProtoContext *context) const;
        void processReferences(ProtoContext *context, void *self, void (*method)(ProtoContext *, void *, const Cell *)) const override;
    };

    class ProtoMultisetImplementation : public Cell {
    public:
        const ProtoSparseList *list;
        unsigned long size;

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

    class ProtoThreadExtension : public Cell {
    public:
        std::thread* osThread;
        Cell* freeCells;
        AttributeCacheEntry* attributeCache;

        ProtoThreadExtension(ProtoContext* context);
        ~ProtoThreadExtension() override;
        void finalize(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        const ProtoObject *implAsObject(ProtoContext *context) const override;
    };

    class ProtoThreadImplementation : public Cell {
    public:
        ProtoContext *context;
        ProtoThreadExtension* extension;
        const ProtoString* name;
        ProtoSpace* space;
        const ProtoList* args;
        const ProtoSparseList* kwargs;

        ProtoThreadImplementation(ProtoContext *context, const ProtoString* name, ProtoSpace* space, ProtoMethod main, const ProtoList* args, const ProtoSparseList* kwargs);
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

    class ProtoListIteratorImplementation final : public Cell {
    public:
        const ProtoListImplementation *base;
        unsigned long currentIndex;

        ProtoListIteratorImplementation(ProtoContext *context, const ProtoListImplementation *b, unsigned long index);

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
        const ProtoObject* base;  /** String (inline or ProtoStringImplementation). */
        unsigned long currentIndex;
        ProtoStringIteratorImplementation(ProtoContext* context, const ProtoObject* stringObj, unsigned long i);
        ~ProtoStringIteratorImplementation() override = default;
        int implHasNext(ProtoContext* context) const;
        const ProtoObject* implNext(ProtoContext* context);
        const ProtoStringIteratorImplementation* implAdvance(ProtoContext* context) const;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        const ProtoStringIterator* asProtoStringIterator(ProtoContext* context) const;
        void finalize(ProtoContext* context) const;
        void processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const override;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoExternalPointerImplementation : public Cell {
    public:
        void* pointer;
        void (*finalizer)(void*);
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
    const ProtoObject* getImportModuleImpl(ProtoSpace* space, const char* logicalPath, const char* attrName2create);

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
