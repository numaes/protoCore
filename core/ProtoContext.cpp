/*
 * ProtoContext.cpp
 *
 *  Created on: 2020-5-1
 *      Author: gamarino
 *
 *  This file implements the ProtoContext, which represents the execution
 *  state and call stack of a single thread.
 */

#include "../headers/proto_internal.h"
#include <random>
#include <cstdlib>

namespace proto
{
    /**
     * @class ProtoContext
     * @brief Represents the execution state of a thread, including the call stack
     *        and memory allocation arena.
     *
     * A ProtoContext is the primary interface for creating new objects. It serves
     * several critical roles:
     * 1.  **Call Stack**: Each context can point to a `previous` context, forming a
     *     linked list that represents the call stack of a `ProtoThread`.
     * 2.  **Memory Management**: It tracks all cells allocated within its scope. When
     *     the context is destroyed (e.g., a function returns), it hands this list
     *     of "young" objects to the `ProtoSpace` for GC analysis. This is a key
     *     part of the implicit generational garbage collection.
     * 3.  **Object Factory**: It provides factory methods (`newList`, `newObject`, etc.)
     *     for creating all types of Proto objects.
     */

    /**
     * @brief Constructs a new execution context.
     * @param space The global `ProtoSpace` this context belongs to.
     * @param previous The parent context in the call stack, or nullptr for a root context.
     * @param localsBase A pointer to an array of local variables for this scope.
     * @param localsCount The number of local variables.
     */
    ProtoContext::ProtoContext(
        ProtoSpace* space,
        ProtoContext* previous,
        ProtoObject** localsBase,
        unsigned int localsCount
    ) : space(space),
        previous(previous),
        thread(nullptr),
        localsBase(localsBase ? localsBase : nullptr),
        localsCount(localsBase ? localsCount : 0),
        lastAllocatedCell(nullptr),
        allocatedCellsCount(0),
        returnValue(PROTO_NONE)
    {
        if (previous)
        {
            this->space = previous->space;
            this->thread = previous->thread;
        }
        
        if (this->thread) {
            this->thread->setCurrentContext(this);
        }
    }

    /**
     * @brief Destructor for the context.
     * When a context goes out of scope, it reports all the cells allocated
     * within it to the global `ProtoSpace` for garbage collection analysis.
     */
    ProtoContext::~ProtoContext()
    {
        if (this->space && this->lastAllocatedCell)
        {
            this->space->analyzeUsedCells(this->lastAllocatedCell);
        }
    }

    /**
     * @brief The core memory allocation function.
     * It attempts to get a new `Cell` from the current thread's local arena.
     * This is a fast, lock-free operation for the common case.
     * @return A pointer to a newly allocated, uninitialized `Cell`.
     */
    Cell* ProtoContext::allocCell()
    {
        Cell* newCell = nullptr;
        if (this->thread)
        {
            newCell = toImpl<ProtoThreadImplementation>(this->thread)->implAllocCell(this);
        }
        else
        {
            // Fallback for contexts not associated with a ProtoThread (e.g., the root context).
            newCell = static_cast<Cell*>(std::malloc(sizeof(BigCell)));
        }

        if (newCell) {
            // Use placement new to construct a base Cell, which links it to this context.
            ::new(newCell) Cell(this);
            this->allocatedCellsCount++;
        }
        return newCell;
    }

    /**
     * @brief Adds a newly constructed cell to this context's tracking list.
     * This is called automatically by the `Cell` constructor.
     * @param cell The cell to add.
     */
    void ProtoContext::addCell2Context(Cell* cell)
    {
        cell->next = this->lastAllocatedCell;
        this->lastAllocatedCell = cell;
    }

    //=========================================================================
    // Factory Methods
    //=========================================================================

    const ProtoObject* ProtoContext::fromUTF8String(const char* zeroTerminatedUtf8String)
    {
        const ProtoList* charList = this->newList();
        const char* currentChar = zeroTerminatedUtf8String;
        while (*currentChar)
        {
            charList = charList->appendLast(this, fromUTF8Char(currentChar));
            // This logic should be improved to correctly advance the pointer
            // based on the number of bytes in the UTF-8 character.
            currentChar++;
        }
        const auto newString = new(this) ProtoStringImplementation(this, ProtoTupleImplementation::tupleFromList(this, charList));
        return newString->implAsObject(this);
    }

    const ProtoList* ProtoContext::newList()
    {
        return (new(this) ProtoListImplementation(this, PROTO_NONE, true))->asProtoList(this);
    }

    const ProtoTuple* ProtoContext::newTuple()
    {
        return (new(this) ProtoTupleImplementation(this, 0, 0))->asProtoTuple(this);
    }

    const ProtoTuple* ProtoContext::newTupleFromList(const ProtoList* sourceList)
    {
        return ProtoTupleImplementation::tupleFromList(this, sourceList)->asProtoTuple(this);
    }

    const ProtoSparseList* ProtoContext::newSparseList()
    {
        return (new(this) ProtoSparseListImplementation(this))->asSparseList(this);
    }

    const ProtoObject* ProtoContext::newObject(const bool mutableObject)
    {
        const auto* attributes = toImpl<const ProtoSparseListImplementation>(newSparseList());
        return (new(this) ProtoObjectCell(this, nullptr, attributes, mutableObject ? generate_mutable_ref() : 0))->asObject(this);
    }

    const ProtoObject* ProtoContext::fromInteger(int value) {
        ProtoObjectPointer p{};
        p.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.si.embedded_type = EMBEDDED_TYPE_SMALLINT;
        p.si.smallInteger = value;
        return p.oid.oid;
    }

    const ProtoObject* ProtoContext::fromUTF8Char(const char* utf8OneCharString) {
        ProtoObjectPointer p{};
        union
        {
            char asBytes[4];
            unsigned int asUnicodeChar;
        } build_buffer{};

        p.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.si.embedded_type = EMBEDDED_TYPE_UNICODE_CHAR;
        
        // This logic correctly handles multi-byte UTF-8 characters.
        const unsigned char* c = (const unsigned char*)utf8OneCharString;
        int len = 0;
        if (c[0] < 0x80) len = 1;
        else if ((c[0] & 0xE0) == 0xC0) len = 2;
        else if ((c[0] & 0xF0) == 0xE0) len = 3;
        else if ((c[0] & 0xF8) == 0xF0) len = 4;

        for (int i = 0; i < len && i < 4; ++i) {
            build_buffer.asBytes[i] = c[i];
        }
        
        p.unicodeChar.unicodeValue = build_buffer.asUnicodeChar;
        return p.oid.oid;
    }

} // namespace proto
