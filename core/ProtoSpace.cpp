/*
 * ProtoSpace.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
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
        // This is a placeholder for a real implementation.
        // A real implementation would need to manage memory pools.
        return static_cast<Cell*>(std::malloc(sizeof(BigCell)));
    }

    void ProtoSpace::submitYoungGeneration(const Cell* cell) {
        // This is a placeholder for a real implementation.
    }

    void ProtoSpace::triggerGC() {
        // This is a placeholder for a real implementation.
    }
}
