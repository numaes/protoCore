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

    class ReturnReference : public Cell {
    public:
        Cell* returnValue;

        ReturnReference(ProtoContext* context, Cell* returnValue) : Cell(context), returnValue(returnValue)
        : Cell(context), returnValue(returnValue) {}

        ~ReturnReference() override {}

        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, const Cell* cell)
        ) const override
        {
            if (this->returnValue)
            {
				ProtoObjectPointer pa{};
        		pa.oid.oid = this->returnValue;
        		if (pa.op.pointer_tag != POINTER_TAG_EMBEDDED_VALUE)
                	method(context, self, this->returnValue);
            }


        }
        const ProtoObject* implAsObject(ProtoContext* context) const override;
    };


    }

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
            // This requires iterating the sparse list, which is complex.
            // For now, we assume a method to process its elements.
            // A full implementation would require a ProtoSparseListIterator.
            // This is a conceptual placeholder.
            /*
            kwargs->processElements(this, nullptr, 
                [&](ProtoContext* ctx, void*, unsigned long key, const ProtoObject* value) {
                    // Find the parameter name by hash
                    bool found = false;
                    for (unsigned int i = 0; i < paramCount; ++i) {
                        const ProtoString* paramName = parameterNames->getAt(ctx, i)->asString(ctx);
                        if (paramName->getHash(ctx) == key) {
                            if (assigned[i]) {
                                throw std::invalid_argument("Parameter assigned twice.");
                            }
                            closureLocals = closureLocals->setAt(ctx, key, value);
                            assigned[i] = true;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        throw std::invalid_argument("Unknown keyword argument provided.");
                    }
                });
            */
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
            auto returnReference = new(this->previous) ProtoReturnReference(this->previous, this->returnValue);
            this->previous->addCell2Context(returnReference);
        }
        // Free the C-style array for automatic variables.
        delete[] automaticLocals;
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

    const ProtoObject* ProtoContext::fromLong(long long value) {
        return Integer::fromLong(this, value);
    }

    const ProtoObject* ProtoContext::fromString(const char* str, int base) {
        return Integer::fromString(this, str, base);
    }

    const ProtoObject* ProtoContext::fromDouble(double value) {
        return (new(this) DoubleImplementation(this, value))->asObject(this);
    }

    const ProtoObject* ProtoContext::fromUTF8Char(const char* utf8OneCharString) {
        ProtoObjectPointer p{};
        union { char asBytes[4]; unsigned int asUnicodeChar; } build_buffer{};
        p.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.si.embedded_type = EMBEDDED_TYPE_UNICODE_CHAR;
        
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
