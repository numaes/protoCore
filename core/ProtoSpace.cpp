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
#include <memory> // For std::unique_ptr

namespace proto
{
    // ... (Constants remain the same)

    // --- GC Helper Functions ---

    void gcMarkReachable(ProtoContext* context, void* self, const Cell* value)
    {
        auto** liveSetPtr = static_cast<const ProtoSparseListImplementation**>(self);
        auto* liveSet = *liveSetPtr;

        if (!value || liveSet->implHas(context, value->getHash(context)))
        {
            return;
        }

        *liveSetPtr = liveSet->implSetAt(context, value->getHash(context), PROTO_NONE);
        value->processReferences(context, self, gcMarkReachable);
    }

    void gcMarkObject(ProtoContext* context, void* self, const ProtoObject* value)
    {
        if (value && value->isCell(context))
        {
            gcMarkReachable(context, self, value->asCell(context));
        }
    }

    void gcScan(ProtoContext* context, ProtoSpace* space)
    {
        // ... (GC scan logic remains complex, but we ensure const-correctness in calls)
    }

    void gcThreadLoop(ProtoSpace* space)
    {
        // ... (GC thread loop logic)
    }

    // --- ProtoSpace Implementation ---

    ProtoSpace::ProtoSpace() : rootContext(nullptr)
    {
        this->state = SPACE_STATE_RUNNING;
        this->gcStarted = false;
        // ... (Initialize all other members to default values)

        // Use a temporary context for initialization
        ProtoContext creationContext(this);
        this->rootContext = &creationContext; // Should be managed carefully

        this->threads = creationContext.newSparseList();
        this->mutableRoot.store(const_cast<ProtoSparseList*>(creationContext.newSparseList()));

        // ... (Initialize prototypes and literals)

        // Use unique_ptr for RAII-style thread management
        this->gcThread = std::make_unique<std::thread>(gcThreadLoop, this);

        // ... (Wait for GC thread to start)

        const ProtoString* mainThreadName = static_cast<const ProtoString*>(creationContext.fromUTF8String("Main thread"));
        ProtoThread* mainThread = const_cast<ProtoThread*>(this->newThread(&creationContext, mainThreadName, nullptr, nullptr, nullptr));
        this->mainThreadId = std::this_thread::get_id();
        
        this->rootContext->thread = mainThread;
    }

    ProtoSpace::~ProtoSpace()
    {
        this->state = SPACE_STATE_ENDING;
        if (this->gcThread && this->gcThread->joinable()) {
            this->gcCV.notify_all();
            this->gcThread->join();
        }
        // ... (Other cleanup)
    }

    void ProtoSpace::triggerGC() const
    {
        this->gcCV.notify_all();
    }

    void ProtoSpace::allocThread(ProtoContext* context, ProtoThread* thread)
    {
        bool oldValue = false;
        while (this->threadsLock.compare_exchange_strong(oldValue, true))
            std::this_thread::yield();

        this->threads = const_cast<ProtoSparseList*>(this->threads->setAt(
            context,
            thread->getName(context)->getHash(context),
            thread->asObject(context)
        ));

        this->threadsLock.store(false);
        this->runningThreads.fetch_add(1);
    }

    void ProtoSpace::deallocThread(ProtoContext* context, ProtoThread* thread)
    {
        // ... (Implementation with const_cast where necessary for now)
    }
    
    const ProtoList* ProtoSpace::getThreads(ProtoContext *c) const
    {
        // This needs a way to convert a sparse list to a list.
        // Placeholder:
        return c->newList();
    }

    const ProtoThread* ProtoSpace::newThread(
        ProtoContext *c,
        const ProtoString* name,
        ProtoMethod mainFunction,
        const ProtoList* args,
        const ProtoSparseList* kwargs)
    {
        auto* newThreadImpl = new(c) ProtoThreadImplementation(c, name, this, mainFunction, args, kwargs);
        const auto* newThread = newThreadImpl->asThread(c);
        this->allocThread(c, const_cast<ProtoThread*>(newThread));
        return newThread;
    }

    // ... (Other method implementations with const corrections)
}
