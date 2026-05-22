/*
 * protoCore
 *
 *  Created on: November, 2017 - Redesign January, 2024
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 */

#ifndef PROTO_H_
#define PROTO_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

namespace proto
{
    class SymbolTable;  // forward declaration for 64-shard interning table
    struct MutableValueCacheEntry;  // defined in proto_internal.h

    // Forward declarations
    class ProtoStringIterator;
    class ProtoTupleIterator;
    class Cell;
    class BigCell;
    class ProtoContext;
    class ProtoSpace;
    class ProtoRootSet;
    class DirtySegment;
    class ProtoObject;
    class TupleDictionary;
    class ProtoTuple;
    class ProtoString;
    class ProtoExternalPointer;
    class ProtoExternalBuffer;
    class ParentLink;
    class ProtoList;
    class ProtoListIterator;
    class ProtoSparseList;
    class ProtoSparseListImplementation;  // raw AVL impl, internal
    class ProtoSparseListIterator;
    class ProtoSet;
    class ProtoSetIterator;
    class ProtoMultiset;
    class ProtoMultisetIterator;
    class ProtoObjectCell;
    class ProtoByteBuffer;
    class ProtoThread;
    class ProtoSpaceImplementation;
    class ModuleProvider;
    class ProviderRegistry;

    //! Useful constants.
    //! @warning They should be kept in sync with proto_internal.h!
    #define PROTO_TRUE ((const proto::ProtoObject*)  1217UL) // Tag: EMBEDDED_VALUE (1), Type: BOOLEAN (3), Value: 1
    #define PROTO_FALSE ((const proto::ProtoObject*) 193UL)  // Tag: EMBEDDED_VALUE (1), Type: BOOLEAN (3), Value: 0
    #define PROTO_NONE ((const proto::ProtoObject*)  321UL)  // Tag: EMBEDDED_VALUE (1), Type: NONE (5), Value: 0

    typedef const ProtoObject*(*ProtoMethod)(
        ProtoContext* context,
        const ProtoObject* self,
        const ParentLink* parentLink,
        const ProtoList* positionalParameters,
        const ProtoSparseList* keywordParameters
    );

    class ProtoObject
    {
    public:
        //- Object Model
        const ProtoObject* getPrototype(ProtoContext* context) const;
        const ProtoObject* clone(ProtoContext* context, bool isMutable = false) const;
        const ProtoObject* newChild(ProtoContext* context, bool isMutable = false) const;

        //- Attributes
        const ProtoObject* getAttribute(ProtoContext* context, const ProtoString* name, bool callbacks = true) const;
        const ProtoObject* hasAttribute(ProtoContext* context, const ProtoString* name) const;
        const ProtoObject* hasOwnAttribute(ProtoContext* context, const ProtoString* name) const;
        const ProtoObject* setAttribute(ProtoContext* context, const ProtoString* name, const ProtoObject* value) const;
        /**
         * Remove an own-attribute from the object.  Mirrors `setAttribute`'s
         * mutable/immutable contract:
         *
         *   - **Immutable** receivers return a new ProtoObject* whose
         *     attribute table no longer carries `name`.  The original is
         *     untouched.
         *   - **Mutable** receivers update the shard root in place via the
         *     same CAS loop used by `setAttribute` and return `this`.
         *
         * If `name` is not present as an OWN attribute (the chain may still
         * resolve it from a parent), the call is a no-op and returns `this`
         * without allocating.  Use `hasOwnAttribute` first if you need to
         * know whether the receiver carried the name.
         *
         * Removal is at the OWN level only — parent attributes are NEVER
         * affected, so `del child.x` on an instance whose class still
         * defines `x` leaves the class binding intact (CPython semantics).
         */
        const ProtoObject* removeAttribute(ProtoContext* context, const ProtoString* name) const;
        const ProtoSparseList* getAttributes(ProtoContext* context) const;
        const ProtoSparseList* getOwnAttributes(ProtoContext* context) const;
        /**
         * Returns the value of an own-attribute by interned symbol pointer key, or nullptr if not
         * found. Resolves mutable state once internally. Does NOT traverse the prototype chain and
         * does NOT invoke descriptor protocol — use only for plain instance own-attribute reads
         * where the caller guarantees name is a POINTER_TAG_SYMBOL (e.g. co_names entries).
         */
        const ProtoObject* getOwnAttributeDirect(ProtoContext* context, const ProtoString* name) const;

        //- Inheritance
        const ProtoList* getParents(ProtoContext* context) const;
        /**
         * @brief Returns the immediate (first) parent without allocating a list.
         *
         * Equivalent to `getParents(ctx)->getAt(ctx, 0)` but without the
         * ProtoList allocation per call, and correctly resolves mutable
         * objects to their current snapshot (which `getPrototype` does not).
         *
         * Returns `PROTO_NONE` when the object has no parent.  This is the
         * recommended hot-path API for embedders walking single-inheritance
         * chains (e.g. `type(obj)` lookups in protoPython, where allocating
         * a ProtoList per attribute access dominates wall time).
         */
        const ProtoObject* getFirstParent(ProtoContext* context) const;
        int hasParent(ProtoContext* context, const ProtoObject* target) const;
        const ProtoObject* addParent(ProtoContext* context, const ProtoObject* newParent) const;
        const ProtoObject* addParentInternal(ProtoContext* context, const ProtoObject* newParent) const;
        /**
         * @brief Replace the entire parent chain with `newParents`.
         *
         * Use this when an embedder needs to mutate the prototype
         * chain wholesale — e.g. when a user-language `__bases__`
         * reassignment must drop the old bases entirely instead of
         * just appending new ones.
         *
         * - For an immutable object, returns a freshly-built handle
         *   sharing the same attributes but with the rebuilt parent
         *   chain.  The original handle becomes stale.
         * - For a mutable object, updates the per-shard mutable state
         *   in place via a CAS loop and returns the SAME handle
         *   (mirroring `addParent` and `setAttribute`).
         *
         * The list is interpreted with index 0 as the immediate
         * (first) parent, matching `getParents()`'s output order.
         * Passing an empty or null list clears the parent chain
         * entirely.
         */
        const ProtoObject* setParents(ProtoContext* context, const ProtoList* newParents) const;
        const ProtoObject* isInstanceOf(ProtoContext* context, const ProtoObject* prototype) const;

        //- Execution
        const ProtoObject* call(ProtoContext* context,
                                const ParentLink* nextParent,
                                const ProtoString* method,
                                const ProtoObject* self,
                                const ProtoList* positionalParameters,
                                const ProtoSparseList* keywordParametersDict = nullptr) const;
        
        const ProtoObject* divmod(ProtoContext* context, const ProtoObject* other) const;

        //- Internals & Type Checking
        unsigned long getHash(ProtoContext* context) const;
        int isCell(ProtoContext* context) const;
        const Cell* asCell(ProtoContext* context) const;
        static bool isCellPointer(const ProtoObject* obj);
        static const Cell* asCellPointer(const ProtoObject* obj);

        bool isBoolean(ProtoContext* context) const;
        bool isInteger(ProtoContext* context) const;
        bool isFloat(ProtoContext* context) const;
        bool isByte(ProtoContext* context) const;
        bool isDate(ProtoContext* context) const;
        bool isTimestamp(ProtoContext* context) const;
        bool isTimeDelta(ProtoContext* context) const;
        // P6 — tag-only fast paths.  These inline a single 6-bit tag-low
        // bitwise check for the dominant case where the receiver is a
        // direct primitive (tag 6 = string, 22 = symbol, 1 = embedded
        // value such as inline string / small int).  When the receiver
        // is a wrapper object (tag 0) that delegates type identity via
        // `__data__`, the slow virtual variant is invoked instead.
        // Hot callers (interpreter dispatchers, getAttribute auto-intern
        // probes, hash dispatch in protoPython / protoJS) avoid the
        // function-call overhead entirely on the common path.
        bool isMethod(ProtoContext* context) const;
        bool isNone(ProtoContext* context) const;
        bool isString(ProtoContext* context) const;
        // Tag-only fast variant: returns true if the pointer encodes a
        // string-typed value directly (POINTER_TAG_STRING = 6,
        // POINTER_TAG_SYMBOL = 22, or EMBEDDED_VALUE with
        // EMBEDDED_TYPE_INLINE_STRING = 4 in bits 6..9).  Returns false
        // for wrapper objects (tag 0); callers that need the full
        // protocol must call isString().
        static inline bool isStringTagFast(const ProtoObject* o) noexcept {
            if (!o) return false;
            uintptr_t p = reinterpret_cast<uintptr_t>(o);
            unsigned tag = static_cast<unsigned>(p & 0x3F);
            if (tag == 6 || tag == 22) return true;
            if (tag == 1) {
                unsigned emb = static_cast<unsigned>((p >> 6) & 0xF);
                if (emb == 4) return true;
            }
            return false;
        }
        bool isDouble(ProtoContext* context) const;
        bool isTuple(ProtoContext* context) const;
        bool isSet(ProtoContext* context) const;
        bool isMultiset(ProtoContext* context) const;
        bool isByteBuffer(ProtoContext* context) const;
        bool isNativeRangeIterator(ProtoContext* context) const;

        //- Type Coercion
        bool asBoolean(ProtoContext* context) const;
        long long asLong(ProtoContext* context) const;
        /**
         * @brief Bignum-safe sign of an integer object.
         *
         * Returns -1 for negative integers, 0 for zero, +1 for positive.
         * Works for both SmallInteger (tagged) and LargeInteger (heap-allocated)
         * objects. Throws std::runtime_error if the receiver is not an integer.
         *
         * Public replacement for the previously private proto::Integer::sign.
         */
        int integerSign(ProtoContext* context) const;
        /**
         * @brief Bignum-safe integer-to-string conversion.
         *
         * Returns a ProtoString containing the receiver's integer value
         * rendered in the given base (2..36). Works for SmallInteger and
         * LargeInteger objects. Throws std::invalid_argument for an
         * out-of-range base, std::runtime_error if the receiver is not
         * an integer.
         *
         * Public replacement for the previously private
         * proto::Integer::toString.
         */
        const ProtoString* asIntegerString(ProtoContext* context, int base = 10) const;
        double asDouble(ProtoContext* context) const;
        char asByte(ProtoContext* context) const;
        void asDate(ProtoContext* context, unsigned int& year, unsigned& month, unsigned& day) const;
        unsigned long asTimestamp(ProtoContext* context) const;
        long asTimeDelta(ProtoContext* context) const;
        const ProtoList* asList(ProtoContext* context) const;
        const ProtoListIterator* asListIterator(ProtoContext* context) const;
        const ProtoTuple* asTuple(ProtoContext* context) const;
        const ProtoTupleIterator* asTupleIterator(ProtoContext* context) const;
        const ProtoString* asString(ProtoContext* context) const;
        const ProtoStringIterator* asStringIterator(ProtoContext* context) const;
        const ProtoSparseList* asSparseList(ProtoContext* context) const;
        const ProtoSparseListIterator* asSparseListIterator(ProtoContext* context) const;
        const ProtoSet* asSet(ProtoContext* context) const;
        const ProtoSetIterator* asSetIterator(ProtoContext* context) const;
        const ProtoMultiset* asMultiset(ProtoContext* context) const;
        const ProtoMultisetIterator* asMultisetIterator(ProtoContext* context) const;
        const ProtoThread* asThread(ProtoContext* context) const;
        const ProtoExternalPointer* asExternalPointer(ProtoContext* context) const;
        const ProtoExternalBuffer* asExternalBuffer(ProtoContext* context) const;
        const ProtoByteBuffer* asByteBuffer(ProtoContext* context) const;
        const ProtoObject* nextInNativeRange(ProtoContext* context) const;
        /**
         * If this object is a ByteBuffer, returns its raw data pointer; otherwise nullptr.
         * Avoids three separate cross-DSO calls (isByteBuffer + asByteBuffer + getBuffer)
         * in hot paths such as FunctionMetaCache and native bytecode access.
         */
        char* getDataIfByteBuffer(ProtoContext* context) const;
        /** If this object is a ProtoExternalBuffer, returns the raw segment pointer; otherwise nullptr. Stable until the object is collected (no compaction). */
        void* getRawPointerIfExternalBuffer(ProtoContext* context) const;
        ProtoMethod asMethod(ProtoContext* context) const;
        const ProtoObject* asMethodSelf(ProtoContext* context) const;

        //- Comparison
        int compare(ProtoContext* context, const ProtoObject* other) const;

        //- Unary Operations
        const ProtoObject* negate(ProtoContext* context) const;
        const ProtoObject* abs(ProtoContext* context) const;

        //- Arithmetic Operations
        const ProtoObject* add(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* subtract(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* multiply(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* divide(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* modulo(ProtoContext* context, const ProtoObject* other) const;

        //- Bitwise Operations
        const ProtoObject* bitwiseAnd(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* bitwiseOr(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* bitwiseXor(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* bitwiseNot(ProtoContext* context) const;
        const ProtoObject* shiftLeft(ProtoContext* context, int amount) const;
        const ProtoObject* shiftRight(ProtoContext* context, int amount) const;
    };

    // ------------------------------------------------------------------
    // Public SmallInt fast-path helpers
    // ------------------------------------------------------------------
    //
    // SmallInt tagged-pointer encoding (the same `si` bit-field layout
    // proto_internal.h declares for `ProtoObjectPointer`):
    //
    //   bits  0-5   :  pointer_tag  = 1  (POINTER_TAG_EMBEDDED_VALUE)
    //   bits  6-9   :  embedded_type = 0 (EMBEDDED_TYPE_SMALLINT)
    //   bits 10-63  :  signed 54-bit integer value (sign-extended on read)
    //
    // The combined low-10-bit tag is therefore the constant value 1.
    //
    // These helpers are `inline` in the public header so an embedder
    // (e.g. protoPython's bytecode dispatcher) can branch on the tag,
    // do the integer ALU op, and re-pack the result without crossing
    // the protoCore shared-library boundary.  protoCore stays a separate
    // shared library — only the bit pattern is exposed, not internal
    // types or symbols.
    //
    // Range is [-(2^53), (2^53) - 1]; values outside that fall through
    // to the slow path (`ProtoObject::add`, etc.) which promotes to a
    // LargeInteger.

    static constexpr long long PROTO_SMALL_INT_MAX  = (1LL << 53) - 1;
    static constexpr long long PROTO_SMALL_INT_MIN  = -(1LL << 53);
    static constexpr unsigned long PROTO_SMALL_INT_TAG_MASK  = 0x3FFUL; // pointer_tag(6) + embedded_type(4)
    static constexpr unsigned long PROTO_SMALL_INT_TAG_VALUE = 0x001UL; // POINTER_TAG_EMBEDDED_VALUE | (EMBEDDED_TYPE_SMALLINT << 6)

    /** True iff `obj` is a tagged SmallInteger pointer (no Cell, no allocation). */
    static inline bool isSmallInt(const ProtoObject* obj) {
        return (reinterpret_cast<unsigned long>(obj) & PROTO_SMALL_INT_TAG_MASK) == PROTO_SMALL_INT_TAG_VALUE;
    }

    /** Extract the signed 54-bit integer value from a SmallInt-tagged pointer.
     *  Caller must have validated with isSmallInt(). */
    static inline long long asSmallInt(const ProtoObject* obj) {
        // Arithmetic right shift on a signed 64-bit int sign-extends bit 63
        // into the upper 10 bits, recovering the original 54-bit signed value.
        return static_cast<long long>(reinterpret_cast<long long>(obj)) >> 10;
    }

    /** True iff `v` fits in a SmallInt (range [-(2^53), (2^53)-1]). */
    static inline bool smallIntInRange(long long v) {
        return v >= PROTO_SMALL_INT_MIN && v <= PROTO_SMALL_INT_MAX;
    }

    /** Build a SmallInt-tagged pointer from a value already known to be in range. */
    static inline const ProtoObject* makeSmallInt(long long v) {
        return reinterpret_cast<const ProtoObject*>((v << 10) | static_cast<long long>(PROTO_SMALL_INT_TAG_VALUE));
    }

    class ProtoListIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* next(ProtoContext* context) const;
        const ProtoListIterator* advance(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoList
    {
    public:
        //- Accessors
        const ProtoObject* getAt(ProtoContext* context, int index) const;
        const ProtoObject* getFirst(ProtoContext* context) const;
        const ProtoObject* getLast(ProtoContext* context) const;
        const ProtoList* getSlice(ProtoContext* context, int from, int to) const;
        unsigned long getSize(ProtoContext* context) const;
        bool has(ProtoContext* context, const ProtoObject* value) const;

        //- Modifiers that return a new list
        const ProtoList* setAt(ProtoContext* context, int index, const ProtoObject* value) const;
        const ProtoList* insertAt(ProtoContext* context, int index, const ProtoObject* value) const;
        const ProtoList* appendFirst(ProtoContext* context, const ProtoObject* value) const;
        const ProtoList* appendLast(ProtoContext* context, const ProtoObject* value) const;
        const ProtoList* extend(ProtoContext* context, const ProtoList* other) const;
        const ProtoList* splitFirst(ProtoContext* context, int index) const;
        const ProtoList* splitLast(ProtoContext* context, int index) const;
        const ProtoList* removeFirst(ProtoContext* context) const;
        const ProtoList* removeLast(ProtoContext* context) const;
        const ProtoList* removeAt(ProtoContext* context, int index) const;
        const ProtoList* removeSlice(ProtoContext* context, int from, int to) const;
        const ProtoList* multiply(ProtoContext* context, const ProtoObject* count) const;

        //- Conversion
        const ProtoObject* asObject(ProtoContext* context) const;
        const ProtoListIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoTupleIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* next(ProtoContext* context);
        const ProtoTupleIterator* advance(ProtoContext* context);
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoTuple
    {
    public:
        //- Accessors
        const ProtoObject* getAt(ProtoContext* context, int index) const;
        const ProtoObject* getFirst(ProtoContext* context) const;
        const ProtoObject* getLast(ProtoContext* context) const;
        const ProtoObject* getSlice(ProtoContext* context, int from, int to) const;
        unsigned long getSize(ProtoContext* context) const;
        bool has(ProtoContext* context, const ProtoObject* value) const;

        //- "Modifiers" (return new tuples)
        const ProtoObject* setAt(ProtoContext* context, int index, const ProtoObject* value) const;
        const ProtoObject* insertAt(ProtoContext* context, int index, const ProtoObject* value) const;
        const ProtoObject* appendFirst(ProtoContext* context, const ProtoTuple* otherTuple) const;
        const ProtoObject* appendLast(ProtoContext* context, const ProtoTuple* otherTuple) const;
        const ProtoObject* splitFirst(ProtoContext* context, int count) const;
        const ProtoObject* splitLast(ProtoContext* context, int count) const;
        const ProtoObject* removeFirst(ProtoContext* context, int count) const;
        const ProtoObject* removeLast(ProtoContext* context, int count) const;
        const ProtoObject* removeAt(ProtoContext* context, int index) const;
        const ProtoObject* removeSlice(ProtoContext* context, int from, int to) const;

        //- Conversion
        const ProtoList* asList(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        const ProtoTupleIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoStringIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* next(ProtoContext* context);
        const ProtoStringIterator* advance(ProtoContext* context);
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoString
    {
    public:
        static const ProtoString* create(ProtoContext* context, const ProtoList* list);

        // @deprecated Use fromUTF8() instead
        [[deprecated("Use fromUTF8 instead")]]
        static const ProtoString* fromUTF8String(ProtoContext* context,
                                                  const char* zeroTerminatedUtf8String);

        /** Creates a ProtoString from a zero-terminated UTF-8 C string. */
        static const ProtoString* fromUTF8(ProtoContext* context, const char* zeroTerminatedUtf8);

        /** Creates a ProtoString from a std::string (UTF-8 encoded). */
        static const ProtoString* fromStdString(ProtoContext* context, const std::string& s);

        /**
         * Decodes a chunk of raw UTF-8 bytes, handling incomplete multi-byte sequences
         * that span buffer boundaries.
         *
         * @param context       The current execution context.
         * @param buf           Pointer to the incoming byte buffer.
         * @param len           Number of bytes in \a buf.
         * @param pending       Bytes saved from the previous call that form the start of an
         *                      incomplete sequence, or nullptr if none.
         * @param pending_count Number of bytes in \a pending (0..3).
         * @param out_remainder Output buffer (minimum 4 bytes) that receives any trailing
         *                      incomplete sequence from \a buf that was not yet decoded.
         * @param out_remainder_count Number of bytes written to \a out_remainder.
         * @return The decoded ProtoString for the complete codepoints in this chunk.
         */
        static const ProtoString* fromUTF8Buffer(ProtoContext* context,
                                                  const uint8_t* buf, size_t len,
                                                  const uint8_t* pending,
                                                  uint8_t pending_count,
                                                  uint8_t* out_remainder,
                                                  uint8_t* out_remainder_count);

        /** Builds a ProtoString from a ProtoTuple of Unicode codepoint objects. */
        static const ProtoString* fromCodepointTuple(ProtoContext* context,
                                                      const ProtoTuple* tuple);

        /** Creates or retrieves an interned symbol for the given UTF-8 string. Symbols with the same
         *  content share a unique pointer identity. Strong symbols (created here) are never GC-collected. */
        static const ProtoString* createSymbol(ProtoContext* context, const char* zeroTerminatedUtf8);
        static const ProtoString* createSymbol(ProtoContext* context, const std::string& s);

        int cmp_to_string(ProtoContext* context, const ProtoString* otherString) const;

        /**
         * @brief True iff this ProtoString* points at an interned canonical
         * symbol (POINTER_TAG_SYMBOL).
         *
         * Strings created via createSymbol(), or auto-interned by
         * ProtoObject::setAttribute, are symbols.  Strings produced by
         * fromUTF8String / appendLast / setAt etc. are not.
         *
         * Equality and lookup against symbols is just pointer comparison
         * — embedders that handle attribute-name-style keys can short-
         * circuit content comparison when both sides are symbols.
         */
        bool isSymbol() const;

        //- Accessors
        const ProtoObject* getAt(ProtoContext* context, int index) const;
        unsigned long getSize(ProtoContext* context) const;
        const ProtoString* getSlice(ProtoContext* context, int from, int to) const;

        //- "Modifiers" (return new strings)
        const ProtoString* setAt(ProtoContext* context, int index, const ProtoObject* character) const;
        const ProtoString* insertAt(ProtoContext* context, int index, const ProtoObject* character) const;
        const ProtoString* setAtString(ProtoContext* context, int index, const ProtoString* otherString) const;
        const ProtoString* insertAtString(ProtoContext* context, int index, const ProtoString* otherString) const;
        const ProtoString* appendFirst(ProtoContext* context, const ProtoString* otherString) const;
        const ProtoString* appendLast(ProtoContext* context, const ProtoString* otherString) const;
        const ProtoString* splitFirst(ProtoContext* context, int count) const;
        const ProtoString* splitLast(ProtoContext* context, int count) const;
        const ProtoString* removeFirst(ProtoContext* context, int count) const;
        const ProtoString* removeLast(ProtoContext* context, int count) const;
        const ProtoString* removeAt(ProtoContext* context, int index) const;
        const ProtoString* removeSlice(ProtoContext* context, int from, int to) const;
        const ProtoString* multiply(ProtoContext* context, const ProtoObject* count) const;
        const ProtoObject* modulo(ProtoContext* context, const ProtoObject* other) const;

        //- Conversion
        const ProtoObject* asObject(ProtoContext* context) const;
        const ProtoList* asList(ProtoContext* context) const;
        const ProtoStringIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
        const ProtoString* isCell(ProtoContext* context) const;
        const Cell* asCell(ProtoContext* context) const;

        /** Appends the UTF-8 representation of this string to \a out. */
        void toUTF8String(ProtoContext* context, std::string& out) const;

        /** Returns the UTF-8 content of this string as a std::string. */
        std::string toStdString(ProtoContext* context) const;
    };

    //! ProtoExternalPointer - Represents a wrapper for external C++ pointers
    class ProtoExternalPointer
    {
    public:
        /**
         * @brief Extracts the wrapped C++ pointer from this ProtoExternalPointer.
         * @param context The current execution context.
         * @return The wrapped void* pointer, or nullptr if invalid.
         */
        void* getPointer(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    /** Contiguous buffer (aligned_alloc). Lifecycle tied to descriptor; GC finalize frees segment (Shadow GC). */
    class ProtoExternalBuffer
    {
    public:
        void* getRawPointer(ProtoContext* context) const;
        unsigned long getSize(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    /** Abstract base for module providers. Resolution chain entries "provider:alias" or "provider:GUID" delegate to a registered provider. */
    class ModuleProvider
    {
    public:
        virtual ~ModuleProvider() = default;
        /** Attempt to load a module for \a logicalPath. Return the module object or PROTO_NONE. */
        virtual const ProtoObject* tryLoad(const std::string& logicalPath, ProtoContext* ctx) = 0;
        /** Obligatory unique identifier. */
        virtual const std::string& getGUID() const = 0;
        /** Optional alias (e.g. "odoo_db"). Lookup by alias takes precedence over GUID. */
        virtual const std::string& getAlias() const = 0;
    };

    /** Singleton registry of ModuleProviders. Lookup by alias takes precedence over GUID. */
    class ProviderRegistry
    {
    public:
        static ProviderRegistry& instance();
        ~ProviderRegistry();
        void registerProvider(std::unique_ptr<ModuleProvider> provider);
        ModuleProvider* findByAlias(const std::string& alias);
        ModuleProvider* findByGUID(const std::string& guid);
        /** Given "provider:alias" or "provider:GUID", return the provider (alias tried first). Returns nullptr if not found or format invalid. */
        ModuleProvider* getProviderForSpec(const std::string& spec);
        ProviderRegistry(const ProviderRegistry&) = delete;
        ProviderRegistry& operator=(const ProviderRegistry&) = delete;
    private:
        ProviderRegistry();
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    class ProtoSparseListIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        unsigned long nextKey(ProtoContext* context) const;
        const ProtoObject* nextValue(ProtoContext* context) const;
        const ProtoSparseListIterator* advance(ProtoContext* context);
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoSparseList
    {
    public:
        bool has(ProtoContext* context, unsigned long index) const;
        const ProtoObject* getAt(ProtoContext* context, unsigned long index) const;
        const ProtoSparseList* setAt(ProtoContext* context, unsigned long index, const ProtoObject* value) const;
        const ProtoSparseList* removeAt(ProtoContext* context, unsigned long index) const;
        bool isEqual(ProtoContext* context, const ProtoSparseList* otherDict) const;
        unsigned long getSize(ProtoContext* context) const;

        const ProtoObject* asObject(ProtoContext* context) const;
        const ProtoSparseListIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;

        void processElements(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, unsigned long, const ProtoObject*)) const;
        void processValues(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const ProtoObject*)) const;
    };

    class ProtoSetIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* next(ProtoContext* context) const;
        const ProtoSetIterator* advance(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    /**
     * @class ProtoSet
     * @brief An immutable collection of unique objects.
     */
    class ProtoSet
    {
    public:
        /**
         * @brief Returns a new set with the given value added.
         */
        const ProtoSet* add(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns PROTO_TRUE if the value is in the set, otherwise PROTO_FALSE.
         */
        const ProtoObject* has(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns a new set with the given value removed.
         */
        const ProtoSet* remove(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns the number of unique elements in the set.
         */
        unsigned long getSize(ProtoContext* context) const;

        /**
         * @brief Returns the set as a generic ProtoObject.
         */
        const ProtoObject* asObject(ProtoContext* context) const;

        /**
         * @brief Returns an iterator for the set.
         */
        const ProtoSetIterator* getIterator(ProtoContext* context) const;
    };

    class ProtoMultisetIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* next(ProtoContext* context) const;
        const ProtoMultisetIterator* advance(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    /**
     * @class ProtoMultiset
     * @brief An immutable collection that can store multiple occurrences of the same object.
     */
    class ProtoMultiset
    {
    public:
        /**
         * @brief Returns a new multiset with the given value added.
         */
        const ProtoMultiset* add(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns the number of times the given value appears in the multiset.
         */
        const ProtoObject* count(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns a new multiset with one occurrence of the given value removed.
         */
        const ProtoMultiset* remove(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns the total number of elements in the multiset (including duplicates).
         */
        unsigned long getSize(ProtoContext* context) const;

        /**
         * @brief Returns the multiset as a generic ProtoObject.
         */
        const ProtoObject* asObject(ProtoContext* context) const;

        /**
         * @brief Returns an iterator for the multiset.
         */
        const ProtoMultisetIterator* getIterator(ProtoContext* context) const;
    };

    class ProtoByteBuffer
    {
    public:
        unsigned long getSize(ProtoContext* context) const;
        char* getBuffer(ProtoContext* context) const;
        char getAt(ProtoContext* context, int index) const;
        void setAt(ProtoContext* context, int index, char value);
        const ProtoObject* asObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoThread
    {
    public:
        static const ProtoThread* getCurrentThread(ProtoContext* context);

        void detach(ProtoContext* context);
        void join(ProtoContext* context);
        void exit(ProtoContext* context);

        const ProtoObject* getName(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;

        void setCurrentContext(ProtoContext* context);
        /** Returns the current execution context for this thread. O(1) thread-local read. */
        ProtoContext* getCurrentContext() const;
        void setManaged();
        void setUnmanaged();
        void synchToGC();
    };

    /**
     * @brief Represents the execution context for a thread, managing the call stack,
     * local variables, and object creation.
     */
    class ProtoContext
    {
    private:
        // C-style array for automatic variables that are destroyed with the context.
        const ProtoObject** automaticLocals;
        unsigned int automaticLocalsCount;
        // False when automaticLocals points to an externally-owned buffer (e.g. stack SBO).
        // The destructor only calls delete[] when this is true.
        bool ownsSlots_;

    public:
        /**
         * @brief Constructs a new execution context for a function call.
         * This constructor is the core of the function execution model. It allocates
         * space for local variables and performs argument-to-parameter binding.
         *
         * @param space The global ProtoSpace this context belongs to.
         * @param previous The parent context in the call stack.
         * @param parameterNames A list of ProtoStrings for the function's declared parameter names.
         * @param localNames A list of ProtoStrings for the function's automatic (C-style) local variables.
         * @param args The positional arguments passed to the function.
         * @param kwargs The keyword arguments passed to the function.
         * @param totalSlots Minimum number of slots to allocate for local variables.
         * @param externalSlots Optional pre-allocated slot buffer (caller-owned, must stay alive
         *        for the lifetime of this context). When non-null, no heap allocation is done for
         *        slots; the caller is responsible for initialising the buffer to PROTO_NONE.
         */
        explicit ProtoContext(
            ProtoSpace* space,
            ProtoContext* previous = nullptr,
            const ProtoList* parameterNames = nullptr,
            const ProtoList* localNames = nullptr,
            const ProtoList* args = nullptr,
            const ProtoSparseList* kwargs = nullptr,
            size_t totalSlots = 0,
            const ProtoObject** externalSlots = nullptr
        );
        ~ProtoContext();

        //- Execution State
        ProtoContext* previous;
        ProtoSpace* space;
        ProtoThread* thread;
        char* currentFileName;
        int currentLineNumber;
        
        // Variables that can be captured by closures, managed by the GC.
        const ProtoSparseList* closureLocals;

        //- Return Value
        const ProtoObject* returnValue;

        //- GC Interface
        /**
         * @brief Provides the GC with access to the automatic local variables.
         * @return A pointer to the array of automatic local variable objects.
         */
        inline const ProtoObject** getAutomaticLocals() const { return automaticLocals; }

        /**
         * @brief Provides the GC with the count of automatic local variables.
         * @return The number of automatic local variables.
         */
        inline unsigned int getAutomaticLocalsCount() const { return automaticLocalsCount; }

        /**
         * @brief Read one automatic-local slot (combined bounds-check + read).
         *
         * Preferred over getAutomaticLocals()[idx] at call sites: single
         * expression, bounds-safe, and keeps the slot access within the
         * protoCore API boundary.  Returns nullptr when idx is out of range.
         */
        inline const ProtoObject* getAutomaticLocal(unsigned int idx) const {
            return (idx < automaticLocalsCount && automaticLocals) ? automaticLocals[idx] : nullptr;
        }

        /**
         * @brief Write one automatic-local slot (combined bounds-check + write).
         *
         * Returns true on success, false if idx is out of range.
         */
        inline bool setAutomaticLocal(unsigned int idx, const ProtoObject* val) {
            if (idx < automaticLocalsCount && automaticLocals) {
                automaticLocals[idx] = val;
                return true;
            }
            return false;
        }

        /**
         * @brief Resize the automatic-locals slot region.
         *
         * Designed for embedders (e.g. protoJS's bytecode interpreter)
         * that don't know the required slot count at construction time
         * but want a flat, GC-visible array — instead of the more
         * expensive ProtoSparseList copy-on-write storage — for the
         * call frame's locals + value stack.
         *
         * If `newCount > automaticLocalsCount`, allocates a fresh
         * heap-owned `const ProtoObject*[newCount]` initialised to
         * PROTO_NONE, copies any existing entries, frees the previous
         * heap buffer (only when `ownsSlots_`), and reseats the
         * pointer.  After the call `automaticLocals` is guaranteed to
         * be heap-owned (`ownsSlots_=true`) so the destructor will
         * release it.
         *
         * If `newCount <= automaticLocalsCount`, the call is a no-op
         * (we never shrink — keeps the existing GC roots reachable).
         */
        void resizeAutomaticLocals(unsigned int newCount);

        //- Factory methods for primitive types.
        const ProtoObject* fromInteger(long long value);
        const ProtoObject* fromLong(long long value);
        const ProtoObject* fromString(const char* str, int base = 10);
        const ProtoObject* fromDouble(double value);
        const ProtoObject* fromUnicodeChar(unsigned int unicodeChar);
        const ProtoObject* fromUTF8String(const char* zeroTerminatedUtf8String);
        const ProtoObject* fromMethod(ProtoObject* self, ProtoMethod method);
        const ProtoObject* fromExternalPointer(void* pointer, void (*finalizer)(void*) = nullptr);
        const ProtoObject* fromBuffer(unsigned long length, char* buffer, bool freeOnExit = false);
        const ProtoObject* newBuffer(unsigned long length);
        const ProtoObject* fromBoolean(bool value);
        const ProtoObject* fromByte(char c);
        const ProtoObject* fromDate(unsigned year, unsigned month, unsigned day);
        const ProtoObject* fromTimestamp(unsigned long timestamp);
        const ProtoObject* fromTimeDelta(long timedelta);

        //- Factory methods for complex types
        // Empty list — returns the inline-storage form (POINTER_TAG_LIST_SMALL,
        // size 0).  Subsequent appendLast calls stay in the inline form for
        // sizes 1..5 and only graduate to the AVL form on overflow.
        const ProtoList* newList();
        // Bulk-construct a list from a count + contiguous source array in
        // a SINGLE cell allocation when n ≤ 5; for n > 5 it falls back to
        // the AVL builder.  Use this whenever the count is known at the call
        // site (interpreter call dispatch, native method argument packs);
        // the result is functionally identical to newList() + N appendLast
        // but costs exactly 1 cell instead of 1 + N.
        const ProtoList* newList(unsigned n, const ProtoObject* const* items);
        const ProtoTuple* newTuple();
        const ProtoTuple* newTuple(const std::vector<const ProtoObject*>& elements);
        const ProtoTuple* newTupleFromList(const ProtoList* sourceList);
        const ProtoSparseList* newSparseList();
        // Returns an empty AVL-form sparse list implementation as a raw
        // C++ pointer. Used for internal struct fields that should not
        // carry a tag (e.g. ProtoObjectCell::attributes); the public
        // newSparseList() above returns a Small-form public-API handle
        // for callers that want the inline-3 optimisation (closures,
        // Set/Multiset backing).
        const ProtoSparseListImplementation* newSparseListImpl();
        const ProtoSet* newSet();
        /** Creates a native range iterator over [start, stop) with the given step. */
        const ProtoObject* newRangeIterator(long long start, long long stop, long long step);
        const ProtoMultiset* newMultiset();
        const ProtoObject* newObject(bool mutableObject = false);
        /** Allocates a contiguous buffer (aligned_alloc). GC finalize frees it when descriptor is collected (Shadow GC). */
        const ProtoObject* newExternalBuffer(unsigned long size);
        /**
         * Create a fresh, GC-owned ProtoByteBuffer holding `len` raw octets.
         * The bytes are copied from `data` (data may be null only if len == 0).
         * Unlike ProtoString, ProtoByteBuffer is opaque-binary: every byte
         * 0..255 round-trips, and embedded nulls do not truncate.
         */
        const ProtoByteBuffer* newByteBuffer(const char* data, unsigned long len);

        //- Memory Management
        Cell* allocCell();
        void addCell2Context(Cell* cell);

        /**
         * @brief Cooperative GC safepoint.
         *
         * Public hook callable from any thread.  When the space's stop-the-world
         * flag is set, the calling thread parks itself on the STW condition
         * variable until the GC pause ends, the same handshake `allocCell`
         * already performs internally every 64 allocations.
         *
         * This is the supported way for an embedder (e.g. protoPython's
         * bytecode dispatch loop) to participate in the GC handshake from a
         * tight, allocation-free hot loop.  Without it, a CPU-bound thread
         * that never calls `allocCell` will starve the GC indefinitely and
         * stall every other thread waiting for STW to begin.
         *
         * Cheap on the fast path: a single relaxed atomic load of
         * `stwFlag`.  Only takes the global mutex if the flag is set.
         */
        void safepoint();

        /**
         * @brief Heap-ceiling checkpoint, run at every outermost critical
         *        section boundary (see CriticalSection).
         *
         * When a hard heap limit is configured (ProtoSpace::setHeapLimits)
         * and the heap has reached its ceiling, this blocks the calling
         * thread until the GC reclaims enough cells to proceed — or, if the
         * live set itself meets the ceiling across two cycles, escalates to
         * the out-of-memory handling.  It is a no-op when no limit is set.
         *
         * It MUST run at criticalSectionDepth == 0: the thread holds no
         * half-built tree there, so it can leave the running set and let the
         * GC run.  Blocking inside a critical section would instead let a
         * stop-the-world cycle reclaim a helper's in-flight, not-yet-anchored
         * cells.  See ProtoSpace::waitForHeapHeadroom.
         */
        void heapLimitCheckpoint();

        Cell* lastAllocatedCell;
        unsigned long allocatedCellsCount;
        Cell* freeCells;

        // Cached pointer to this thread's MutableValueCacheEntry array
        // (stashed at context construction to short-circuit the
        // `toImpl(thread) → extension → mutableValueCache` indirection
        // chain that resolveMutableState would otherwise pay on every
        // mutable read).  See protoCore/core/ProtoObject.cpp's
        // resolveMutableState for the validation/refresh discipline.
        // nullptr when the thread has no extension yet (e.g. early
        // bootstrap, off-thread alloc with NULL context).
        MutableValueCacheEntry* mutableValueCache_;
        Cell* pendingRoot;
        std::atomic_flag lock{ATOMIC_FLAG_INIT};

        /**
         * @brief Depth counter for in-progress GC critical sections on this
         *        thread (e.g. mutable setAttribute mid-construction).
         *
         * While > 0, the cooperative STW poll in allocCell()/safepoint() skips
         * parking — the GC's stop-the-world phase therefore cannot start
         * until every running thread either finishes its critical section
         * or reaches a safepoint outside one.  This protects the construct +
         * CAS-into-root pattern from being interrupted between the
         * allocation of intermediate cells and the final atomic publish:
         * without the guard a sweep would see those cells in dirtySegments
         * (chain submitted at the per-context threshold) and unreachable
         * from any GC root, and free them under the running mutator.
         *
         * Only mutated by the owning thread; never touched concurrently.
         * Wrap any tree-builder that allocates ≥1 cell *and* attaches them
         * to a GC root only via a final CAS in a CriticalSection RAII guard.
         */
        unsigned int criticalSectionDepth = 0;

        /**
         * @brief RAII guard for a GC critical section on a ProtoContext.
         *
         * Increments criticalSectionDepth on construction and decrements on
         * destruction; STW polling in allocCell()/safepoint() skips parking
         * while the depth is non-zero.  Use this around any code that holds
         * ProtoObject* / Cell* values in C++ locals across one or more
         * allocations and later attaches them to a GC root (typically by
         * CAS'ing a new mutable snapshot into a mutableRoot shard).  The
         * guard is per-thread, no atomics, no lock.
         */
        class CriticalSection {
        public:
            explicit CriticalSection(ProtoContext* ctx) : ctx_(ctx) {
                if (ctx_) {
                    // Enforce the heap ceiling only at the outermost section:
                    // here criticalSectionDepth is still 0, so the thread
                    // holds no half-built tree and may safely block for the
                    // GC.  Nested sections (depth > 0) skip the check — a
                    // helper is mid-construction with un-anchored cells.
                    if (ctx_->criticalSectionDepth == 0)
                        ctx_->heapLimitCheckpoint();
                    ctx_->criticalSectionDepth++;
                }
            }
            ~CriticalSection() {
                if (ctx_) ctx_->criticalSectionDepth--;
            }
            CriticalSection(const CriticalSection&) = delete;
            CriticalSection& operator=(const CriticalSection&) = delete;
        private:
            ProtoContext* ctx_;
        };
        
        ProtoContext(const ProtoContext&) = delete;
        ProtoContext& operator=(const ProtoContext&) = delete;
    };

    /**
     * @brief The main container for the Proto runtime environment.
     *
     * A ProtoSpace manages the global state, including the object heap,
     * garbage collector, and all running threads. It holds the root const prototypes* for all built-in types.
     */
    /**
     * @brief A registry of GC roots owned by an embedder (e.g. a JS or
     *        Python runtime built on protoCore).
     *
     * Embedders frequently need to keep a `ProtoObject` alive across an
     * allocation boundary that is invisible to protoCore's tracing GC —
     * the canonical case is an asynchronous callback whose receiver is
     * captured into a C++ lambda that fires from a non-protoCore event
     * loop.  Without an explicit root the GC may reclaim the object
     * between enqueue and dispatch.
     *
     * `ProtoRootSet` solves this without forcing every embedder to
     * invent its own anchor scheme on top of `setAttribute`-on-globals
     * (which serialises through the mutable shard locks and is prone to
     * silent CAS livelock under contention).  Each embedder asks the
     * `ProtoSpace` for one or more root sets, calls `add()` to pin a
     * `ProtoObject*` (receiving an opaque `Handle`), and `remove()` to
     * release it.  The GC's marking phase iterates every registered
     * root set during STW and treats their contents as additional
     * roots — see `ProtoSpace::forEachRootSet`.
     *
     * Thread-safe: `add` / `remove` / `resolve` are safe to call from
     * any thread.  `forEach` is intended to be called only from the GC
     * thread during STW; mutators are parked at that point so no
     * concurrent `add`/`remove` can race with iteration.
     */
    class ProtoRootSet
    {
    public:
        using Handle = unsigned long long;
        static constexpr Handle kNullHandle = 0;

        /**
         * @brief Pin `obj` as a GC root.  Returns an opaque handle that
         *        the caller must later pass to `remove()`.  Pinning a
         *        null pointer is a no-op and returns `kNullHandle`.
         */
        Handle add(const ProtoObject* obj);

        /**
         * @brief Look up the object behind a handle without removing it.
         *        Returns nullptr if the handle is unknown or already
         *        removed.  Convenient when the lambda owns the handle
         *        and wants to dispatch on the pinned value.
         */
        const ProtoObject* resolve(Handle h) const;

        /**
         * @brief Release the root pinned by `add`.  Safe to call with
         *        `kNullHandle` (no-op).  Each handle should be removed
         *        exactly once.
         */
        void remove(Handle h);

        /** @brief Number of currently pinned roots — for tests/diagnostics. */
        unsigned long size() const;

        /** @brief Human-readable name set at creation, for diagnostics. */
        const char* getName() const;

        /**
         * @brief Iterate every currently-pinned root.  The visitor is
         *        invoked once per live slot.  Holds the internal mutex
         *        for the duration so it is safe to call concurrently
         *        with `add` / `remove`, but the GC normally calls this
         *        during STW where the lock is uncontended.
         */
        void forEachRoot(void (*visit)(void* user, const ProtoObject* obj),
                          void* user) const;

    private:
        friend class ProtoSpace;
        ProtoRootSet(ProtoSpace* owner, const char* name);
        ~ProtoRootSet();

        struct Impl;
        Impl* impl_;
    };

    class ProtoSpace
    {
    public:
        explicit ProtoSpace();
        ~ProtoSpace();

        //- Core Prototypes
        ProtoObject* objectPrototype{};
        ProtoObject* smallIntegerPrototype{};
        ProtoObject* largeIntegerPrototype{};
        ProtoObject* floatPrototype{};
        ProtoObject* unicodeCharPrototype{};
        ProtoObject* bytePrototype{};
        ProtoObject* nonePrototype{};
        ProtoObject* methodPrototype{};
        ProtoObject* bufferPrototype{};
        ProtoObject* pointerPrototype{};
        ProtoObject* booleanPrototype{};
        ProtoObject* doublePrototype{};
        ProtoObject* datePrototype{};
        ProtoObject* timestampPrototype{};
        ProtoObject* timedeltaPrototype{};
        ProtoObject* threadPrototype{};
        ProtoObject* rootObject{};

        //- Collection Prototypes
        ProtoObject* listPrototype{};
        ProtoObject* listIteratorPrototype{};
        ProtoObject* tuplePrototype{};
        ProtoObject* tupleIteratorPrototype{};
        ProtoObject* stringPrototype{};
        ProtoObject* stringIteratorPrototype{};
        ProtoObject* sparseListPrototype{};
        ProtoObject* sparseListIteratorPrototype{};
        ProtoObject* setPrototype{};
        ProtoObject* setIteratorPrototype{};
        ProtoObject* multisetPrototype{};
        ProtoObject* multisetIteratorPrototype{};
        ProtoObject* rangeIteratorPrototype{};

        // --- Cached Literals ---
        ProtoString* literalData;
        ProtoString* literalSetAttribute;
        ProtoString* literalCallMethod;

        //- Callbacks
        ProtoObject* (*nonMethodCallback)(
            ProtoContext* context,
            const ParentLink* nextParent,
            const ProtoString* method,
            const ProtoObject* self,
            const ProtoList* unnamedParametersList,
            const ProtoSparseList* keywordParametersDict){};

        ProtoObject* (*attributeNotFoundGetCallback)(
            ProtoContext* context,
            const ProtoObject* self,
            const ProtoString* attributeName){};

        ProtoObject* (*parameterNotFoundCallback)(
            ProtoContext* context,
            const ProtoObject* self,
            const ProtoString* attributeName){};

        ProtoObject* (*parameterTwiceAssignedCallback)(
            ProtoContext* context,
            const ProtoObject* self,
            const ProtoString* attributeName){};


        ProtoObject* (*outOfMemoryCallback)(
            ProtoContext* context){};

        ProtoObject* (*invalidConversionCallback)(
            ProtoContext* context){};

        //- Public Methods
        /**
         * @brief Submits a chain of newly created cells (a "young generation")
         * from a returning context to the garbage collector.
         * @param cellChain A pointer to the first cell in the linked list.
         */
        void submitYoungGeneration(const Cell* cellChain);
        
        void deallocMutable(unsigned long mutable_ref);
        const ProtoList* getThreads(ProtoContext* context) const;
        const ProtoThread* newThread(
            ProtoContext *c,
            const ProtoString* name,
            ProtoMethod mainFunction,
            const ProtoList* args,
            const ProtoSparseList* kwargs);

        //- Memory Management & GC
        Cell* getFreeCells(ProtoContext* ctx);
        void analyzeUsedCells(Cell* cellsChain);
        void triggerGC();

        /**
         * @brief Configure the heap allocation watermarks (both in Cells).
         *
         * `softCells` — above this, the Cell allocator prefers GC reclamation
         * over growing the heap (it waits one GC cycle before requesting more
         * memory from the OS).  `hardCells` — the hard ceiling: `heapSize`
         * never exceeds it for ordinary mutator allocations; a thread that
         * would cross it waits for the GC and, if the live working set itself
         * meets the ceiling, the configured out-of-memory path runs.
         *
         * Passing `0` for a limit disables it.  Both `0` (the default) gives
         * unbounded allocation — behaviour identical to a build with no limit.
         * `softCells` is clamped to `<= hardCells` when both are non-zero.
         *
         * The GC thread bypasses the ceiling (it cannot wait on itself).
         * The limit is enforced at critical-section boundaries (see
         * ProtoContext::heapLimitCheckpoint); an allocation that exhausts the
         * cell pool *inside* a critical section may overshoot by at most one
         * OS batch, since it cannot block there.
         */
        void setHeapLimits(int softCells, int hardCells);

        /**
         * @brief Block until the heap has room to satisfy an allocation, or
         *        escalate to out-of-memory handling.
         *
         * Called from ProtoContext::heapLimitCheckpoint (a critical-section
         * boundary) and from the depth-0 path of getFreeCells.  When the heap
         * is at its ceiling with no free cells, it leaves the running set,
         * waits for the GC to reclaim, and re-checks.  If two consecutive GC
         * cycles confirm the live set itself meets the ceiling, it invokes
         * `outOfMemoryCallback` once and then performs a controlled abort.
         *
         * A no-op when no hard limit is configured, on the GC thread, or for
         * a contextless caller.  The caller MUST NOT hold globalMutex and
         * MUST be at criticalSectionDepth == 0.
         */
        void waitForHeapHeadroom(ProtoContext* ctx);

        //- Embedder Root Sets
        //
        // Lets a runtime built on protoCore (e.g. protoJS, protoPython)
        // pin ProtoObjects as GC roots without smuggling them into
        // setAttribute-on-the-global hacks.  See `ProtoRootSet` for the
        // full motivation and threading model.
        //
        // `name` is copied for diagnostics.  Caller must `destroyRootSet`
        // the returned handle before the ProtoSpace is destroyed (or
        // leak on shutdown — the destructor frees any still-registered
        // sets).
        ProtoRootSet* createRootSet(const char* name);
        void destroyRootSet(ProtoRootSet* rs);

        /**
         * @brief Invokes `visit(user, rs)` for every currently-registered
         *        root set.  Called from the GC thread during STW; embedders
         *        should not call this themselves.
         */
        void forEachRootSet(void (*visit)(void* user, ProtoRootSet* rs),
                             void* user) const;

        //- Thread Management
        void allocThread(ProtoContext* context, const ProtoThread* thread);
        void deallocThread(ProtoContext* context, const ProtoThread* thread);
        const ProtoList* getThreads(ProtoContext* context);
        const ProtoThread* getCurrentThread(ProtoContext* context);
        const ProtoThread* getThreadByName(ProtoContext* context, const ProtoString* threadName);

        /** Returns the current resolution chain (ProtoList of ProtoString). Default is platform-dependent if not set. */
        const ProtoObject* getResolutionChain() const;
        /** Sets the resolution chain. \a newChain must be a ProtoList of ProtoString; if null or invalid, restores default chain. */
        void setResolutionChain(const ProtoObject* newChain);
        /** Resolve and load a module by \a logicalPath using this space's resolution chain. Returns a wrapper object with attribute \a attrName2create pointing to the module, or PROTO_NONE. Thread-safe; uses SharedModuleCache. */
        const ProtoObject* getImportModule(ProtoContext* context, const char* logicalPath, const char* attrName2create);

        /**
         * @brief Creates and starts a new managed thread within this ProtoSpace.
         * @param context The current ProtoContext from which the thread is being created.
         * @param threadName A ProtoString representing the name of the new thread.
         * @param target The ProtoMethod to be executed by the new thread.
         * @param args A ProtoList of positional arguments for the target method.
         * @param kwargs A ProtoSparseList of keyword arguments for the target method.
         * @return A pointer to the newly created ProtoThread object.
         */

        const ProtoSpaceImplementation* impl{};
        int state;
        ProtoContext* rootContext;
        // 256 independent shards: mutable_ref % MUTABLE_ROOT_SHARDS selects the shard.
        // Reduces CAS contention and AVL depth in multi-threaded workloads; transparent to API callers.
        // Each shard slot is padded to 64 bytes; the array is cache-line aligned so consecutive
        // slots fall on distinct cache lines, eliminating false sharing between cores
        // performing CAS on different shards.
        static constexpr int MUTABLE_ROOT_SHARDS = 256;
        struct MutableShardSlot {
            std::atomic<ProtoSparseList*> root;
            char _pad[64 - sizeof(std::atomic<ProtoSparseList*>)];
        };
        alignas(64) MutableShardSlot mutableRoot[MUTABLE_ROOT_SHARDS];
        std::atomic<unsigned long> nextMutableRef;

        // --- Maquinaria Interna (Público por ahora) ---

        ProtoSparseList* threads;
        Cell* freeCells;
        // Tail pointer for the global freeCells linked list.  Maintained
        // alongside `freeCells` so getFreeCells can take the entire list in
        // O(1) when the requested batchSize swallows everything available
        // (the common case during warmup and after a small sweep). When
        // freeCells is null, freeCellsTail is also null.
        Cell* freeCellsTail;

        /**
         * @brief Pre-chunked freelist (path #5 v2 chunked GC).
         *
         * Sweep accumulates dead cells into chunks of CELL_CHUNK_SIZE while
         * walking segments (negligible extra cost; we already touch every
         * cell).  At chunk boundary the chunk is published to `freeChunks`.
         *
         * `getFreeCells` pops one chunk in O(1) (no linked-list walk to find
         * a cut point) and returns its head as the thread's refill batch.
         * The chunk struct itself is recycled to `freeChunkPool` after use.
         *
         * `freeCells` remains for the partial / OS-fallback paths; chunks are
         * the fast path.  Both are protected by the same `globalMutex`.
         */
        struct FreeChunk {
            Cell* head;
            Cell* tail;
            unsigned long count;
            FreeChunk* next;
        };
        FreeChunk* freeChunks;
        FreeChunk* freeChunkPool;
        static constexpr unsigned long CELL_CHUNK_SIZE = 8192;

        /** Lock-free stack of dirty segments; GC drains under globalMutex. */
        std::atomic<DirtySegment*> dirtySegments;
        /**
         * Lock-free free pool of unused DirtySegments.  submitYoungGeneration
         * pops from here before falling back to a fresh allocation; the GC
         * pushes consumed segments back here after sweep instead of freeing
         * them.  Eliminates the per-Python-method-call malloc/free pair on
         * the hot path (~480 K/run on richards_lite).
         */
        std::atomic<DirtySegment*> dirtySegmentFreePool;

        /**
         * @brief Holding pen for sweep survivors when stagger > 1.
         *
         * The survivor re-chain (PROTOCORE_GC_REINCLUDE_SURVIVORS) re-pushes
         * cells that were marked alive into a follow-up segment so the next
         * cycle can re-examine them.  By default that "next cycle" is
         * literally the next one, which means every live cell pays the
         * mark cost once per cycle.  With stagger > 1, sweep instead pushes
         * survivors here; the pen is folded back into `dirtySegments` only
         * once every `survivorStagger` cycles, so survivors are re-examined
         * less often.  RSS grows by up to `stagger × working set` because
         * cells that became unreachable during the staggered window stay
         * alive in the pen until the next fold; mark cost across the same
         * window drops to ~`1/stagger` of the un-staggered baseline.
         *
         * Lock-free LIFO push from sweep, atomic-exchange-to-nullptr drain
         * from gcThreadLoop's STW phase.  Always-empty when stagger == 1.
         */
        std::atomic<DirtySegment*> survivorPen;

        /**
         * @brief How many GC cycles between successive folds of the
         *        survivor pen back into dirtySegments.
         *
         * 1 = re-check every cycle (current behaviour, no stagger).
         * 2 = re-check every other cycle.
         * N = re-check every Nth cycle.
         *
         * Default: 1 (no stagger).  Override at startup via env var
         * PROTOCORE_GC_SURVIVOR_STAGGER.  Set once at ProtoSpace
         * construction, never modified afterwards.
         */
        static constexpr unsigned int SURVIVOR_STAGGER_DEFAULT = 1;
        unsigned int survivorStagger;

        /**
         * @brief Monotonic GC cycle counter.
         *
         * Incremented by the GC thread (single writer) at the start of each
         * cycle, inside STW.  Read by mutator threads via `getGCCycleCount()`
         * to invalidate per-thread caches that hold raw cell pointers
         * (e.g. embedder behavior caches keyed by ProtoObject*).
         *
         * Atomic with relaxed ordering: the value is only used as a
         * change-detection token, not as a synchronisation primitive.
         * Mutator reads observing a stale value will simply postpone
         * cache invalidation until the next call, which is safe because
         * the cache is consulted with the same pointer on the next hot
         * path entry.
         *
         * When `gcCycleCount % survivorStagger == 0`, the survivor pen is
         * folded back into `dirtySegments` before the cycle's root collection.
         */
        std::atomic<uint64_t> gcCycleCount;

        /**
         * @brief Cells found reachable by the most recently completed GC
         * cycle's mark phase.  Published by the GC thread at end of cycle;
         * used only for the diagnostic message of the out-of-memory abort.
         */
        std::atomic<unsigned long> liveCellsLastCycle;

        /**
         * @brief Cells reclaimed (swept into the freelist) by the most
         * recently completed GC cycle.  Published by the GC thread at end of
         * cycle.
         *
         * This is the authoritative out-of-memory signal for the heap-limit
         * path (ProtoSpace::waitForHeapHeadroom): once the heap is at its
         * ceiling, two consecutive completed cycles that each reclaim zero
         * cells mean no collection can free space — genuine, unrecoverable
         * OOM.  Reclamation, not mark-phase reachability, is the metric:
         * retained-but-unmarked cells (a live context's un-submitted young
         * generation) fill the heap without ever entering `markedList`, so a
         * mark-based count would under-report and miss that OOM.
         */
        std::atomic<unsigned long> reclaimedLastCycle;

        /**
         * @brief Notified by the GC thread at the end of every cycle, once the
         * sweep has published reclaimed Cells to the freelist.  A thread
         * parked in getFreeCells() waiting for memory wakes here and re-checks.
         */
        std::condition_variable_any memoryReclaimedCV;

        /**
         * @brief Returns the current GC cycle counter (relaxed load).
         *
         * Embedders cache cell-pointer-keyed data on the assumption that the
         * pointer remains valid; after a GC cycle, freed pointers can be
         * recycled, so caches must be invalidated.  Compare the returned
         * value against a thread-local snapshot; any change means at least
         * one GC cycle has happened since the last check, so caches must
         * be cleared before reuse.
         */
        uint64_t getGCCycleCount() const { return gcCycleCount.load(std::memory_order_relaxed); }

        /**
         * @brief Per-context allocation threshold for the GC trigger.
         *
         * When PROTOCORE_GC_REINCLUDE_SURVIVORS is enabled and a single
         * ProtoContext has allocated more than this many cells since its
         * last GC submission, allocCell() submits the context's young
         * chain to dirtySegments, resets the per-context count, and calls
         * triggerGC().  This bounds RSS in tight loops with small working
         * sets without changing the algorithm: the GC still marks from
         * roots and frees what is unreachable.
         *
         * Default: CONTEXT_GC_THRESHOLD_DEFAULT (10000 cells).
         * Override at startup via env var PROTOCORE_GC_CONTEXT_THRESHOLD.
         *
         * Read by ProtoContext::allocCell() in the hot path; not atomic
         * because it is set once at construction and never changed.
         */
        static constexpr unsigned int CONTEXT_GC_THRESHOLD_DEFAULT = 10000;
        unsigned int maxAllocatedCellsPerContext;
        int blocksPerAllocation;
        int heapSize;
        /**
         * @brief Hard heap ceiling, in Cells.  `heapSize` never exceeds this
         * for ordinary mutator allocations.  `0` disables the ceiling.
         * Set via setHeapLimits(); see getFreeCells() for the enforcement.
         */
        int maxHeapSize;
        /**
         * @brief Soft heap watermark, in Cells.  Above it, the Cell allocator
         * prefers GC reclamation over growing the heap.  `0` disables it.
         */
        int softHeapLimit;
        int freeCellsCount;
        unsigned int gcSleepMilliseconds;
        std::atomic<TupleDictionary*> tupleRoot;
        void* stringInternMap;
        SymbolTable* symbolTable{};
        std::atomic<bool> mutableLock;
        std::atomic<bool> threadsLock;
        std::atomic<bool> gcLock;
        std::thread::id mainThreadId;
        std::unique_ptr<std::thread> gcThread; // Usando unique_ptr
        std::condition_variable_any stopTheWorldCV;
        std::condition_variable_any restartTheWorldCV;
        std::condition_variable_any gcCV;
        std::atomic<bool> gcStarted;
        std::atomic<int> runningThreads;
        std::atomic<bool> stwFlag;
        std::atomic<int> parkedThreads;
        ProtoContext* mainContext;

        const ProtoList* resolutionChain_;
        std::vector<const ProtoObject*> moduleRoots;
        std::mutex moduleRootsMutex;

        /** @brief Global reentrant mutex for protecting space-wide metadata (interning, thread registry, GC state). */
        static std::recursive_mutex globalMutex;

        // --- Embedder root sets (see `createRootSet`) ---
        std::vector<ProtoRootSet*> rootSets_;
        mutable std::mutex rootSetsMutex_;
    };
}

#endif /* PROTO_H_ */
