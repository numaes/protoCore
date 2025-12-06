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
        this->tuplePrototype = const_cast<ProtoObject*>(root_context->newObject(false));
        this->stringPrototype = const_cast<ProtoObject*>(root_context->newObject(false));
        this->setPrototype = const_cast<ProtoObject*>(root_context->newObject(false));
        this->multisetPrototype = const_cast<ProtoObject*>(root_context->newObject(false));
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
        for (int i = 0; i < 1024; ++i, newCell++)
        {
            newCell->next = previous;
            previous = newCell;
        }
        return newCell;
    }

    void ProtoSpace::submitYoungGeneration(const Cell* cell) {
        // This is a placeholder for a real implementation.
    }

    void ProtoSpace::triggerGC() {
        // This is a placeholder for a real implementation.
    }
}
