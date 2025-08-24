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

#include "proto_internal.h"


namespace proto
{
	class ProtoStringIterator;
	class ProtoTupleIterator;

	typedef int BOOLEAN;

	// Usefull constants.
	// ATENTION: They should be kept on synch with proto_internal.h!

#define PROTO_TRUE ((proto::ProtoObject *)  0x010FL)
#define PROTO_FALSE ((proto::ProtoObject *) 0x000FL)
#define PROTO_NONE ((proto::ProtoObject *) NULL)

	// Root base of any internal structure.
	// All Cell objects should be non mutable once initialized
	// They could only change their context and nextCell, assuming that the
	// new nextCell includes the previous chain.
	// Changing the context or the nextCell are the ONLY changes allowed
	// (taking into account the previous restriction).
	// Changes should be atomic
	//
	// Cells should be always smaller or equal to 64 bytes.
	// Been a power of two size has huge advantages and it opens
	// the posibility to extend the model to massive parallel computers
	//
	// Allocations of Cells will be performed in OS page size chunks
	// Page size is allways a power of two and bigger than 64 bytes
	// There is no other form of allocation for proto objects or scalars
	//

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
		                  ProtoList* unnamedParametersList = nullptr,
		                  ProtoSparseList* keywordParametersDict = nullptr);

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


		bool asBoolean(ProtoContext *context);
		int asInteger(ProtoContext *context);
		float asFloat(ProtoContext *context);
		char asByte(ProtoContext *context);
		void asDate(ProtoContext *context, unsigned int& year, unsigned& month, unsigned& day);
		unsigned long asTimestamp(ProtoContext *context);
		long asTimeDelta(ProtoContext *context);

		bool isBoolean(ProtoContext *context);
		bool isInteger(ProtoContext *context);
		bool isFloat(ProtoContext *context);
		bool isByte(ProtoContext *context);
		bool isDate(ProtoContext *context);
		bool isTimestamp(ProtoContext *context);
		bool isTimeDelta(ProtoContext *context);

		ProtoList * asList(ProtoContext *context);
		ProtoListIterator * asListIterator(ProtoContext *context);
		ProtoTuple * asTuple(ProtoContext *context);
		ProtoTupleIterator * asTupleIterator(ProtoContext *context);
		ProtoString * asString(ProtoContext *context);
		ProtoStringIterator * asStringIterator(ProtoContext *context);
		ProtoSparseList * asSparseList(ProtoContext *context);
		ProtoSparseListIterator * asSparseListIterator(ProtoContext *context);

	};


	// ParentPointers are chains of parent classes used to solve attribute access
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
		int isEqual(ProtoContext* context, ProtoSparseList* otherDict) ;

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

	// In order to compile a method the folling structure is recommended:
	// method (self, parent, p1, p2, p3, p4=init4, p5=init5) {
	//	   super().method()
	// }
	//
	// ProtoObject *literalForP4, *literalForP5;
	// ProtoObject *constantForInit4, *constantForInit5;
	//
	// ProtoObject *method(ProtoContext *previousContext, ProtoParent *parent, ProtoList *positionalParameters, ProtoSparseList *keywordParameters)
	//      // Parameters + locals
	//		struct {
	//   		ProtoObject *p1, *p2, *p3, *p4, *p5, *l1, *l2, *l3;
	//		} locals;
	//		ProtoContext context(previousContext, &locals, sizeof(locals) / sizeof(ProtoObject *));
	//
	//		locals.p4 = alreadyInitializedConstantForInit4;
	//		locals.p5 = alreadyInitializedConstantForInit5;
	//
	//      if (positionalParameters) {
	//     		int unnamedSize = positionalParameters->getSize(&context);
	//
	//          if (unnamedSize < 3)
	//    			raise "Too few parameters. At least 3 positional parameters are expected"
	//
	//	    	locals.p1 = positionalParameters->getAt(&context, 0);
	//	    	locals.p2 = positionalParameters->getAt(&context, 1);
	//	    	locals.p3 = positionalParameters->getAt(&context, 2);
	//
	//          if (unnamedSize > 3)
	//			    locals.p4 = positionalParameters->getAt(&context, 3);
	//
	//          if (unnamedSize > 4)
	//	    		locals.p5 = positionalParameters->getAt(&context, 4);
	//
	//		    if (unnamedSize > 5)
	//			    raise "Too many parameters"
	//      }
	//      else
	//          raise "At least 3 positional parameters expected"
	//
	//      if (keywordParameters) {
	//          if (keywordParameters->has(&context, literalForP4)) {
	//	    	    if (unnamedSize > 4)
	//		    	    raise "Double assignment on p4"
	//
	//			    locals.p4 = keywordParameters->getAt(&context, literalForP4);
	//          }
	//
	//          if (keywordParameters->has(&context, literalForP5)) {
	//	    	    if (unnamedSize > 5)
	//		    	    raise "Double assignment on p5"
	//
	//			    locals.p5 = keywordParameters->getAt(&context, literalForP5);
	//          }
	//      }
	//
	//		ProtoParent *nextParent;
	//      if (parent)
	//		    nextParent = parent;
	//
	//      if (nextParent)
	//          nextParent->object->call(this, nextParent->parent, positionalParameters, keywordParameters);
	//      else
	//          raise "There is no super!!"
	//
	//
	//
	// Not used keywordParameters are not detected
	// This provides a similar behaviour to Python, and it can be automatically generated based on compilation time info
	//
	// You can use try ... catch to handle exceptions or not, it's up to you

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
		void exit(ProtoContext* context) ; // ONLY for current thread!!!

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
		__resharper_unknown_type thread;
		ProtoObject** localsBase;
		unsigned int localsCount;

		void checkCellsCount();
		void setReturnValue(ProtoContext* context, ProtoObject* returnValue);
		void addCell2Context(Cell* newCell);

		// Constructors for base types, here to get the right context on invoke
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

		ProtoObject* getThreads();

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