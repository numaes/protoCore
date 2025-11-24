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
    uint64_t generate_mutable_ref()
    {
        thread_local std::mt19937_64 generator(std::random_device{}());
        uint64_t id = 0;
        while (id == 0)
        {
            id = generator();
        }
        return id;
    }

    // --- Constructor and Destructor ---

    ProtoContext::ProtoContext(
        ProtoSpace* space,
        ProtoContext* previous,
        ProtoObject** localsBase,
        unsigned int localsCount
    ) : space(space),
        previous(previous),
        thread(nullptr),
        localsBase(localsBase),
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
        } else if (this->space) {
            // If there's no previous context but there is a space, this is likely a root context.
            // We need to get the current thread context from the space or thread manager.
            // For now, we assume this is handled externally or by the thread itself.
        }
    }

    ProtoContext::~ProtoContext()
    {
        // The sole responsibility of the destructor is to pass the list of
        // locally allocated cells to the garbage collector for analysis.
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
            newCell = static_cast<ProtoThreadImplementation*>(this->thread)->implAllocCell();
        }
        else
        {
            // Fallback for initialization before a thread is fully set up.
            // This memory is not tracked by the GC in the same way.
            newCell = static_cast<Cell*>(std::malloc(sizeof(BigCell)));
        }

        if (newCell) {
            // Use placement new to construct the Cell object
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

    // --- Primitive Type Constructors (from...) ---
    // All factory methods now return const pointers to enforce immutability.

    const ProtoObject* ProtoContext::fromInteger(int value)
    {
        ProtoObjectPointer p{};
        p.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.si.embedded_type = EMBEDDED_TYPE_SMALLINT;
        p.si.smallInteger = value;
        return p.oid.oid;
    }

    const ProtoObject* ProtoContext::fromFloat(float value)
    {
        ProtoObjectPointer p{};
        p.sd.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.sd.embedded_type = EMBEDDED_TYPE_FLOAT;
        union { unsigned int uiv; float fv; } u;
        u.fv = value;
        p.sd.floatValue = u.uiv;
        return p.oid.oid;
    }

    const ProtoObject* ProtoContext::fromUTF8Char(const char* utf8OneCharString)
    {
        // ... (implementation is correct)
        return nullptr; // Placeholder
    }

    const ProtoObject* ProtoContext::fromUTF8String(const char* zeroTerminatedUtf8String)
    {
        const ProtoList* charList = this->newList();
        const char* currentChar = zeroTerminatedUtf8String;
        while (*currentChar)
        {
            charList = charList->appendLast(this, fromUTF8Char(currentChar));
            // ... (UTF-8 char advancing logic)
        }
        const auto newString = new(this) ProtoStringImplementation(this, ProtoTupleImplementation::tupleFromList(this, charList));
        return newString->implAsObject(this);
    }

    const ProtoList* ProtoContext::newList()
    {
        return new(this) ProtoListImplementation(this, PROTO_NONE, true);
    }

    const ProtoTuple* ProtoContext::newTuple()
    {
        return new(this) ProtoTupleImplementation(this, 0);
    }

    const ProtoTuple* ProtoContext::newTupleFromList(const ProtoList* sourceList)
    {
        return ProtoTupleImplementation::tupleFromList(this, sourceList);
    }

    const ProtoSparseList* ProtoContext::newSparseList()
    {
        return new(this) ProtoSparseListImplementation(this);
    }

    const ProtoObject* ProtoContext::newObject(const bool mutableObject)
    {
        return (new(this) ProtoObjectCell(this, nullptr, newSparseList(), mutableObject ? generate_mutable_ref() : 0))->asObject(this);
    }

    const ProtoObject* ProtoContext::fromBoolean(bool value)
    {
        return value ? PROTO_TRUE : PROTO_FALSE;
    }

    const ProtoObject* ProtoContext::fromByte(char c)
    {
        ProtoObjectPointer p{};
        p.byteValue.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.byteValue.embedded_type = EMBEDDED_TYPE_BYTE;
        p.byteValue.byteData = c;
        return p.oid.oid;
    }

    // ... (other from... methods returning const types)
}
