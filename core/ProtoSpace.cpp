/*
 * ProtoSpace.cpp
 *
 *  Created on: Jun 20, 2016
 *      Author: gamarino
 *
 *  This file implements the ProtoSpace, the global runtime environment for
 *  all Proto objects and threads.
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
    // The global mutex, defined here as a static member.
    std::mutex ProtoSpace::globalMutex;

    /**
     * @brief The main loop for the dedicated garbage collector thread.
     * @param space The ProtoSpace instance to monitor.
     */
    void gcThreadLoop(ProtoSpace* space)
    {
        ProtoContext gcContext(space);
        space->gcStarted = true;
        space->gcCV.notify_one();

        while (space->state != SPACE_STATE_ENDING)
        {
            std::unique_lock<std::mutex> lk(ProtoSpace::globalMutex);
            // Wait for a GC trigger or a timeout.
            space->gcCV.wait_for(lk, std::chrono::milliseconds(space->gcSleepMilliseconds));
            
            if (space->dirtySegments)
            {
                // The actual GC logic would be invoked here.
                // gcScan(&gcContext, space);
            }
        }
    }

    /**
     * @class ProtoSpace
     * @brief The global container and manager for the entire Proto runtime.
     *
     * A ProtoSpace is the top-level object that holds all shared state, including:
     * - The main heap and free cell lists for the garbage collector.
     * - The dedicated GC thread and its synchronization primitives.
     * - The root prototypes for all built-in types (Object, List, String, etc.).
     * - The global, thread-safe lists for mutable objects and active threads.
     */

    /**
     * @brief Constructs the ProtoSpace, initializing the entire runtime.
     * This involves setting up the root context, initializing all base prototypes,
     * and spawning the garbage collector thread.
     */
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
        // These objects form the base of the prototype chain for all other objects.
        // const_cast is used here safely during initialization, as the API returns
        // const pointers but these root-level members must be mutable.
        this->objectPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());
        // ... (all other prototype initializations) ...
        this->sparseListIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject());

        // --- Initialize Cached Literals ---
        this->literalGetAttribute = const_cast<ProtoString*>(this->rootContext->fromUTF8String("getAttribute")->asString(this->rootContext));
        this->literalSetAttribute = const_cast<ProtoString*>(this->rootContext->fromUTF8String("setAttribute")->asString(this->rootContext));
        this->literalCallMethod = const_cast<ProtoString*>(this->rootContext->fromUTF8String("callMethod")->asString(this->rootContext));

        // Spawn the GC thread and wait for it to signal that it's ready.
        this->gcThread = std::make_unique<std::thread>(gcThreadLoop, this);
        while (!this->gcStarted) {
            std::unique_lock<std::mutex> lk(globalMutex);
            this->gcCV.wait_for(lk, std::chrono::milliseconds(100));
        }

        // Create and register the main application thread.
        const ProtoString* mainThreadName = this->rootContext->fromUTF8String("Main thread")->asString(this->rootContext);
        ProtoThread* mainThread = const_cast<ProtoThread*>(this->newThread(this->rootContext, mainThreadName, nullptr, nullptr, nullptr));
        this->rootContext->thread = mainThread;
    }

    /**
     * @brief Destroys the ProtoSpace, shutting down the runtime.
     * This signals the GC thread to terminate and joins it, ensuring a clean shutdown.
     */
    ProtoSpace::~ProtoSpace()
    {
        this->state = SPACE_STATE_ENDING;
        if (this->gcThread && this->gcThread->joinable()) {
            this->gcCV.notify_all();
            this->gcThread->join();
        }
        delete this->rootContext;
    }

    /**
     * @brief Manually triggers a garbage collection cycle.
     */
    void ProtoSpace::triggerGC()
    {
        this->gcCV.notify_all();
    }

    /**
     * @brief Registers a new thread with the ProtoSpace.
     * This adds the thread to the globally managed list of active threads,
     * ensuring it is correctly tracked by the garbage collector.
     */
    void ProtoSpace::allocThread(ProtoContext* context, const ProtoThread* thread) {
        bool oldValue = false;
        while (this->threadsLock.compare_exchange_strong(oldValue, true)) {
            std::this_thread::yield();
        }
        this->threads = const_cast<ProtoSparseList*>(
            this->threads->setAt(
                context,
                thread->getName(context)->getHash(context),
                thread->asObject(context)
            )
        );
        this->threadsLock.store(false);
        this->runningThreads.fetch_add(1);
    }

    /**
     * @brief Placeholder for handling newly allocated cells from a context.
     * This is a critical part of the GC, where "young" objects are processed.
     */
    void ProtoSpace::analyzeUsedCells(proto::Cell *cellsChain) {
        // GC logic to handle cells from a destroyed context would go here.
    }

    /**
     * @brief Unregisters a thread from the ProtoSpace.
     */
    void ProtoSpace::deallocThread(ProtoContext* context, const ProtoThread* thread) {
        // Logic to remove the thread from the `this->threads` list would go here.
        this->runningThreads.fetch_sub(1);
    }

    /**
     * @brief Provides a batch of free cells to a thread's local arena.
     */
    Cell* ProtoSpace::getFreeCells(const ProtoThread* currentThread) {
        // GC logic to get cells from the global free list would go here.
        return nullptr;
    }

    /**
     * @brief Factory method to create and register a new ProtoThread.
     */
    const ProtoThread* ProtoSpace::newThread(ProtoContext* c, const ProtoString* name, ProtoMethod mainFunction, const ProtoList* args, const ProtoSparseList* kwargs) {
        auto* newThreadImpl = new(c) ProtoThreadImplementation(c, name, this, mainFunction, args, kwargs);
        const auto* newThread = newThreadImpl->asThread(c);
        this->allocThread(c, newThread);
        return newThread;
    }

} // namespace proto
