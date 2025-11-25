/*
 * ProtoSpace.cpp
 *
 *  Created on: Jun 20, 2016
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <memory>

namespace proto
{
    // ... (GC helper functions and thread loop)

    void gcThreadLoop(ProtoSpace* space) {
        ProtoContext gcContext(space);
        space->gcStarted = true;
        space->gcCV.notify_one();

        while (space->state != SPACE_STATE_ENDING) {
            std::unique_lock<std::mutex> lk(ProtoSpace::globalMutex);
            space->gcCV.wait_for(lk, std::chrono::milliseconds(space->gcSleepMilliseconds));
            // ...
        }
    }

    ProtoSpace::ProtoSpace()
    {
        this->state = SPACE_STATE_RUNNING;
        this->rootContext = new ProtoContext(this);
        
        this->threads = const_cast<ProtoSparseList*>(this->rootContext->newSparseList());
        this->mutableRoot.store(const_cast<ProtoSparseList*>(this->rootContext->newSparseList()));
        
        this->mainThreadId = std::this_thread::get_id();
        this->gcStarted = false;
        // ... (initialize other members)

        // --- Initialize Prototypes ---
        // Use const_cast to assign the const pointers returned by the context
        // to the non-const member variables. This is safe during initialization.
        this->objectPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->smallIntegerPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->floatPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->unicodeCharPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->bytePrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->nonePrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->methodPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->bufferPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->pointerPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->booleanPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->doublePrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->datePrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->timestampPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->timedeltaPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->threadPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->rootObject = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->listPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->listIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->tuplePrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->tupleIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->stringPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->stringIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->sparseListPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        this->sparseListIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());

        // Initialize literals
        this->literalGetAttribute = const_cast<ProtoString*>(this->rootContext->fromUTF8String("getAttribute")->asString(this->rootContext));
        this->literalSetAttribute = const_cast<ProtoString*>(this->rootContext->fromUTF8String("setAttribute")->asString(this->rootContext));
        this->literalCallMethod = const_cast<ProtoString*>(this->rootContext->fromUTF8String("callMethod")->asString(this->rootContext));

        this->gcThread = std::make_unique<std::thread>(gcThreadLoop, this);

        while (!this->gcStarted) {
            std::unique_lock<std::mutex> lk(globalMutex);
            this->gcCV.wait_for(lk, std::chrono::milliseconds(100));
        }

        const ProtoString* mainThreadName = this->rootContext->fromUTF8String("Main thread")->asString(this->rootContext);
        ProtoThread* mainThread = const_cast<ProtoThread*>(this->newThread(this->rootContext, mainThreadName, nullptr, nullptr, nullptr));
        this->rootContext->thread = mainThread;
    }

    ProtoSpace::~ProtoSpace()
    {
        this->state = SPACE_STATE_ENDING;
        if (this->gcThread && this->gcThread->joinable()) {
            this->gcCV.notify_all();
            this->gcThread->join();
        }
        delete this->rootContext;
    }

    void ProtoSpace::triggerGC()
    {
        this->gcCV.notify_all();
    }

    // ... (rest of the file)
}
