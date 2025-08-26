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
	class ProtoStringIterator;
	class ProtoTupleIterator;

	typedef int BOOLEAN;

	// Useful constants.
	// ATTENTION: They should be kept in sync with proto_internal.h!

#define PROTO_TRUE ((proto::ProtoObject *)  0x010FL)
#define PROTO_FALSE ((proto::ProtoObject *) 0x000FL)
#define PROTO_NONE ((proto::ProtoObject *) NULL)

	/**
	 * @brief The root base for any internal structure.
	 *
	 * All Cell objects are designed to be immutable once initialized.
	 * The only mutable fields are `context` and `nextCell`, with the assumption
	 * that any new `nextCell` preserves the existing chain.
	 * Modifying `context` or `nextCell` are the only permitted changes,
	 * and they must be performed atomically.
	 *
	 * Cells must always be less than or equal to 64 bytes in size.
	 * Maintaining a power-of-two size offers significant advantages and opens
	 * the possibility of extending the model to massively parallel computing architectures.
	 *
	 * Cell allocations are performed in OS page-sized chunks.
	 * The page size is always a power of two and larger than 64 bytes.
	 * This is the only allocation method for Proto objects or scalars.
	 */

	// Forward declarations

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


	typedef ProtoObject*(*ProtoMethod)(
		ProtoContext*, // context
		ProtoObject*, // self
		ParentLink*, // parentLink
		ProtoList*, // positionalParameters
		ProtoSparseList* // keywordParameters
	);

	class ProtoObject
	{
	public:
		ProtoObject* getPrototype(const ProtoContext* c);
		ProtoObject* clone(ProtoContext* c, bool isMutable = false);
		ProtoObject* newChild(ProtoContext* c, bool isMutable = false);

		ProtoObject* getAttribute(ProtoContext* c, ProtoString* name);
		ProtoObject* hasAttribute(ProtoContext* c, ProtoString* name);
		ProtoObject* hasOwnAttribute(ProtoContext* c, ProtoString* name);
		ProtoObject* setAttribute(ProtoContext* c, ProtoString* name, ProtoObject* value);

		ProtoSparseList* getAttributes(ProtoContext* c);
		ProtoSparseList* getOwnAttributes(ProtoContext* c);
		ProtoList* getParents(ProtoContext* c);

		ProtoObject* addParent(ProtoContext* c, ProtoObject* newParent);
		ProtoObject* isInstanceOf(ProtoContext* c, const ProtoObject* prototype);

		ProtoObject* call(ProtoContext* c,
		                  ParentLink* nextParent,
		                  ProtoString* method,
		                  ProtoObject* self,
		                  ProtoList* unnamedParametersList = nullptr, ProtoSparseList* keywordParametersDict = nullptr);

		unsigned long getHash(ProtoContext* context);
		int isCell(ProtoContext* context);
		Cell* asCell(ProtoContext* context);

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

		bool isBoolean(ProtoContext *context);
		bool isInteger(ProtoContext *context);
		bool isFloat(ProtoContext *context);
		bool isByte(ProtoContext *context);
		bool isDate(ProtoContext *context);
		bool isTimestamp(ProtoContext *context);
		bool isTimeDelta(ProtoContext *context);
		bool isMethod(ProtoContext *context);

		bool asBoolean(ProtoContext *context);
		int asInteger(ProtoContext *context);
		float asFloat(ProtoContext *context);
		char asByte(ProtoContext *context);
		void asDate(ProtoContext *context, unsigned int& year, unsigned& month, unsigned& day);
		unsigned long asTimestamp(ProtoContext *context);
		long asTimeDelta(ProtoContext *context);
		ProtoList * asList(ProtoContext *context);
		ProtoListIterator * asListIterator(ProtoContext *context);
		ProtoTuple * asTuple(ProtoContext *context);
		ProtoTupleIterator * asTupleIterator(ProtoContext *context);
		ProtoString * asString(ProtoContext *context);
		ProtoStringIterator * asStringIterator(ProtoContext *context);
		ProtoSparseList * asSparseList(ProtoContext *context);
		ProtoSparseListIterator * asSparseListIterator(ProtoContext *context);
		ProtoMethod asMethod(ProtoContext *context);

	};


	// Represents a link in the prototype chain, used for attribute lookup.
	class ParentLink
	{
	public:
		ProtoObject* getObject(ProtoContext* context);
		ParentLink* getParent(ProtoContext* context);
	};

	class ProtoListIterator
	{
	public:
		~ProtoListIterator() = default;
		int hasNext(ProtoContext* context) ;
		ProtoObject* next(ProtoContext* context) ;
		ProtoListIterator* advance(ProtoContext* context) ;

		ProtoObject* asObject(ProtoContext* context) ;
	};

	class ProtoList
	{
	public:
		~ProtoList() = default;
		ProtoObject* getAt(ProtoContext* context, int index) ;
		ProtoObject* getFirst(ProtoContext* context) ;
		ProtoObject* getLast(ProtoContext* context) ;
		ProtoList* getSlice(ProtoContext* context, int from, int to) ;
		unsigned long getSize(ProtoContext* context) ;

		bool has(ProtoContext* context, ProtoObject* value) ;
		ProtoList* setAt(ProtoContext* context, int index, ProtoObject* value) ;
		ProtoList* insertAt(ProtoContext* context, int index, ProtoObject* value) ;

		ProtoList* appendFirst(ProtoContext* context, ProtoObject* value) ;
		ProtoList* appendLast(ProtoContext* context, ProtoObject* value) ;

		ProtoList* extend(ProtoContext* context, ProtoList* other) ;

		ProtoList* splitFirst(ProtoContext* context, int index) ;
		ProtoList* splitLast(ProtoContext* context, int index) ;

		ProtoList* removeFirst(ProtoContext* context) ;
		ProtoList* removeLast(ProtoContext* context) ;
		ProtoList* removeAt(ProtoContext* context, int index) ;
		ProtoList* removeSlice(ProtoContext* context, int from, int to) ;

		ProtoObject* asObject(ProtoContext* context) ;
		unsigned long getHash(ProtoContext* context) ;
		ProtoListIterator* getIterator(ProtoContext* context) ;
	};

	class ProtoTupleIterator
	{
	public:
		~ProtoTupleIterator() = default;
		int hasNext(ProtoContext* context) ;
		ProtoObject* next(ProtoContext* context) ;
		ProtoTupleIterator* advance(ProtoContext* context) ;

		ProtoObject* asObject(ProtoContext* context) ;
	};

	class ProtoTuple
	{
	public:
		ProtoObject* getAt(ProtoContext* context, int index) ;
		ProtoObject* getFirst(ProtoContext* context) ;
		ProtoObject* getLast(ProtoContext* context) ;
		ProtoObject* getSlice(ProtoContext* context, int from, int to) ;
		unsigned long getSize(ProtoContext* context) ;

		bool has(ProtoContext* context, ProtoObject* value) ;
		ProtoObject* setAt(ProtoContext* context, int index, ProtoObject* value) ;
		ProtoObject* insertAt(ProtoContext* context, int index, ProtoObject* value) ;

		ProtoObject* appendFirst(ProtoContext* context, ProtoTuple* otherTuple) ;
		ProtoObject* appendLast(ProtoContext* context, ProtoTuple* otherTuple) ;

		ProtoObject* splitFirst(ProtoContext* context, int count) ;
		ProtoObject* splitLast(ProtoContext* context, int count) ;

		ProtoObject* removeFirst(ProtoContext* context, int count) ;
		ProtoObject* removeLast(ProtoContext* context, int count) ;
		ProtoObject* removeAt(ProtoContext* context, int index) ;
		ProtoObject* removeSlice(ProtoContext* context, int from, int to) ;

		ProtoList* asList(ProtoContext* context) ;
		ProtoObject* asObject(ProtoContext* context) ;
		unsigned long getHash(ProtoContext* context) ;
		ProtoTupleIterator* getIterator(ProtoContext* context) ;
	};

	class ProtoStringIterator
	{
	public:
		~ProtoStringIterator() = default;
		int hasNext(ProtoContext* context) ;
		ProtoObject* next(ProtoContext* context) ;
		ProtoStringIterator* advance(ProtoContext* context) ;

		ProtoObject* asObject(ProtoContext* context) ;
	};

	class ProtoString
	{
	public:
		~ProtoString() = default;
		int cmp_to_string(ProtoContext* context, ProtoString* otherString);

		ProtoObject* getAt(ProtoContext* context, int index) ;
		ProtoString* setAt(ProtoContext* context, int index, ProtoObject* character) ;
		ProtoString* insertAt(ProtoContext* context, int index, ProtoObject* character) ;
		unsigned long getSize(ProtoContext* context) ;
		ProtoString* getSlice(ProtoContext* context, int from, int to) ;

		ProtoString* setAtString(ProtoContext* context, int index, ProtoString* otherString) ;
		ProtoString* insertAtString(ProtoContext* context, int index, ProtoString* otherString) ;

		ProtoString* appendFirst(ProtoContext* context, ProtoString* otherString) ;
		ProtoString* appendLast(ProtoContext* context, ProtoString* otherString) ;

		ProtoString* splitFirst(ProtoContext* context, int count) ;
		ProtoString* splitLast(ProtoContext* context, int count) ;

		ProtoString* removeFirst(ProtoContext* context, int count) ;
		ProtoString* removeLast(ProtoContext* context, int count) ;
		ProtoString* removeAt(ProtoContext* context, int index) ;
		ProtoString* removeSlice(ProtoContext* context, int from, int to) ;

		ProtoObject* asObject(ProtoContext* context) ;
		ProtoList* asList(ProtoContext* context) ;
		unsigned long getHash(ProtoContext* context) ;
		ProtoStringIterator* getIterator(ProtoContext* context) ;
	};

	class ProtoSparseListIterator
	{
	public:
		~ProtoSparseListIterator() = default;
		int hasNext(ProtoContext* context) ;
		unsigned long nextKey(ProtoContext* context) ;
		ProtoObject* nextValue(ProtoContext* context) ;
		ProtoSparseListIterator* advance(ProtoContext* context) ;

		ProtoObject* asObject(ProtoContext* context) ;
		void finalize(ProtoContext* context) ;
	};

	class ProtoSparseList
	{
	public:
		~ProtoSparseList() = default;
		bool has(ProtoContext* context, unsigned long index) ;
		ProtoObject* getAt(ProtoContext* context, unsigned long index) ;
		ProtoSparseList* setAt(ProtoContext* context, unsigned long index, ProtoObject* value) ;
		ProtoSparseList* removeAt(ProtoContext* context, unsigned long index) ;
		bool isEqual(ProtoContext* context, ProtoSparseList* otherDict) ;

		unsigned long getSize(ProtoContext* context) ;
		ProtoObject* asObject(ProtoContext* context) ;
		unsigned long getHash(ProtoContext* context) ;
		ProtoSparseListIterator* getIterator(ProtoContext* context) ;

		void processElements(
			ProtoContext* context,
			void* self,
			void (*method)(
				ProtoContext* context,
				void* self,
				unsigned long index,
				ProtoObject* value
			)
		) ;

		void processValues(
			ProtoContext* context,
			void* self,
			void (*method)(
				ProtoContext* context,
				void* self,
				ProtoObject* value
			)
		) ;
	};

	class ProtoByteBuffer
	{
	public:
		~ProtoByteBuffer() = default;
		unsigned long getSize(ProtoContext* context) ;
		char* getBuffer(ProtoContext* context) ;
		char getAt(ProtoContext* context, int index) ;
		void setAt(ProtoContext* context, int index, char value) ;

		ProtoObject* asObject(ProtoContext* context) ;
		unsigned long getHash(ProtoContext* context) ;
	};

	class ProtoExternalPointer
	{
	public:
		~ProtoExternalPointer() = default;
		void* getPointer(ProtoContext* context) ;
		ProtoObject* asObject(ProtoContext* context) ;
		unsigned long getHash(ProtoContext* context) ;
	};

	/**
	 * @brief Recommended structure for compiling a method.
	 *
	 * This comment block illustrates the recommended C++ structure for implementing
	 * a Proto method, including argument parsing and handling `super` calls.
	 *
	 * @example
	 * // Pseudo-code for the target method:
	 * // method(self, parent, p1, p2, p3, p4=init4, p5=init5) {
	 * //     super().method()
	 * // }
	 *
	 * // C++ implementation:
	 * ProtoObject* my_method(
	 *     ProtoContext* previousContext,
	 *     ParentLink* parent,
	 *     ProtoList* positionalParameters,
	 *     ProtoSparseList* keywordParameters
	 * ) {
	 *     // 1. Define literals for keyword argument names.
	 *     ProtoString* literalForP4 = context->fromUTF8String("p4");
	 *     ProtoString* literalForP5 = context->fromUTF8String("p5");
	 *
	 *     // 2. Define constants for default parameter values.
	 *     ProtoObject* constantForInit4 = context->fromInteger(4);
	 *     ProtoObject* constantForInit5 = context->fromInteger(5);
	 *
	 *     // 3. Set up the local stack frame and a new context.
	 *     struct {
	 *         ProtoObject *p1, *p2, *p3, *p4, *p5, *local1, *local2;
	 *     } locals;
	 *     ProtoContext context(previousContext, &locals, sizeof(locals) / sizeof(ProtoObject*));
	 *
	 *     // 4. Initialize parameters with default values.
	 *     locals.p4 = constantForInit4;
	 *     locals.p5 = constantForInit5;
	 *
	 *     // 5. Parse positional arguments.
	 *     if (positionalParameters) {
	 *         int pos_count = positionalParameters->getSize(&context);
	 *         if (pos_count < 3) {
	 *             // raise error: "Too few positional arguments."
	 *         }
	 *         locals.p1 = positionalParameters->getAt(&context, 0);
	 *         locals.p2 = positionalParameters->getAt(&context, 1);
	 *         locals.p3 = positionalParameters->getAt(&context, 2);
	 *         if (pos_count > 3) locals.p4 = positionalParameters->getAt(&context, 3);
	 *         if (pos_count > 4) locals.p5 = positionalParameters->getAt(&context, 4);
	 *         if (pos_count > 5) {
	 *             // raise error: "Too many positional arguments."
	 *         }
	 *     } else {
	 *         // raise error: "Expected at least 3 positional arguments."
	 *     }
	 *
	 *     // 6. Parse keyword arguments, checking for duplicates.
	 *     if (keywordParameters) {
	 *         if (keywordParameters->has(&context, literalForP4->getHash(&context))) {
	 *             if (positionalParameters && positionalParameters->getSize(&context) > 3) {
	 *                 // raise error: "Multiple values for argument 'p4'."
	 *             }
	 *             locals.p4 = keywordParameters->getAt(&context, literalForP4->getHash(&context));
	 *         }
	 *         // ... and so on for p5 ...
	 *     }
	 *
	 *     // 7. Handle a `super` call.
	 *     ParentLink* nextParent = parent;
	 *     if (nextParent) {
	 *         nextParent->getObject(&context)->call(
	 *             &context,
	 *             nextParent->getParent(&context), // The next link in the chain
	 *             this_method_name, // A ProtoString for the current method name
	 *             positionalParameters,
	 *             keywordParameters
	 *         );
	 *     } else {
	 *         // raise error: "super() called but no parent exists."
	 *     }
	 *
	 *     // Note: Unused keyword arguments are not detected, similar to Python's behavior.
	 *     // This structure can be auto-generated from compile-time information.
	 *     // Exception handling (e.g., try/catch) is optional.
	 * }
	 */

	class ProtoMethodCell
	{
	public:
		~ProtoMethodCell() = default;
		ProtoObject* getSelf(ProtoContext* context) ;
		ProtoMethod getMethod(ProtoContext* context) ;

		ProtoObject* asObject(ProtoContext* context) ;
		unsigned long getHash(ProtoContext* context) ;

		ProtoMethod method{};
		ProtoObject* self{};
	};

	class ProtoObjectCell
	{
	public:
		~ProtoObjectCell() = default;
		ProtoObjectCell* addParent(ProtoContext* context, ProtoObjectCell* object) ;

		ProtoObject* asObject(ProtoContext* context) ;
		unsigned long getHash(ProtoContext* context) ;

		unsigned long mutable_ref{};
		ParentLink* parent{};
		ProtoSparseList* attributes{};
	};

	class ProtoThread
	{
	public:
		~ProtoThread() = default;
		static ProtoThread* getCurrentThread(ProtoContext* context);

		

		void detach(ProtoContext* context) ;
		void join(ProtoContext* context) ;
		void exit(ProtoContext* context) ; // Must only be called by the current thread.

		ProtoObject* getName(ProtoContext* context) ;
		ProtoObject* asObject(ProtoContext* context) ;
		unsigned long getHash(ProtoContext* context) ;
		void setCurrentContext(ProtoContext* context) ;
		void setManaged() ;
		void setUnmanaged() ;
		void synchToGC() ;
	};

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

		ProtoContext* previous;
		ProtoSpace* space;
		ProtoThread* thread;
		ProtoObject** localsBase;
		unsigned int localsCount;

		void checkCellsCount();
		void setReturnValue(ProtoContext* context, ProtoObject* returnValue);
		void addCell2Context(Cell* newCell);

		// Factory methods for primitive types.
		ProtoObject* fromInteger(int value);
		ProtoObject* fromDouble(double value);
		ProtoObject* fromUTF8Char(const char* utf8OneCharString);
		ProtoString* fromUTF8String(const char* zeroTerminatedUtf8String);
		ProtoMethodCell* fromMethod(ProtoObject* self, ProtoMethod method);
		ProtoExternalPointer* fromExternalPointer(void* pointer);
		ProtoByteBuffer* fromBuffer(unsigned long length, char* buffer);
		ProtoByteBuffer* newBuffer(unsigned long length);
		ProtoObject* fromBoolean(bool value);
		ProtoObject* fromByte(char c);
		ProtoObject* fromDate(unsigned year, unsigned month, unsigned day);
		ProtoObject* fromTimestamp(unsigned long timestamp);
		ProtoObject* fromTimeDelta(long timedelta);

		ProtoObject* fromThread(ProtoThread* thread);
		ProtoObject* fromList(ProtoList* list);
		ProtoObject* fromTuple(ProtoTuple* tuple);
		ProtoObject* fromSparseList(ProtoSparseList* sparseList);
		ProtoObject* fromString(ProtoString* string);
		ProtoObject* fromMethodCell(ProtoMethodCell* methodCell);
		ProtoObject* fromObjectCell(ProtoObjectCell* objectCell);
		ProtoObject* fromObject(ProtoObject* object);
		ProtoObject* fromPointer(void* pointer);
		ProtoObject* fromMethod(ProtoMethod method);

		ProtoList* newList();
		ProtoTuple* newTuple();
		ProtoTuple* newTupleFromList(ProtoList* sourceList);
		ProtoSparseList* newSparseList();
		ProtoObject* newObject(bool mutableObject = false);

		Cell* allocCell();

		Cell* lastAllocatedCell;
		unsigned int allocatedCellsCount;
		ProtoObject* lastReturnValue;
	};

	class ProtoSpace
	{
	public:
		explicit ProtoSpace(
			ProtoMethod mainFunction,
			int argc ,
			char** argv = nullptr
		);
		~ProtoSpace();

		ProtoObject* getThreads(ProtoContext *c);

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

		ProtoObject* listPrototype;
		ProtoObject* listIteratorPrototype;

		ProtoObject* tuplePrototype;
		ProtoObject* tupleIteratorPrototype;

		ProtoObject* stringPrototype;
		ProtoObject* stringIteratorPrototype;

		ProtoObject* sparseListPrototype;
		ProtoObject* sparseListIteratorPrototype;

		ProtoString* literalGetAttribute;
		ProtoString* literalSetAttribute;
		ProtoString* literalCallMethod;

		Cell* getFreeCells(ProtoThread* currentThread);
		void analyzeUsedCells(Cell* cellsChain);
		void triggerGC();
		void allocThread(ProtoContext* context, ProtoThread* thread);
		void deallocThread(ProtoContext* context, ProtoThread* thread);

		/**
		 * @brief Creates and starts a new managed thread within this ProtoSpace.
		 * @param context The current ProtoContext from which the thread is being created.
		 * @param name A ProtoString representing the name of the new thread.
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

		ProtoSparseList* threads;

		Cell* freeCells;
		DirtySegment* dirtySegments;
		int state;

		unsigned int maxAllocatedCellsPerContext;
		int blocksPerAllocation;
		int heapSize;
		int maxHeapSize;
		int freeCellsCount;
		unsigned int gcSleepMilliseconds;
		int blockOnNoMemory;

		std::atomic<TupleDictionary*> tupleRoot;
		std::atomic<ProtoSparseList*> mutableRoot;
		std::atomic<bool> mutableLock;
		std::atomic<bool> threadsLock;
		std::atomic<bool> gcLock;
		std::thread::id mainThreadId;
		std::thread* gcThread;
		std::condition_variable stopTheWorldCV;
		std::condition_variable restartTheWorldCV;
		std::condition_variable gcCV;
		int gcStarted;

		static std::mutex globalMutex;
	};

}

#endif /* PROTO_H_ */