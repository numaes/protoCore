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
    // Forward declarations
    class ProtoStringIterator;
    class ProtoTupleIterator;
    class Cell;
    class BigCell;
    class ProtoContext;
    class ProtoSpace;
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
    class ProtoSparseListIterator;
    class ProtoSet;
    class ProtoSetIterator;
    class ProtoMultiset;
    class ProtoMultisetIterator;
    class ProtoObjectCell;
    class ProtoThread;
    class ProtoSpaceImplementation;
    class ModuleProvider;
    class ProviderRegistry;

    //! Useful constants.
    //! @warning They should be kept in sync with proto_internal.h!
    #define PROTO_TRUE ((const proto::ProtoObject*)  1217UL) // Tag: EMBEDDED_VALUE (1), Type: BOOLEAN (3), Value: 1
    #define PROTO_FALSE ((const proto::ProtoObject*) 193UL)  // Tag: EMBEDDED_VALUE (1), Type: BOOLEAN (3), Value: 0
    #define PROTO_NONE ((const proto::ProtoObject*) nullptr)

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
        const ProtoSparseList* getAttributes(ProtoContext* context) const;
        const ProtoSparseList* getOwnAttributes(ProtoContext* context) const;

        //- Inheritance
        const ProtoList* getParents(ProtoContext* context) const;
        const ProtoObject* addParent(ProtoContext* context, const ProtoObject* newParent) const;
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

        bool isBoolean(ProtoContext* context) const;
        bool isInteger(ProtoContext* context) const;
        bool isFloat(ProtoContext* context) const;
        bool isByte(ProtoContext* context) const;
        bool isDate(ProtoContext* context) const;
        bool isTimestamp(ProtoContext* context) const;
        bool isTimeDelta(ProtoContext* context) const;
        bool isMethod(ProtoContext* context) const;
        bool isNone(ProtoContext* context) const;
        bool isString(ProtoContext* context) const;
        bool isDouble(ProtoContext* context) const;
        bool isTuple(ProtoContext* context) const;
        bool isSet(ProtoContext* context) const;
        bool isMultiset(ProtoContext* context) const;

        //- Type Coercion
        bool asBoolean(ProtoContext* context) const;
        long long asLong(ProtoContext* context) const;
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
        /** If this object is a ProtoExternalBuffer, returns the raw segment pointer; otherwise nullptr. Stable until the object is collected (no compaction). */
        void* getRawPointerIfExternalBuffer(ProtoContext* context) const;
        ProtoMethod asMethod(ProtoContext* context) const;

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
        static const ProtoString* fromUTF8String(ProtoContext* context, const char* zeroTerminatedUtf8String);

        int cmp_to_string(ProtoContext* context, const ProtoString* otherString) const;

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

        //- Conversion
        const ProtoObject* asObject(ProtoContext* context) const;
        const ProtoList* asList(ProtoContext* context) const;
        const ProtoStringIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
        const ProtoString* isCell(ProtoContext* context) const;
        const Cell* asCell(ProtoContext* context) const;

        /** Appends the UTF-8 representation of this string to \a out. */
        void toUTF8String(ProtoContext* context, std::string& out) const;
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
         */
        explicit ProtoContext( // Add default arguments to allow fewer parameters
            ProtoSpace* space,
            ProtoContext* previous = nullptr,
            const ProtoList* parameterNames = nullptr,
            const ProtoList* localNames = nullptr,
            const ProtoList* args = nullptr,
            const ProtoSparseList* kwargs = nullptr
        );
        ~ProtoContext();

        //- Execution State
        ProtoContext* previous;
        ProtoSpace* space;
        ProtoThread* thread;
        
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
        const ProtoList* newList();
        const ProtoTuple* newTuple();
        const ProtoTuple* newTupleFromList(const ProtoList* sourceList);
        const ProtoSparseList* newSparseList();
        const ProtoSet* newSet();
        const ProtoMultiset* newMultiset();
        const ProtoObject* newObject(bool mutableObject = false);
        /** Allocates a contiguous buffer (aligned_alloc). GC finalize frees it when descriptor is collected (Shadow GC). */
        const ProtoObject* newExternalBuffer(unsigned long size);

        //- Memory Management
        Cell* allocCell();
        void addCell2Context(Cell* cell);

        Cell* lastAllocatedCell;
        unsigned long allocatedCellsCount;
        Cell* freeCells;
    };

    /**
     * @brief The main container for the Proto runtime environment.
     *
     * A ProtoSpace manages the global state, including the object heap,
     * garbage collector, and all running threads. It holds the root const prototypes* for all built-in types.
     */
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
        Cell* getFreeCells(const ProtoThread* currentThread);
        void analyzeUsedCells(Cell* cellsChain);
        void triggerGC();

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
        std::atomic<ProtoSparseList*> mutableRoot;
        std::atomic<unsigned long> nextMutableRef;

        // --- Maquinaria Interna (Público por ahora) ---

        ProtoSparseList* threads;
        Cell* freeCells;
        /** Lock-free stack of dirty segments; GC drains under globalMutex. */
        std::atomic<DirtySegment*> dirtySegments;
        unsigned int maxAllocatedCellsPerContext;
        int blocksPerAllocation;
        int heapSize;
        int maxHeapSize;
        int freeCellsCount;
        unsigned int gcSleepMilliseconds;
        int blockOnNoMemory;
        std::atomic<TupleDictionary*> tupleRoot;
        void* stringInternMap;
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
    };
}

#endif /* PROTO_H_ */
