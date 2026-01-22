/*
 * ProtoSpace.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <iostream>
#include <cstdlib>
#include <iostream>

namespace proto {

    namespace {
        void gcThreadLoop(ProtoSpace* space) {
            while (space->state != SPACE_STATE_ENDING) {
                // GC logic goes here
            }
        }
    }

    ProtoSpace::ProtoSpace() :
        state(SPACE_STATE_RUNNING),
        gcThread(std::make_unique<std::thread>(gcThreadLoop, this)),
        booleanPrototype(nullptr),
        unicodeCharPrototype(nullptr),
        listPrototype(nullptr),
        sparseListPrototype(nullptr),
        tuplePrototype(nullptr),
        stringPrototype(nullptr),
        setPrototype(nullptr),
        multisetPrototype(nullptr),
        attributeNotFoundGetCallback(nullptr),
        nonMethodCallback(nullptr),
        parameterTwiceAssignedCallback(nullptr),
        parameterNotFoundCallback(nullptr)
    {
        // Initialize prototypes
        auto* root_context = new ProtoContext(this, nullptr, nullptr, nullptr, nullptr, nullptr);
        this->booleanPrototype = const_cast<ProtoObject*>(root_context->newObject(false));
        this->unicodeCharPrototype = const_cast<ProtoObject*>(root_context->newObject(false));
        this->listPrototype = const_cast<ProtoObject*>(root_context->newObject(false));
        this->sparseListPrototype = const_cast<ProtoObject*>(root_context->newObject(false));
        this->objectPrototype = const_cast<ProtoObject*>(root_context->newObject(false));
        this->tuplePrototype = const_cast<ProtoObject*>(root_context->newObject(false));
        this->stringPrototype = const_cast<ProtoObject*>(root_context->newObject(false));
        this->setPrototype = const_cast<ProtoObject*>(root_context->newObject(false));
        this->multisetPrototype = const_cast<ProtoObject*>(root_context->newObject(false));

        // Initialize all other prototypes to a basic object for now
        // This prevents null dereferences if getPrototype is called on an uninitialized type
        this->smallIntegerPrototype = this->objectPrototype;
        this->largeIntegerPrototype = this->objectPrototype;
        this->floatPrototype = this->objectPrototype; // Assuming float is not yet implemented as distinct from double
        this->bytePrototype = this->objectPrototype;
        this->nonePrototype = this->objectPrototype; // PROTO_NONE is a special constant, but its prototype can be objectPrototype
        this->methodPrototype = this->objectPrototype;
        this->bufferPrototype = this->objectPrototype;
        this->pointerPrototype = this->objectPrototype;
        this->doublePrototype = this->objectPrototype;
        this->datePrototype = this->objectPrototype;
        this->timestampPrototype = this->objectPrototype;
        this->timedeltaPrototype = this->objectPrototype;
        this->threadPrototype = this->objectPrototype;
        this->rootObject = this->objectPrototype; // Root object can be the base object prototype

        this->listIteratorPrototype = this->objectPrototype;
        this->tupleIteratorPrototype = this->objectPrototype;
        this->stringIteratorPrototype = this->objectPrototype;
        this->sparseListIteratorPrototype = this->objectPrototype;
        this->setIteratorPrototype = this->objectPrototype;
        this->multisetIteratorPrototype = this->objectPrototype;

        // Initialize mutableRoot
        auto* emptyRaw = new(root_context) ProtoSparseListImplementation(root_context, 0, PROTO_NONE, nullptr, nullptr, true);
        this->mutableRoot = const_cast<ProtoSparseList*>(emptyRaw->asSparseList(root_context));

        delete root_context;
    }

    ProtoSpace::~ProtoSpace() {
        this->state = SPACE_STATE_ENDING;
        if (gcThread->joinable()) {
            gcThread->join();
        }
    }

    const ProtoThread* ProtoSpace::newThread(
        ProtoContext* context,
        const ProtoString* name,
        ProtoMethod mainFunction,
        const ProtoList* args,
        const ProtoSparseList* kwargs
    ) {
        auto* c = context ? context : new ProtoContext(this, nullptr, nullptr, nullptr, args, kwargs);
        auto* newThreadImpl = new(c) ProtoThreadImplementation(c, name, this, mainFunction, args, kwargs);
        return newThreadImpl->asThread(c);
    }

    Cell* ProtoSpace::getFreeCells(const ProtoThread* thread) {
        // Temporary implementation. Just get 1024 cells
        // TODO: interact with the garbage collector
        Cell* newCell = nullptr;
        // Intentar asignar memoria alineada a 64 bytes
        int result = posix_memalign(reinterpret_cast<void**>(&newCell), 64, 1024 * sizeof(BigCell));
        if (result != 0) {
            // posix_memalign devuelve un código de error si falla (por ejemplo, ENOMEM si no hay memoria)
            if (this->outOfMemoryCallback)
                (this->outOfMemoryCallback(nullptr));
            std::cerr << "NO MORE MEMORY!!: " << result << std::endl;
            exit(-1);
        }

        Cell* previous = nullptr;
        BigCell* bigCellPtr = reinterpret_cast<BigCell*>(newCell);
        for (int i = 0; i < 1024; ++i)
        {
            Cell* current = reinterpret_cast<Cell*>(&bigCellPtr[i]);
            current->next = previous;
            previous = current;
        }
        return previous;
    }

    void ProtoSpace::submitYoungGeneration(const Cell* cell) {
        // This is a placeholder for a real implementation.
    }

    void ProtoSpace::triggerGC() {
        // This is a placeholder for a real implementation.
    }
}
