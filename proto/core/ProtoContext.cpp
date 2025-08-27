/*
 * ProtoContext.cpp
 *
 *  Created on: 2020-5-1
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

#include <thread>
#include <cstdlib>   // For std::malloc
#include <algorithm> // For std::max
#include <random>

namespace proto
{
    uint64_t generate_mutable_ref()
    {
        // 'thread_local' ensures that each thread has its own number generator,
        // which is crucial for safety in multithreaded environments.
        // It is seeded with std::random_device for high-quality randomness.
        thread_local std::mt19937_64 generator(std::random_device{}());

        uint64_t id = 0;
        // We ensure that the generated ID is never 0,
        // as 0 is reserved for immutable objects.
        while (id == 0)
        {
            id = generator();
        }
        return id;
    }

    // WARNING: These global variables can cause issues in a multithreaded
    // environment and make state reasoning difficult. Consider encapsulating them
    // within the ProtoSpace class.
    std::atomic<bool> literalMutex(false);
    BigCell* literalFreeCells = nullptr;
    unsigned literalFreeCellsIndex = 0;
    ProtoContext globalContext;

    // --- Constructor and Destructor ---

    ProtoContext::ProtoContext(
        ProtoContext* previous,
        ProtoObject** localsBase,
        unsigned int localsCount,
        ProtoThread* thread,
        ProtoSpace* space
    ) : previous(previous),
        space(space),
        thread(thread),
        localsBase(localsBase),
        localsCount(localsCount),
        lastAllocatedCell(nullptr),
        allocatedCellsCount(0),
        lastReturnValue(nullptr)
    {
        if (previous)
        {
            // If there is a previous context, inherit its space and thread.
            this->space = previous->space;
            this->thread = previous->thread;
        }

        if (this->thread)
        {
            // Update the thread's current context through a public method.
            this->thread->setCurrentContext(this);
        }

        if (this->localsBase)
        {
            // CRITICAL FIX: Loop corrected to prevent buffer overflow.
            // The original loop (i <= localsCount) wrote one element too many.
            for (unsigned int i = 0; i < this->localsCount; ++i)
            {
                this->localsBase[i] = nullptr;
            }
        }
    }


    ProtoContext::~ProtoContext()
    {
        if (this->previous && this->space && this->lastAllocatedCell)
        {
            // When a context is destroyed, the space is informed about the cells
            // that were allocated in it so the GC can analyze them.
            this->space->analyzeUsedCells(this->lastAllocatedCell);
        }
    }

    // --- Cell and GC Management ---

    void ProtoContext::setReturnValue(ProtoContext* context, ProtoObject* returnValue)
    {
        if (this->previous)
        {
            // Ensures that the return value is kept as a valid reference
            // when this context is destroyed, by assigning it to the previous context.
            this->previous->lastReturnValue = returnValue;
        }
    }

    void ProtoContext::checkCellsCount()
    {
        if (this->allocatedCellsCount >= this->space->maxAllocatedCellsPerContext)
        {
            this->space->analyzeUsedCells(this->lastAllocatedCell);
            this->lastAllocatedCell = nullptr;
            this->allocatedCellsCount = 0;
            this->space->triggerGC();
        }
    }

    Cell* ProtoContext::allocCell()
    {
        Cell* newCell;
        if (this->thread)
        {
            newCell = ((ProtoThreadImplementation*)(this->thread))->implAllocCell();
            ::new(newCell) Cell(this);
            newCell = static_cast<Cell*>(newCell);
            this->allocatedCellsCount++;
            this->checkCellsCount();
        }
        else
        {
            // WARNING: This branch uses malloc directly, which bypasses the GC.
            // This is likely a remnant of old code and could be a source of memory leaks.
            // All cell allocations should go through the space's memory manager.
            void* newChunk = std::malloc(sizeof(BigCell));
            ::new(newChunk) Cell(this);
            newCell = static_cast<Cell*>(newChunk);
        }

        // The cells are chained in a simple list for tracking within the context.
        newCell->nextCell = this->lastAllocatedCell;
        this->lastAllocatedCell = newCell;
        return newCell;
    }

    void ProtoContext::addCell2Context(Cell* newCell)
    {
        newCell->nextCell = this->lastAllocatedCell;
        this->lastAllocatedCell = newCell;
    }

    // --- Primitive Type Constructors (from...) ---

    ProtoObject* ProtoContext::fromInteger(int value)
    {
        ProtoObjectPointer p;
        p.oid.oid = nullptr;
        p.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.si.embedded_type = EMBEDDED_TYPE_SMALLINT;
        p.si.smallInteger = value;
        return p.oid.oid;
    }

    ProtoObject* ProtoContext::fromDouble(double value)
    {
        ProtoObjectPointer p;
        p.oid.oid = nullptr;
        p.sd.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.sd.embedded_type = EMBEDDED_TYPE_FLOAT;

        union
        {
            unsigned long lv;
            double dv;
        } u;
        u.dv = value;
        p.sd.floatValue = u.lv >> TYPE_SHIFT;
        return p.oid.oid;
    }

    ProtoObject* ProtoContext::fromUTF8Char(const char* utf8OneCharString)
    {
        ProtoObjectPointer p;
        p.oid.oid = nullptr;
        p.unicodeChar.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.unicodeChar.embedded_type = EMBEDDED_TYPE_UNICODECHAR;

        unsigned unicodeValue = 0U;
        unsigned char firstByte = utf8OneCharString[0];

        if ((firstByte & 0x80) == 0)
        {
            // 1-byte char (ASCII)
            unicodeValue = firstByte;
        }
        else if ((firstByte & 0xE0) == 0xC0)
        {
            // 2-byte char
            unicodeValue = ((firstByte & 0x1F) << 6) | (utf8OneCharString[1] & 0x3F);
        }
        else if ((firstByte & 0xF0) == 0xE0)
        {
            // 3-byte char
            unicodeValue = ((firstByte & 0x0F) << 12) | ((utf8OneCharString[1] & 0x3F) << 6) | (utf8OneCharString[2] &
                0x3F);
        }
        else if ((firstByte & 0xF8) == 0xF0)
        {
            // 4-byte char
            // CRITICAL FIX: A copy-paste error was corrected.
            // The previous version used utf8OneCharString[1] twice.
            unicodeValue = ((firstByte & 0x07) << 18) | ((utf8OneCharString[1] & 0x3F) << 12) | ((utf8OneCharString[2] &
                0x3F) << 6) | (utf8OneCharString[3] & 0x3F);
        }

        p.unicodeChar.unicodeValue = unicodeValue;
        return p.oid.oid;
    }

    ProtoString* ProtoContext::fromUTF8String(const char* zeroTerminatedUtf8String)
    {
        const char* currentChar = zeroTerminatedUtf8String;
        ProtoList* charList = this->newList();

        while (*currentChar)
        {
            charList = (ProtoList*)charList->appendLast(this, this->fromUTF8Char(currentChar));

            // Advance the pointer according to the number of bytes of the UTF-8 character
            if ((*currentChar & 0x80) == 0) currentChar += 1;
            else if ((*currentChar & 0xE0) == 0xC0) currentChar += 2;
            else if ((*currentChar & 0xF0) == 0xE0) currentChar += 3;
            else if ((*currentChar & 0xF8) == 0xF0) currentChar += 4;
            else currentChar += 1; // Invalid character, advance 1 to avoid an infinite loop
        }

        // Creating a string involves converting the list of characters into a tuple.
        return new(this) ProtoStringImplementation(
            this,
            ProtoTupleImplementation::tupleFromList(this, charList)
        );
    }

    // --- Collection Type Constructors (new...) ---

    ProtoList* ProtoContext::newList()
    {
        return new(this) ProtoListImplementation(this);
    }

    ProtoTuple* ProtoContext::newTuple()
    {
        return new(this) ProtoTupleImplementation(this, 0, 0, static_cast<ProtoObject**>(nullptr));
    }

    ProtoTuple* ProtoContext::newTupleFromList(ProtoList* sourceList)
    {
        return ProtoTupleImplementation::tupleFromList(this, sourceList);
    }

    ProtoSparseList* ProtoContext::newSparseList()
    {
        return new(this) ProtoSparseListImplementation(this);
    }

    ProtoObject* ProtoContext::newObject(bool mutableObject)
    {
        return new(this) ProtoObjectCellImplementation(
            this,
            nullptr,
            mutableObject ? generate_mutable_ref() : 0,
            nullptr
        );
    }


    // --- Other Constructors (from...) ---

    ProtoObject* ProtoContext::fromBoolean(bool value)
    {
        ProtoObjectPointer p;
        p.oid.oid = nullptr;
        p.booleanValue.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.booleanValue.embedded_type = EMBEDDED_TYPE_BOOLEAN;
        p.booleanValue.booleanValue = value;
        return p.oid.oid;
    }

    ProtoObject* ProtoContext::fromByte(char c)
    {
        ProtoObjectPointer p;
        p.oid.oid = nullptr;
        p.byteValue.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.byteValue.embedded_type = EMBEDDED_TYPE_BYTE;
        p.byteValue.byteData = c;
        return p.oid.oid;
    }

    ProtoObject* ProtoContext::fromDate(unsigned year, unsigned month, unsigned day)
    {
        ProtoObjectPointer p;
        p.oid.oid = nullptr;
        p.date.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.date.embedded_type = EMBEDDED_TYPE_DATE;
        p.date.year = year;
        p.date.month = month;
        p.date.day = day;
        return p.oid.oid;
    }

    ProtoObject* ProtoContext::fromTimestamp(unsigned long timestamp)
    {
        ProtoObjectPointer p;
        p.oid.oid = nullptr;
        p.timestampValue.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.timestampValue.embedded_type = EMBEDDED_TYPE_TIMESTAMP;
        p.timestampValue.timestamp = timestamp;
        return p.oid.oid;
    }

    ProtoObject* ProtoContext::fromTimeDelta(long timedelta)
    {
        ProtoObjectPointer p;
        p.oid.oid = nullptr;
        p.timedeltaValue.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.timedeltaValue.embedded_type = EMBEDDED_TYPE_TIMEDELTA;
        p.timedeltaValue.timedelta = timedelta;
        return p.oid.oid;
    }

    ProtoMethodCell* ProtoContext::fromMethod(ProtoObject* self, ProtoMethod method)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*) new(this) ProtoMethodCellImplementation(this, method);
        p.op.pointer_tag = POINTER_TAG_METHOD;
        return (ProtoMethodCell*)p.oid.oid;
    }

    ProtoExternalPointer* ProtoContext::fromExternalPointer(void* pointer)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*) new(this) ProtoExternalPointerImplementation(this, pointer);
        p.op.pointer_tag = POINTER_TAG_EXTERNAL_POINTER;
        return (ProtoExternalPointer*)p.oid.oid;
    }

    ProtoByteBuffer* ProtoContext::fromBuffer(unsigned long length, char* buffer)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*) new(this) ProtoByteBufferImplementation(this, length, buffer);
        p.op.pointer_tag = POINTER_TAG_BYTE_BUFFER;
        return (ProtoByteBuffer*)p.oid.oid;
    }

    ProtoByteBuffer* ProtoContext::newBuffer(unsigned long length)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*) new(this) ProtoByteBufferImplementation(this, length, NULL);
        p.op.pointer_tag = POINTER_TAG_BYTE_BUFFER;
        return (ProtoByteBuffer*)p.oid.oid;
    }
} // namespace proto
