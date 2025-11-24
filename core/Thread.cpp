/*
 * Thread.cpp
 *
 *  Created on: 2020-5-1
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <cstdlib> // Use the standard C++ header
#include <memory>
#include <thread>


namespace proto
{
    // This thread-local variable is the core of the solution. Each thread gets its
    // own private instance of this pointer. It's extremely fast and lock-free.
    thread_local ProtoThreadImplementation* current_thread_impl = nullptr;

    /**
     * @brief The main entry point for all newly created Proto threads.
     */
    void thread_entry_point(
        ProtoThreadImplementation* self,
        const ProtoMethod targetCode,
        const ProtoList* args,
        const ProtoSparseList* kwargs)
    {
        // 1. The very first action is to register this thread's implementation object.
        //    Now, any call to implGetCurrentThread() from this thread will succeed.
        current_thread_impl = self;

        // 2. Create the base context for this thread's execution.
        //    The ProtoContext constructor will now correctly find the current thread.
        ProtoContext baseContext(self->space);

        // 3. Execute the user's code.
        targetCode(&baseContext, self->implAsObject(&baseContext), nullptr, args, kwargs);

        // 4. When the user code finishes, the thread automatically deallocates itself from the space.
        self->space->deallocThread(&baseContext, reinterpret_cast<ProtoThread*>(self));
    }


    // Modernized constructor with an initialization list, adjusted to not use templates.
    ProtoThreadImplementation::ProtoThreadImplementation(
        ProtoContext* context,
        const ProtoString* name,
        ProtoSpace* space,
        const ProtoMethod method,
        const ProtoList* unnamedArgList,
        const ProtoSparseList* kwargs
    ) : Cell(context),
        space(space),
        state(THREAD_STATE_MANAGED),
        name(name),
        method(method), // This is const
        unnamedArgList(unnamedArgList),
        kwargs(kwargs),
        osThread(nullptr),
        currentContext(nullptr),
        unmanagedCount(0),
        freeCells(nullptr)
    {
        // Initialize the method cache
        this->attributeCache = new AttributeCacheEntry[THREAD_CACHE_DEPTH];
        for (unsigned int i = 0; i < THREAD_CACHE_DEPTH; ++i)
        {
            this->attributeCache[i].object = nullptr;
            this->attributeCache[i].attributeName = nullptr;
        }

        // Register the thread in the memory space.
        this->space->allocThread(context, reinterpret_cast<ProtoThread*>(this));

        // Create and start the operating system thread if code is provided to execute.
        if (this->method)
        {
            this->osThread = new std::thread(thread_entry_point, this, this->method, unnamedArgList, kwargs);
        }
        else
        {
            // This is the special case for the main thread, which does not have an associated std::thread
            // because it is already the main process thread.
            current_thread_impl = this; // Register the main thread as well.
        }
    }

    // The destructor must ensure that the OS thread has been joined or detached.
    ProtoThreadImplementation::~ProtoThreadImplementation()
    {
        // The finalize() method handles the cleanup. The destructor is here for completeness
        // in case an object is ever stack-allocated, but in Proto's model, finalize() is key.
        this->finalize(nullptr);
        delete[] this->attributeCache;
    }

    // --- Public Interface Methods ---

    void ProtoThreadImplementation::implSetUnmanaged()
    {
        this->unmanagedCount.fetch_add(1);
        this->state = THREAD_STATE_UNMANAGED;
    }

    void ProtoThreadImplementation::implSetManaged()
    {
        if (this->unmanagedCount.load() > 0)
        {
            this->unmanagedCount.fetch_sub(1);
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

    void ProtoThreadImplementation::implFinalize(ProtoContext* context) const
    {
        // FIX: Added a check to prevent a crash if osThread is null (main thread).
        if (this->osThread && this->osThread->get_id() == std::this_thread::get_id())
        {
            this->space->deallocThread(context, this->asThread());
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
                std::unique_lock lk(ProtoSpace::globalMutex);
                this->space->stopTheWorldCV.notify_one(); // Notify GC that we are stopping

                // Wait until the GC either finishes or moves to the next state
                this->space->restartTheWorldCV.wait(lk, [this]
                {
                    return this->space->state != SPACE_STATE_STOPPING_WORLD;
                });

                if (this->space->state == SPACE_STATE_WORLD_TO_STOP)
                {
                    this->state = THREAD_STATE_STOPPED;
                    this->space->stopTheWorldCV.notify_one(); // Notify GC we are fully stopped

                    // Wait for the GC to finish and restart the world
                    this->space->restartTheWorldCV.wait(lk, [this] { return this->space->state == SPACE_STATE_RUNNING; });
                }

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
            this->freeCells = static_cast<Cell*>(this->space->getFreeCells(reinterpret_cast<ProtoThread*>(this)));
        }

        // Take the first cell from the local list.
        Cell* newCell = this->freeCells;
        if (newCell)
        {
            this->freeCells = static_cast<Cell*>(newCell->next);
            newCell->next = nullptr; // Unlink it completely.
        }

        return newCell;
    }

    // --- Garbage Collector (GC) Methods ---

    void ProtoThreadImplementation::finalize(ProtoContext* context) const override
    {
        // This is the effective "destructor" called by the GC.
        // It's responsible for cleaning up non-Proto resources.
        if (this->osThread)
        {
            if (this->osThread->joinable())
            {
                // Detach the thread to allow the OS to reclaim its resources
                // without forcing the program to wait (join).
                this->osThread->detach();
            }
            delete this->osThread;
            // Set to nullptr to prevent double-delete if destructor is also called.
            const_cast<ProtoThreadImplementation*>(this)->osThread = nullptr;
        }
    };

    void ProtoThreadImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*callBackMethod)(ProtoContext* context, void* self, Cell* cell)
    ) const override
    {
        // 1. The context chain (the thread's call stack).
        const ProtoContext* ctx = this->currentContext;
        while (ctx)
        {
            // Scan local variables in each stack frame.
            if (ctx->localsBase)
            {
                for (unsigned int i = 0; i < ctx->localsCount; ++i)
                {
                    if (ctx->localsBase[i] && ctx->localsBase[i]->isCell(context))
                    {
                        callBackMethod(context, self, const_cast<Cell*>(ctx->localsBase[i]->asCell(context)));
                    }
                }
            }

            ctx = ctx->previous;
        }

        // 3. The thread's method cache
        // CRITICAL FIX: The GC must scan the cache for live objects, not destroy it.
        // The old code was a use-after-free time bomb.
        for (unsigned int i = 0; i < THREAD_CACHE_DEPTH; ++i)
        {
            // Scan the object key of the cache entry.
            if (this->attributeCache[i].object && this->attributeCache[i].object->isCell(context)) {
                callBackMethod(context, self, const_cast<Cell*>(this->attributeCache[i].object->asCell(context)));
            }
            // Scan the resolved value. ProtoStrings for attributeName are interned and don't need scanning.
            if (this->attributeCache[i].value && this->attributeCache[i].value->isCell(context)) {
                callBackMethod(context, self, const_cast<Cell*>(this->attributeCache[i].value->asCell(context)));
            }
        }
    }

    const ProtoObject* ProtoThreadImplementation::implAsObject(ProtoContext* context) const override
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

    unsigned long ProtoThreadImplementation::getHash(ProtoContext* context) const override
    {
        // The hash of a Cell is derived directly from its memory address.
        // This provides a fast and unique identifier for the object.
        ProtoObjectPointer p{};
        p.threadImplementation = this;

        return p.asHash.hash;
    }

    ProtoThreadImplementation* ProtoThreadImplementation::implGetCurrentThread()
    {
        // This static method now efficiently returns the thread-local pointer.
        // It returns a pointer to the implementation, which is a subclass of the public API class.
        // The public API returns a const pointer, but internally we need a mutable one.
        return static_cast<ProtoThread*>(current_thread_impl);
    }
} // namespace proto
