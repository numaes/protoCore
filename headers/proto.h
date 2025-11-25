/*
 * proto
 *
 *  Created on: November, 2017 - Redesign January, 2024
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 */

#ifndef PROTO_H_
#define PROTO_H_

#include <atomic>
#include <condition_variable>

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
    class ParentLink;
    class ProtoList;
    class ProtoListIterator;
    class ProtoSparseList;
    class ProtoSparseListIterator;
    class ProtoObjectCell;
    class ProtoThread;
    class ProtoSpaceImplementation;

    //! Useful constants.
    //! @warning They should be kept in sync with proto_internal.h!
    #define PROTO_TRUE ((const proto::ProtoObject*)  0x010FL)
    #define PROTO_FALSE ((const proto::ProtoObject*) 0x000FL)
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
        const ProtoObject* getAttribute(ProtoContext* context, ProtoString* name) const;
        const ProtoObject* hasAttribute(ProtoContext* context, ProtoString* name) const;
        const ProtoObject* hasOwnAttribute(ProtoContext* context, ProtoString* name) const;
        const ProtoObject* setAttribute(ProtoContext* context, ProtoString* name, const ProtoObject* value) const;
        const ProtoSparseList* getAttributes(ProtoContext* context) const;
        const ProtoSparseList* getOwnAttributes(ProtoContext* context) const;

        //- Inheritance
        const ProtoList* getParents(ProtoContext* context) const;
        const ProtoObject* addParent(ProtoContext* context, const ProtoObject* newParent);
        const ProtoObject* isInstanceOf(ProtoContext* context, const ProtoObject* prototype) const;

        //- Execution
        const ProtoObject* call(ProtoContext* context,
                                const ParentLink* nextParent,
                                const ProtoString* method,
                                const ProtoObject* self,
                                const ProtoList* positionalParameters,
                                const ProtoSparseList* keywordParametersDict = nullptr) const;

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

        //- Type Coercion
        bool asBoolean(ProtoContext* context) const;
        int asInteger(ProtoContext* context) const;
        float asFloat(ProtoContext* context) const;
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
        const ProtoThread* asThread(ProtoContext* context) const;
        ProtoMethod asMethod(ProtoContext* context) const;
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

    class ProtoExternalPointer
    {
    public:
        void* getPointer(ProtoContext* context) const;
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
        void setManaged();
        void setUnmanaged();
        void synchToGC();
    };

    /**
     * @brief Represents the execution context for a thread.
     *
     * A ProtoContext holds the call stack (via the `previousNode` pointer),
     * the local variables for the current scope, and references to the
     * current thread and the ProtoSpace it belongs to. It also acts as
     * the primary factory for creating new Proto objects.
     */
    class ProtoContext
    {
    public:
        explicit ProtoContext(
            ProtoSpace* space = nullptr,
            ProtoContext* previous = nullptr,
            ProtoObject** localsBase = nullptr,
            unsigned int localsCount = 0
        );
        ~ProtoContext();

        //- Execution State
        ProtoContext* previous;
        ProtoSpace* space;
        ProtoThread* thread;
        ProtoObject** localsBase;
        unsigned int localsCount;

        //- Return Value
        const ProtoObject* returnValue;

        //- Factory methods for primitive types.
        const ProtoObject* fromInteger(int value);
        const ProtoObject* fromFloat(float value);
        const ProtoObject* fromUTF8Char(const char* utf8OneCharString);
        const ProtoObject* fromUTF8String(const char* zeroTerminatedUtf8String);
        const ProtoObject* fromMethod(ProtoObject* self, ProtoMethod method);
        const ProtoObject* fromExternalPointer(void* pointer);
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
        const ProtoObject* newObject(bool mutableObject = false);

        //- Memory Management
        Cell* allocCell();
        void addCell2Context(Cell* cell);

        Cell* lastAllocatedCell;
        unsigned long allocatedCellsCount;
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

        // --- Cached Literals ---
        ProtoString* literalGetAttribute;
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

        // ---- Métodos públicos ---
        void analyzeUsedCells(const Cell* cellsChain);
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

        // --- Maquinaria Interna (Público por ahora) ---

        ProtoSparseList* threads;
        Cell* freeCells;
        DirtySegment* dirtySegments;
        unsigned int maxAllocatedCellsPerContext;
        int blocksPerAllocation;
        int heapSize;
        int maxHeapSize;
        int freeCellsCount;
        unsigned int gcSleepMilliseconds;
        int blockOnNoMemory;
        std::atomic<TupleDictionary*> tupleRoot;
        std::atomic<bool> mutableLock;
        std::atomic<bool> threadsLock;
        std::atomic<bool> gcLock;
        std::thread::id mainThreadId;
        std::unique_ptr<std::thread> gcThread; // Usando unique_ptr
        std::condition_variable stopTheWorldCV;
        std::condition_variable restartTheWorldCV;
        std::condition_variable gcCV;
        bool gcStarted;
        std::atomic<int> runningThreads;

        static std::mutex globalMutex;
    };
}

#endif /* PROTO_H_ */
