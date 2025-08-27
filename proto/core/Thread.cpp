/*
 * Thread.cpp
 *
 *  Created on: 2020-5-1
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <cstdlib> // Use the standard C++ header
#include <thread>


namespace proto
{
    // --- Constructor and Destructor ---

    // Modernized constructor with an initialization list, adjusted to not use templates.
    ProtoThreadImplementation::ProtoThreadImplementation(
        ProtoContext* context,
        ProtoString* name,
        ProtoSpace* space,
        ProtoMethod targetCode,
        ProtoList* args,
        ProtoSparseList* kwargs
    ) : Cell(context),
        state(THREAD_STATE_MANAGED),
        name(name),
        space(space),
        osThread(nullptr),
        freeCells(nullptr),
        currentContext(nullptr),
        unmanagedCount(0)
    {
        // Initialize the method cache
        this->attribute_cache = static_cast<AttributeCacheEntry*>(
            std::malloc(THREAD_CACHE_DEPTH * sizeof(*(this->attribute_cache))));
        for (unsigned int i = 0; i < THREAD_CACHE_DEPTH; ++i)
        {
            this->attribute_cache[i].object = nullptr;
            this->attribute_cache[i].attribute_name = nullptr;
        }

        // Register the thread in the memory space.
        this->space->allocThread(context, reinterpret_cast<ProtoThread*>(this));

        // Create and start the operating system thread if code is provided to execute.
        if (targetCode)
        {
            // Use std::thread to manage the thread's life safely.
            // The lambda now captures by value to avoid lifetime issues.
            this->osThread = new std::thread(
                [=](ProtoThreadImplementation* self)
                {
                    // Each thread needs its own base context.
                    ProtoContext baseContext(nullptr, nullptr, 0,
                                             reinterpret_cast<ProtoThread*>(self), self->space);
                    // Execute the thread's code.
                    targetCode(
                        &baseContext,
                        self->implAsObject(&baseContext),
                        nullptr,
                        args,
                        kwargs
                    );
                    // When the code finishes, the thread deallocates itself.
                    self->space->deallocThread(&baseContext, reinterpret_cast<ProtoThread*>(self));
                },
                this
            );
        }
        else
        {
            // This is the special case for the main thread, which does not have an associated std::thread
            // because it is already the main process thread.
            // FIX: Removed the creation of a ProtoContext with 'new', which caused a memory leak.
            // The main thread's context is managed externally by ProtoSpace.
            this->space->mainThreadId = std::this_thread::get_id();
        }
    }

    // The destructor must ensure that the OS thread has been joined or detached.
    ProtoThreadImplementation::~ProtoThreadImplementation()
    {
        // Clean up method cache
        std::free(this->attribute_cache);

        if (this->osThread)
        {
            if (this->osThread->joinable())
            {
                // For safety, if the thread is still joinable, we detach it
                // to prevent the program from terminating abruptly.
                this->osThread->detach();
            }
            delete this->osThread;
            this->osThread = nullptr;
        }
    }

    // --- Public Interface Methods ---

    void ProtoThreadImplementation::implSetUnmanaged()
    {
        this->unmanagedCount++;
        this->state = THREAD_STATE_UNMANAGED;
    }

    void ProtoThreadImplementation::implSetManaged()
    {
        if (this->unmanagedCount > 0)
        {
            this->unmanagedCount--;
        }
        if (this->unmanagedCount == 0)
        {
            this->state = THREAD_STATE_MANAGED;
        }
    }

    void ProtoThreadImplementation::implDetach(ProtoContext* context) const
    {
        if (this->osThread && this->osThread->joinable())
        {
            this->osThread->detach();
        }
    }

    void ProtoThreadImplementation::implJoin(ProtoContext* context) const
    {
        if (this->osThread && this->osThread->joinable())
        {
            this->osThread->join();
        }
    }

    void ProtoThreadImplementation::implExit(ProtoContext* context)
    {
        // FIX: Added a check to prevent a crash if osThread is null (main thread).
        if (this->osThread && this->osThread->get_id() == std::this_thread::get_id())
        {
            this->space->deallocThread(context, reinterpret_cast<ProtoThread*>(this));
            // NOTE: In a real system, an exception should be thrown here or
            // a mechanism used to terminate the thread safely.
            // std::terminate() or similar could be an option, but it is abrupt.
        }
    }

    // --- Synchronization with the Garbage Collector (GC) ---

    void ProtoThreadImplementation::implSynchToGC()
    {
        // CRITICAL FIX: The state logic was broken.
        // The state of the 'space' must be checked, not the 'thread's'.
        if (this->state == THREAD_STATE_MANAGED && this->space->state != SPACE_STATE_RUNNING)
        {
            if (this->space->state == SPACE_STATE_STOPPING_WORLD)
            {
                this->state = THREAD_STATE_STOPPING;
                this->space->stopTheWorldCV.notify_one();

                // Wait for the GC to indicate that the world should stop.
                std::unique_lock lk(ProtoSpace::globalMutex);
                this->space->restartTheWorldCV.wait(lk, [this]
                {
                    return this->space->state == SPACE_STATE_WORLD_TO_STOP;
                });

                this->state = THREAD_STATE_STOPPED;
                this->space->stopTheWorldCV.notify_one();

                // Wait for the GC to finish and the world to restart.
                this->space->restartTheWorldCV.wait(lk, [this]
                {
                    return this->space->state == SPACE_STATE_RUNNING;
                });

                this->state = THREAD_STATE_MANAGED;
            }
        }
    }

    Cell* ProtoThreadImplementation::implAllocCell()
    {
        if (!this->freeCells)
        {
            // If we run out of local cells, we synchronize with the GC
            // and request a new block of cells from the space.
            this->implSynchToGC();
            this->freeCells = static_cast<BigCell*>(this->space->getFreeCells(reinterpret_cast<ProtoThread*>(this)));
        }

        // Take the first cell from the local list.
        Cell* newCell = this->freeCells;
        if (newCell)
        {
            this->freeCells = static_cast<BigCell*>(newCell->nextCell);
            newCell->nextCell = nullptr; // Unlink it completely.
        }

        return newCell;
    }

    // --- Garbage Collector (GC) Methods ---

    void ProtoThreadImplementation::finalize(ProtoContext* context)
    {
    };

    void ProtoThreadImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, Cell* cell)
    )
    {
        // 1. The context chain (the thread's call stack).
        ProtoContext* ctx = this->currentContext;
        while (ctx)
        {
            // 4. Local variables in each stack frame.
            if (ctx->localsBase)
            {
                for (unsigned int i = 0; i < ctx->localsCount; ++i)
                {
                    if (ctx->localsBase[i] && ctx->localsBase[i]->isCell(context))
                    {
                        method(context, self, ctx->localsBase[i]->asCell(context));
                    }
                }
            }
            ctx = ctx->previous;
        }

        // 2. The thread's local list of context created objects (it uses the freeCells pointer).
        Cell* currentFree = this->freeCells;
        while (currentFree)
        {
            method(context, self, currentFree);
            currentFree = currentFree->nextCell;
        }

        // 3. The thread's method cache
        struct AttributeCacheEntry* mce = this->attribute_cache;
        for (unsigned int i = 0;
            i < THREAD_CACHE_DEPTH; ++i)
            mce++->object = nullptr;

    }

    ProtoObject* ProtoThreadImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.threadImplementation = this;
        // CRITICAL FIX: Use the correct tag for a thread.
        p.op.pointer_tag = POINTER_TAG_THREAD;
        return p.oid.oid;
    }

    void ProtoThreadImplementation::implSetCurrentContext(ProtoContext* context)
    {
        this->currentContext = context;
    }

    ProtoContext* ProtoThreadImplementation::implGetCurrentContext() const
    {
        return this->currentContext;
    }

    unsigned long ProtoThreadImplementation::getHash(ProtoContext* context)
    {
        // The hash of a Cell is derived directly from its memory address.
        // This provides a fast and unique identifier for the object.
        ProtoObjectPointer p{};
        p.oid.oid = reinterpret_cast<ProtoObject*>(this);

        return p.asHash.hash;
    }

    ProtoThread* ProtoThreadImplementation::implGetCurrentThread(const ProtoContext* context)
    {
        return context->thread;
    }
} // namespace proto
