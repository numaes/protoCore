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

    //! Useful constants.
    //! @warning They should be kept in sync with proto_internal.h!
    #define PROTO_TRUE ((proto::ProtoObject *)  0x010FL)
    #define PROTO_FALSE ((proto::ProtoObject *) 0x000FL)
    #define PROTO_NONE ((proto::ProtoObject *) nullptr)

    typedef ProtoObject*(*ProtoMethod)(
        ProtoContext* context,
        ProtoObject* self,
        ParentLink* parentLink,
        ProtoList* positionalParameters,
        ProtoSparseList* keywordParameters
    );

    class ProtoObject
    {
    public:
        //- Object Model
        ProtoObject* getPrototype(ProtoContext* context) const;
        ProtoObject* clone(ProtoContext* context, bool isMutable = false) const;
        ProtoObject* newChild(ProtoContext* context, bool isMutable = false) const;

        //- Attributes
        ProtoObject* getAttribute(ProtoContext* context, ProtoString* name) const;
        ProtoObject* hasAttribute(ProtoContext* context, ProtoString* name) const;
        ProtoObject* hasOwnAttribute(ProtoContext* context, ProtoString* name) const;
        ProtoObject* setAttribute(ProtoContext* context, ProtoString* name, ProtoObject* value);
        ProtoSparseList* getAttributes(ProtoContext* context) const;
        ProtoSparseList* getOwnAttributes(ProtoContext* context) const;

        //- Inheritance
        ProtoList* getParents(ProtoContext* context) const;
        ProtoObject* addParent(ProtoContext* context, ProtoObject* newParent);
        ProtoObject* isInstanceOf(ProtoContext* context, const ProtoObject* prototype) const;

        //- Execution
        ProtoObject* call(ProtoContext* context,
                          ParentLink* nextParent,
                          ProtoString* method,
                          ProtoObject* self,
                          ProtoList* unnamedParametersList = nullptr,
                          ProtoSparseList* keywordParametersDict = nullptr);

        //- Internals & Type Checking
        unsigned long getHash(ProtoContext* context) const;
        int isCell(ProtoContext* context) const;
        Cell* asCell(ProtoContext* context) const;

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
        ProtoList* asList(ProtoContext* context) const;
        ProtoListIterator* asListIterator(ProtoContext* context) const;
        ProtoTuple* asTuple(ProtoContext* context) const;
        ProtoTupleIterator* asTupleIterator(ProtoContext* context) const;
        ProtoString* asString(ProtoContext* context) const;
        ProtoStringIterator* asStringIterator(ProtoContext* context) const;
        ProtoSparseList* asSparseList(ProtoContext* context) const;
        ProtoSparseListIterator* asSparseListIterator(ProtoContext* context) const;
        ProtoThread* asThread(ProtoContext* context) const;
        ProtoMethod asMethod(ProtoContext* context) const;
    };

    class ProtoListIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        ProtoObject* next(ProtoContext* context);
        ProtoListIterator* advance(ProtoContext* context);
        ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoList
    {
    public:
        //- Accessors
        ProtoObject* getAt(ProtoContext* context, int index) const;
        ProtoObject* getFirst(ProtoContext* context) const;
        ProtoObject* getLast(ProtoContext* context) const;
        ProtoList* getSlice(ProtoContext* context, int from, int to) const;
        unsigned long getSize(ProtoContext* context) const;
        bool has(ProtoContext* context, ProtoObject* value) const;

        //- Modifiers that return a new list
        ProtoList* setAt(ProtoContext* context, int index, ProtoObject* value) const;
        ProtoList* insertAt(ProtoContext* context, int index, ProtoObject* value) const;
        ProtoList* appendFirst(ProtoContext* context, ProtoObject* value) const;
        ProtoList* appendLast(ProtoContext* context, ProtoObject* value) const;
        ProtoList* extend(ProtoContext* context, ProtoList* other) const;
        ProtoList* splitFirst(ProtoContext* context, int index) const;
        ProtoList* splitLast(ProtoContext* context, int index) const;
        ProtoList* removeFirst(ProtoContext* context) const;
        ProtoList* removeLast(ProtoContext* context) const;
        ProtoList* removeAt(ProtoContext* context, int index) const;
        ProtoList* removeSlice(ProtoContext* context, int from, int to) const;

        //- Conversion
        ProtoObject* asObject(ProtoContext* context) const;
        ProtoListIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoTupleIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        ProtoObject* next(ProtoContext* context);
        ProtoTupleIterator* advance(ProtoContext* context);
        ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoTuple
    {
    public:
        //- Accessors
        ProtoObject* getAt(ProtoContext* context, int index) const;
        ProtoObject* getFirst(ProtoContext* context) const;
        ProtoObject* getLast(ProtoContext* context) const;
        ProtoObject* getSlice(ProtoContext* context, int from, int to) const;
        unsigned long getSize(ProtoContext* context) const;
        bool has(ProtoContext* context, ProtoObject* value) const;

        //- "Modifiers" (return new tuples)
        ProtoObject* setAt(ProtoContext* context, int index, ProtoObject* value) const;
        ProtoObject* insertAt(ProtoContext* context, int index, ProtoObject* value) const;
        ProtoObject* appendFirst(ProtoContext* context, ProtoTuple* otherTuple) const;
        ProtoObject* appendLast(ProtoContext* context, ProtoTuple* otherTuple) const;
        ProtoObject* splitFirst(ProtoContext* context, int count) const;
        ProtoObject* splitLast(ProtoContext* context, int count) const;
        ProtoObject* removeFirst(ProtoContext* context, int count) const;
        ProtoObject* removeLast(ProtoContext* context, int count) const;
        ProtoObject* removeAt(ProtoContext* context, int index) const;
        ProtoObject* removeSlice(ProtoContext* context, int from, int to) const;

        //- Conversion
        ProtoList* asList(ProtoContext* context) const;
        ProtoObject* asObject(ProtoContext* context) const;
        ProtoTupleIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoStringIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        ProtoObject* next(ProtoContext* context);
        ProtoStringIterator* advance(ProtoContext* context);
        ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoString
    {
    public:
        static ProtoString* fromUTF8String(const char* zeroTerminatedUtf8String);

        int cmp_to_string(ProtoContext* context, ProtoString* otherString) const;

        //- Accessors
        ProtoObject* getAt(ProtoContext* context, int index) const;
        unsigned long getSize(ProtoContext* context) const;
        ProtoString* getSlice(ProtoContext* context, int from, int to) const;

        //- "Modifiers" (return new strings)
        ProtoString* setAt(ProtoContext* context, int index, ProtoObject* character) const;
        ProtoString* insertAt(ProtoContext* context, int index, ProtoObject* character) const;
        ProtoString* setAtString(ProtoContext* context, int index, ProtoString* otherString) const;
        ProtoString* insertAtString(ProtoContext* context, int index, ProtoString* otherString) const;
        ProtoString* appendFirst(ProtoContext* context, ProtoString* otherString) const;
        ProtoString* appendLast(ProtoContext* context, ProtoString* otherString) const;
        ProtoString* splitFirst(ProtoContext* context, int count) const;
        ProtoString* splitLast(ProtoContext* context, int count) const;
        ProtoString* removeFirst(ProtoContext* context, int count) const;
        ProtoString* removeLast(ProtoContext* context, int count) const;
        ProtoString* removeAt(ProtoContext* context, int index) const;
        ProtoString* removeSlice(ProtoContext* context, int from, int to) const;

        //- Conversion
        ProtoObject* asObject(ProtoContext* context) const;
        ProtoList* asList(ProtoContext* context) const;
        ProtoStringIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoSparseListIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        unsigned long nextKey(ProtoContext* context) const;
        ProtoObject* nextValue(ProtoContext* context) const;
        ProtoSparseListIterator* advance(ProtoContext* context);
        ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoSparseList
    {
    public:
        bool has(ProtoContext* context, unsigned long index) const;
        ProtoObject* getAt(ProtoContext* context, unsigned long index) const;
        ProtoSparseList* setAt(ProtoContext* context, unsigned long index, ProtoObject* value) const;
        ProtoSparseList* removeAt(ProtoContext* context, unsigned long index) const;
        bool isEqual(ProtoContext* context, ProtoSparseList* otherDict) const;
        unsigned long getSize(ProtoContext* context) const;

        ProtoObject* asObject(ProtoContext* context) const;
        ProtoSparseListIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;

        void processElements(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, unsigned long, ProtoObject*)) const;
        void processValues(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, ProtoObject*)) const;
    };

    class ProtoByteBuffer
    {
    public:
        unsigned long getSize(ProtoContext* context) const;
        char* getBuffer(ProtoContext* context) const;
        char getAt(ProtoContext* context, int index) const;
        void setAt(ProtoContext* context, int index, char value);
        ProtoObject* asObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoExternalPointer
    {
    public:
        void* getPointer(ProtoContext* context) const;
        ProtoObject* asObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoThread
    {
    public:
        static ProtoThread* getCurrentThread(ProtoContext* context);

        void detach(ProtoContext* context);
        void join(ProtoContext* context);
        void exit(ProtoContext* context);

        ProtoObject* getName(ProtoContext* context) const;
        ProtoObject* asObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;

        void setCurrentContext(ProtoContext* context);
        void setManaged();
        void setUnmanaged();
        void synchToGC();
    };

    /**
     * @brief Represents the execution context for a thread.
     *
     * A ProtoContext holds the call stack (via the `previous` pointer),
     * the local variables for the current scope, and references to the
     * current thread and the ProtoSpace it belongs to. It also acts as
     * the primary factory for creating new Proto objects.
     */
    class ProtoContext
    {
    public:
        explicit ProtoContext(
            ProtoContext* previous = nullptr,
            ProtoObject** localsBase = nullptr,
            unsigned int localsCount = 0,
            ProtoThread* thread = nullptr,
            ProtoSpace* space = nullptr
        );
        ~ProtoContext();

        //- Execution State
        ProtoContext* previous;
        ProtoSpace* space;
        ProtoThread* thread;
        ProtoObject** localsBase;
        unsigned int localsCount;

        //- Return Value
        ProtoObject* returnValue;

        //- Factory methods for primitive types.
        ProtoObject* fromInteger(int value);
        ProtoObject* fromFloat(float value);
        ProtoObject* fromUTF8Char(const char* utf8OneCharString);
        ProtoObject* fromUTF8String(const char* zeroTerminatedUtf8String);
        ProtoObject* fromMethod(ProtoObject* self, ProtoMethod method);
        ProtoObject* fromExternalPointer(void* pointer);
        ProtoObject* fromBuffer(unsigned long length, char* buffer);
        ProtoObject* newBuffer(unsigned long length);
        ProtoObject* fromBoolean(bool value);
        ProtoObject* fromByte(char c);
        ProtoObject* fromDate(unsigned year, unsigned month, unsigned day);
        ProtoObject* fromTimestamp(unsigned long timestamp);
        ProtoObject* fromTimeDelta(long timedelta);

        //- Factory methods for complex types
        ProtoList* newList();
        ProtoTuple* newTuple();
        ProtoTuple* newTupleFromList(ProtoList* sourceList);
        ProtoSparseList* newSparseList();
        ProtoObject* newObject(bool mutableObject = false);

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
     * garbage collector, and all running threads. It holds the root prototypes
     * for all built-in types.
     */
    class ProtoSpace
    {
    public:
        explicit ProtoSpace(ProtoMethod mainFunction, int argc, char** argv = nullptr);
        ~ProtoSpace();

        //- Core Prototypes
        ProtoObject* objectPrototype;
        ProtoObject* smallIntegerPrototype;
        ProtoObject* floatPrototype;
        ProtoObject* unicodeCharPrototype;
        ProtoObject* bytePrototype;
        ProtoObject* nonePrototype;
        ProtoObject* methodPrototype;
        ProtoObject* bufferPrototype;
        ProtoObject* pointerPrototype;
        ProtoObject* booleanPrototype;
        ProtoObject* doublePrototype;
        ProtoObject* datePrototype;
        ProtoObject* timestampPrototype;
        ProtoObject* timedeltaPrototype;
        ProtoObject* threadPrototype;
        ProtoObject* rootObject;

        //- Collection Prototypes
        ProtoObject* listPrototype;
        ProtoObject* listIteratorPrototype;
        ProtoObject* tuplePrototype;
        ProtoObject* tupleIteratorPrototype;
        ProtoObject* stringPrototype;
        ProtoObject* stringIteratorPrototype;
        ProtoObject* sparseListPrototype;
        ProtoObject* sparseListIteratorPrototype;

        //- Memory Management & GC
        Cell* getFreeCells(ProtoThread* currentThread);
        void analyzeUsedCells(Cell* cellsChain);
        void triggerGC();

        //- Thread Management
        void allocThread(ProtoContext* context, ProtoThread* thread);
        void deallocThread(ProtoContext* context, ProtoThread* thread);
        ProtoList* getThreads(ProtoContext* context);
        ProtoThread* getCurrentThread(ProtoContext* context);
        ProtoThread* getThreadByName(ProtoContext* context, ProtoString* threadName);

        /**
         * @brief Creates and starts a new managed thread within this ProtoSpace.
         * @param context The current ProtoContext from which the thread is being created.
         * @param threadName A ProtoString representing the name of the new thread.
         * @param target The ProtoMethod to be executed by the new thread.
         * @param args A ProtoList of positional arguments for the target method.
         * @param kwargs A ProtoSparseList of keyword arguments for the target method.
         * @return A pointer to the newly created ProtoThread object.
         */

        ProtoThread* newThread(
            ProtoContext* context,
            ProtoString* threadName,
            ProtoMethod target,
            ProtoList* args,
            ProtoSparseList* kwargs
        );

    private:
        ProtoSparseList* threads;
    };
}

#endif /* PROTO_H_ */
