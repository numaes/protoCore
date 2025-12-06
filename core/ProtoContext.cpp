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
#include <stdexcept>
#include <vector>

namespace proto
{

    /**
     * @class ProtoContext
     * @brief Represents the execution state of a thread, including the call stack
     *        and memory allocation arena.
     */

    /**
     * @brief Constructs a new execution context for a function call.
     * This is the core of the function execution model. It allocates local variable
     * storage and performs argument-to-parameter binding, throwing exceptions on errors.
     */
    ProtoContext::ProtoContext(
        ProtoSpace* space,
        ProtoContext* previous,
        const ProtoList* parameterNames,
        const ProtoList* localNames,
        const ProtoList* args,
        const ProtoSparseList* kwargs
    ) : previous(previous),
        space(space),
        thread(nullptr),
        closureLocals(nullptr),
        automaticLocals(nullptr),
        automaticLocalsCount(0),
        lastAllocatedCell(nullptr),
        allocatedCellsCount(0),
        returnValue(PROTO_NONE)
    {
        // Step 1: Inherit essential services from the parent context.
        // This must be done first so that this context can allocate objects.
        if (previous) {
            this->space = previous->space;
            this->thread = previous->thread;
        } else {
            // This is the root context, it doesn't have a parent thread yet.
            this->thread = nullptr;
        }
        if (this->thread) {
            this->thread->setCurrentContext(this);
        }

        // Step 2: Allocate storage for local variables.
        if (localNames) {
            automaticLocalsCount = localNames->getSize(this);
            if (automaticLocalsCount > 0) {
                automaticLocals = new const ProtoObject*[automaticLocalsCount];
                for (unsigned int i = 0; i < automaticLocalsCount; ++i) {
                    automaticLocals[i] = PROTO_NONE;
                }
            }
        }
        closureLocals = this->newSparseList();

        // Step 3: Argument to Parameter Binding
        if (!parameterNames) return; // Nothing more to do if there are no parameters.

        const unsigned int paramCount = parameterNames->getSize(this);
        const unsigned int argCount = args ? args->getSize(this) : 0;

        // 3a. Too many positional arguments
        if (argCount > paramCount) {
            throw std::invalid_argument("Too many positional arguments provided.");
        }

        std::vector<bool> assigned(paramCount, false);

        // 3b. Bind positional arguments
        for (unsigned int i = 0; i < argCount; ++i) {
            const ProtoString* paramName = parameterNames->getAt(this, i)->asString(this);
            const ProtoObject* value = args->getAt(this, i);
            closureLocals = closureLocals->setAt(this, paramName->getHash(this), value);
            assigned[i] = true;
        }

        // 3c. Bind keyword arguments
        if (kwargs) {
            const ProtoSparseListIterator* iterator = kwargs->getIterator(this);
            while (iterator->hasNext(this)) {
                unsigned long key = iterator->nextKey(this);
                const ProtoObject* value = iterator->nextValue(this);

                bool found = false;
                for (unsigned int i = 0; i < paramCount; ++i) {
                    const ProtoString* paramName = parameterNames->getAt(this, i)->asString(this);
                    if (paramName->getHash(this) == key) {
                        if (assigned[i]) {
							if (this->space->parameterTwiceAssignedCallback)
								this->space->parameterTwiceAssignedCallback(this, nullptr, paramName);
							continue;
                        }
                        closureLocals = closureLocals->setAt(this, key, value);
                        assigned[i] = true;
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    // To get the name for the error message, we would need a reverse mapping
                    // from hash to string, which is complex. We'll give a generic error.
					if (this->space->parameterNotFoundCallback)
						this->space->parameterNotFoundCallback(this, nullptr, nullptr);
                }
                iterator = const_cast<ProtoSparseListIterator*>(iterator)->advance(this);
            }
        }
    }

    /**
     * @brief Destructor for the context.
     * Reports allocated cells to the GC and frees automatic local variable storage.
     */
    ProtoContext::~ProtoContext()
    {
        if (this->space && this->lastAllocatedCell) {
            this->space->submitYoungGeneration(this->lastAllocatedCell);
        }

        if (this->returnValue && this->previous)
        {
            auto returnReference = new(this->previous) ReturnReference(this->previous, (Cell*)this->returnValue);
            this->previous->addCell2Context(returnReference);
        }
        // Free the C-style array for automatic variables.
        delete[] automaticLocals;
    }

    /**
     * @brief The core memory allocation function.
     */
    Cell* ProtoContext::allocCell()
    {
        Cell* newCell = nullptr;
        if (this->thread) {
            newCell = toImpl<ProtoThreadImplementation>(this->thread)->implAllocCell(this);
        } else {
            newCell = static_cast<Cell*>(std::malloc(sizeof(BigCell)));
        }

        if (newCell) {
            ::new(newCell) Cell(this);
            this->allocatedCellsCount++;
        }
        return newCell;
    }

    /**
     * @brief Adds a newly constructed cell to this context's tracking list.
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
        const unsigned char* s = (const unsigned char*)zeroTerminatedUtf8String;
        while (*s) {
            unsigned int unicodeChar;
            int len;
            if (*s < 0x80) {
                unicodeChar = *s;
                len = 1;
            } else if ((*s & 0xE0) == 0xC0) {
                unicodeChar = *s & 0x1F;
                len = 2;
            } else if ((*s & 0xF0) == 0xE0) {
                unicodeChar = *s & 0x0F;
                len = 3;
            } else { // Assuming 4-byte character
                unicodeChar = *s & 0x07;
                len = 4;
            }

            for (int i = 1; i < len; ++i) {
                if (s[i] == '\0' || (s[i] & 0xC0) != 0x80) {
                    unicodeChar = *s;
                    len = 1;
                    break;
                }
                unicodeChar = (unicodeChar << 6) | (s[i] & 0x3F);
            }
            charList = charList->appendLast(this, fromUnicodeChar(unicodeChar));
            s += len;
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

    const ProtoSet* ProtoContext::newSet()
    {
        return (new(this) ProtoSetImplementation(this, newSparseList(), 0))->asProtoSet(this);
    }

    const ProtoMultiset* ProtoContext::newMultiset()
    {
        return (new(this) ProtoMultisetImplementation(this, newSparseList(), 0))->asProtoMultiset(this);
    }

    const ProtoObject* ProtoContext::newObject(const bool mutableObject)
    {
        const auto* attributes = toImpl<const ProtoSparseListImplementation>(newSparseList());
        return (new(this) ProtoObjectCell(this, nullptr, attributes, mutableObject ? generate_mutable_ref() : 0))->asObject(this);
    }

    const ProtoObject* ProtoContext::fromBoolean(bool value) {
        return value ? PROTO_TRUE : PROTO_FALSE;
    }

    const ProtoObject* ProtoContext::fromByte(char c) {
        return fromInteger(c);
    }

    const ProtoObject* ProtoContext::fromInteger(int value) {
        ProtoObjectPointer p{};
        p.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.si.embedded_type = EMBEDDED_TYPE_SMALLINT;
        p.si.smallInteger = value;
        return p.oid.oid;
    }

    const ProtoObject* ProtoContext::fromLong(long long value) {
        return Integer::fromLong(this, value);
    }

    const ProtoObject* ProtoContext::fromString(const char* str, int base) {
        return Integer::fromString(this, str, base);
    }

    const ProtoObject* ProtoContext::fromDouble(double value) {
        return (new(this) DoubleImplementation(this, value))->implAsObject(this);
    }

    const ProtoObject* ProtoContext::fromUnicodeChar(unsigned int unicodeChar) {
        ProtoObjectPointer p{};
        p.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.si.embedded_type = EMBEDDED_TYPE_UNICODE_CHAR;
        p.unicodeChar.unicodeValue = unicodeChar;
        return p.oid.oid;
    }

    const ProtoObject* ReturnReference::implAsObject(ProtoContext* context) const
    {
        // A ReturnReference should not be exposed as a user-facing object.
        return PROTO_NONE;
    }

} // namespace proto
