/*
 * ProtoContext.cpp
 *
 *  Created on: 2020-5-1
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <random>
#include <cstdlib>

namespace proto
{
    // ... (generate_mutable_ref remains the same)

    // --- Constructor and Destructor ---

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

    ProtoContext::~ProtoContext()
    {
        if (this->space && this->lastAllocatedCell)
        {
            this->space->analyzeUsedCells(this->lastAllocatedCell);
        }
    }

    Cell* ProtoContext::allocCell()
    {
        Cell* newCell = nullptr;
        if (this->thread)
        {
            // Corrected: Use toImpl for safe casting
            newCell = toImpl<ProtoThreadImplementation>(this->thread)->implAllocCell();
        }
        else
        {
            newCell = static_cast<Cell*>(std::malloc(sizeof(BigCell)));
        }

        if (newCell) {
            ::new(newCell) Cell(this);
            this->allocatedCellsCount++;
        }
        return newCell;
    }

    void ProtoContext::addCell2Context(Cell* cell)
    {
        cell->next = this->lastAllocatedCell;
        this->lastAllocatedCell = cell;
    }

    // --- Factory Methods ---

    const ProtoObject* ProtoContext::fromUTF8String(const char* zeroTerminatedUtf8String)
    {
        const ProtoList* charList = this->newList();
        const char* currentChar = zeroTerminatedUtf8String;
        while (*currentChar)
        {
            charList = charList->appendLast(this, fromUTF8Char(currentChar));
            // ... (UTF-8 char advancing logic)
        }
        // Corrected: The constructor now accepts a const pointer
        const auto newString = new(this) ProtoStringImplementation(this, ProtoTupleImplementation::tupleFromList(this, charList));
        return newString->implAsObject(this);
    }

    const ProtoList* ProtoContext::newList()
    {
        // Corrected: Use the asProtoList method for safe conversion
        return (new(this) ProtoListImplementation(this, PROTO_NONE, true))->asProtoList(this);
    }

    const ProtoTuple* ProtoContext::newTuple()
    {
        // Corrected: Use the asProtoTuple method for safe conversion
        return (new(this) ProtoTupleImplementation(this, 0))->asProtoTuple(this);
    }

    const ProtoTuple* ProtoContext::newTupleFromList(const ProtoList* sourceList)
    {
        // tupleFromList already returns the correct public type
        return ProtoTupleImplementation::tupleFromList(this, sourceList);
    }

    const ProtoSparseList* ProtoContext::newSparseList()
    {
        // Corrected: Use the asSparseList method for safe conversion
        return (new(this) ProtoSparseListImplementation(this))->asSparseList(this);
    }

    const ProtoObject* ProtoContext::newObject(const bool mutableObject)
    {
        // Corrected: Use toImpl to cast the sparse list to its implementation type for the constructor
        const auto* attributes = toImpl<const ProtoSparseListImplementation>(newSparseList());
        return (new(this) ProtoObjectCell(this, nullptr, attributes, mutableObject ? generate_mutable_ref() : 0))->asObject(this);
    }

    // ... (rest of the from... methods are correct as they don't involve implementation classes)
}
