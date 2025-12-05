/*
 * Thread.cpp
 *
 *  Created on: 2020-5-1
 *      Author: gamarino
 *
 *  This file implements the ProtoThread, which is a native OS thread
 *  with its own execution context and memory arena.
 */

#include "../headers/proto_internal.h"
#include <cstdlib>
#include <thread>

namespace proto
{
    //=========================================================================
    // ProtoThreadExtension
    //=========================================================================

    /**
     * @class ProtoThreadExtension
     * @brief A supplementary cell to hold large or complex members of a ProtoThread.
     *
     * This class was created to solve a memory layout problem. The main
     * `ProtoThreadImplementation` cell was larger than the 64-byte `BigCell`
     * limit. This extension cell holds the larger members (`std::thread`,
     * the attribute cache, and the free cell list) so that both thread-related
     * cells fit within the 64-byte constraint.
     */

    ProtoThreadExtension::ProtoThreadExtension(ProtoContext* context) : Cell(context)
    {
        this->osThread = nullptr;
        this->freeCells = nullptr;
        this->attributeCache = static_cast<AttributeCacheEntry*>(
            std::malloc(THREAD_CACHE_DEPTH * sizeof(AttributeCacheEntry)));
        // Initialize cache entries to null.
        for (unsigned int i = 0; i < THREAD_CACHE_DEPTH; ++i)
        {
            this->attributeCache[i] = {nullptr, nullptr, nullptr};
        }
    }

    ProtoThreadExtension::~ProtoThreadExtension()
    {
        std::free(this->attributeCache);
    }

    void ProtoThreadExtension::finalize(ProtoContext* context) const
    {
        // If the thread is still joinable upon garbage collection, detach it
        // to prevent resource leaks.
        if (this->osThread && this->osThread->joinable()) {
            this->osThread->detach();
        }
    }

    void ProtoThreadExtension::processReferences(
        ProtoContext* context,
        void* self,
        void (*callBackMethod)(ProtoContext*, void*, const Cell* cell)
    ) const
    {
        // Report all cells in this thread's local free list to the GC.
        const Cell* currentFree = this->freeCells;
        while (currentFree)
        {
            callBackMethod(context, self, currentFree);
            currentFree = currentFree->next;
        }
    }


    //=========================================================================
    // ProtoThreadImplementation
    //=========================================================================

    /**
     * @brief Converts the internal implementation pointer to a public ProtoObject pointer.
     */
    const ProtoObject* ProtoThreadImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p;
        p.threadImplementation = this;
        p.op.pointer_tag = POINTER_TAG_THREAD;
        return p.oid.oid;
    }


    /**
     * @class ProtoThreadImplementation
     * @brief The core implementation of a ProtoThread.
     *
     * This class wraps a native OS thread (`std::thread`) and integrates it
     * with the Proto runtime. It manages the thread's state, its call stack
     * (via `ProtoContext`), and its local memory arena for lock-free allocation.
     */
    const ProtoThread* ProtoThreadImplementation::asThread(ProtoContext* context) const
    {
        return (const ProtoThread*)this->implAsObject(context);
    }


    ProtoThreadImplementation::ProtoThreadImplementation(
        ProtoContext* context,
        const ProtoString* name,
        ProtoSpace* space,
        ProtoMethod method,
        const ProtoList* unnamedArgList,
        const ProtoSparseList* kwargs
    ) : Cell(context),
        space(space),
        currentContext(nullptr),
        state(THREAD_STATE_MANAGED),
        unmanagedCount(0),
        name(name),
        extension(nullptr)
    {
        // Create the extension cell that holds our large members.
        this->extension = new(context) ProtoThreadExtension(context);
        
        // Register this new thread with the global space.
        this->space->allocThread(context, (ProtoThread*)this->asThread(context));

        // If a main function is provided, spawn the native thread.
        if (method)
        {
            this->extension->osThread = std::make_unique<std::thread>(
                [=](ProtoThreadImplementation* self)
                {
                    // Each new thread gets its own root context.
                    ProtoContext baseContext(self->space);
                    baseContext.thread = (ProtoThread*)self->asThread(&baseContext);
                    
                    // Execute the user's main function.
                    method(
                        &baseContext,
                        self->implAsObject(&baseContext),
                        nullptr,
                        const_cast<ProtoList*>(unnamedArgList),
                        const_cast<ProtoSparseList*>(kwargs)
                    );

                    // Unregister the thread when its main function finishes.
                    self->space->deallocThread(&baseContext, (ProtoThread*)self->asThread(&baseContext));
                },
                this
            );
        }
    }

    ProtoThreadImplementation::~ProtoThreadImplementation()
    {
    }

    /**
     * @brief Informs the GC about all objects this thread holds references to.
     * This is a critical GC root-finding method. It traverses the thread's
     * entire call stack (all contexts) and its attribute cache, reporting
     * every referenced object to the collector.
     */
    void ProtoThreadImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*callBackMethod)(ProtoContext*, void*, const Cell* cell)
    ) const
    {
        // The extension cell itself is a GC-managed object.
        if (this->extension) {
            callBackMethod(context, self, this->extension);
        }

        // Traverse the call stack (linked list of contexts).
        const ProtoContext* ctx = this->currentContext;
        while (ctx)
        {
            // Report all local variables in the current context.
            if (ctx->getAutomaticLocals())
            {
                for (unsigned int i = 0; i < ctx->getAutomaticLocalsCount(); ++i)
                {
                    const ProtoObject* local = ctx->getAutomaticLocals()[i];
                    if (local && local->isCell(context))
                    {
                        callBackMethod(context, self, local->asCell(context));
                    }
                }
            }
            ctx = ctx->previous;
        }

        // Report all objects currently in the attribute cache.
        if (this->extension && this->extension->attributeCache) {
            for (unsigned int i = 0; i < THREAD_CACHE_DEPTH; ++i) {
                const AttributeCacheEntry& entry = this->extension->attributeCache[i];
                const ProtoObject* obj = entry.object;
                const ProtoObject* attrName = (const ProtoObject*)entry.attributeName;
                const ProtoObject* val = entry.value;

                if (obj && obj->isCell(context)) callBackMethod(context, self, obj->asCell(context));
                if (attrName && attrName->isCell(context)) callBackMethod(context, self, attrName->asCell(context));
                if (val && val->isCell(context)) callBackMethod(context, self, val->asCell(context));
            }
        }
    }

    /**
     * @brief Provides a new cell from this thread's local memory arena.
     * This is the heart of the lock-free allocation strategy.
     */
    Cell* ProtoThreadImplementation::implAllocCell(ProtoContext* context)
    {
        // If the local free list is empty, synchronize with the GC to get a new batch.
        if (!this->extension->freeCells)
        {
            this->implSynchToGC();
            this->extension->freeCells = this->space->getFreeCells((ProtoThread*)this->asThread(this->currentContext));
        }

        // Pop a cell from the front of the local free list.
        Cell* newCell = this->extension->freeCells;
        if (newCell)
        {
            this->extension->freeCells = newCell->next;
            newCell->next = nullptr;
        }
        return newCell;
    }

    /**
     * @brief The synchronization mechanism for the "stop-the-world" GC phase.
     * If a GC cycle is requested, this function blocks the current thread until
     * the GC's root-finding phase is complete.
     */
    void ProtoThreadImplementation::implSynchToGC()
    {
        if (this->state == THREAD_STATE_MANAGED && this->space->state != SPACE_STATE_RUNNING)
        {
            if (this->space->state == SPACE_STATE_STOPPING_WORLD)
            {
                this->state = THREAD_STATE_STOPPING;
                this->space->stopTheWorldCV.notify_one();

                std::unique_lock lk(ProtoSpace::globalMutex);
                this->space->restartTheWorldCV.wait(lk, [this] {
                    return this->space->state == SPACE_STATE_WORLD_TO_STOP;
                });

                this->state = THREAD_STATE_STOPPED;
                this->space->stopTheWorldCV.notify_one();

                this->space->restartTheWorldCV.wait(lk, [this] {
                    return this->space->state == SPACE_STATE_RUNNING;
                });

                this->state = THREAD_STATE_MANAGED;
            }
        }
    }

    void ProtoThreadImplementation::implSetCurrentContext(ProtoContext* context) {
        this->currentContext = context;
    }

    void ProtoThreadImplementation::finalize(ProtoContext* context) const {
        // Nothing to do here, as the std::thread is handled by the unique_ptr
        // in the extension cell.
    }

    unsigned long ProtoThreadImplementation::getHash(ProtoContext* context) const {
        return Cell::getHash(context);
    }
    
    const ProtoObject* ProtoThread::getName(ProtoContext* context) const {
        return (const ProtoObject*)toImpl<const ProtoThreadImplementation>(this)->name;
    }

    const ProtoObject* ProtoThread::asObject(ProtoContext* context) const {
        return toImpl<const ProtoThreadImplementation>(this)->implAsObject(context);
    }

    unsigned long ProtoThread::getHash(ProtoContext* context) const {
        return toImpl<const ProtoThreadImplementation>(this)->getHash(context);
    }

} // namespace proto
