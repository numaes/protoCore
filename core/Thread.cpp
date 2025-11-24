/*
 * Thread.cpp
 *
 *  Created on: 2020-5-1
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <cstdlib>
#include <thread>

namespace proto
{
    // --- ProtoThreadExtension Implementation ---

    ProtoThreadExtension::ProtoThreadExtension(ProtoContext* context) : Cell(context)
    {
        this->osThread = nullptr;
        this->freeCells = nullptr;
        this->attributeCache = static_cast<AttributeCacheEntry*>(
            std::malloc(THREAD_CACHE_DEPTH * sizeof(AttributeCacheEntry)));
        for (unsigned int i = 0; i < THREAD_CACHE_DEPTH; ++i)
        {
            this->attributeCache[i].object = nullptr;
            this->attributeCache[i].attributeName = nullptr;
            this->attributeCache[i].value = nullptr;
        }
    }

    ProtoThreadExtension::~ProtoThreadExtension()
    {
        std::free(this->attributeCache);
    }

    void ProtoThreadExtension::finalize(ProtoContext* context) const
    {
        if (this->osThread && this->osThread->joinable()) {
            this->osThread->detach();
        }
    }

    void ProtoThreadExtension::processReferences(
        ProtoContext* context,
        void* self,
        void (*callBackMethod)(ProtoContext*, void*, Cell*)
    ) const
    {
        Cell* currentFree = this->freeCells;
        while (currentFree)
        {
            callBackMethod(context, self, currentFree);
            currentFree = currentFree->next;
        }
    }


    // --- ProtoThreadImplementation Implementation ---

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
        extension(nullptr) // Initialize to null
    {
        // Create and link the extension cell
        this->extension = new(context) ProtoThreadExtension(context);

        this->space->allocThread(context, (ProtoThread*)this->asThread(context));

        if (method)
        {
            // Use the osThread from the extension
            this->extension->osThread = std::make_unique<std::thread>(
                [=](ProtoThreadImplementation* self)
                {
                    ProtoContext baseContext(self->space);
                    baseContext.thread = (ProtoThread*)self->asThread(&baseContext);
                    
                    method(
                        &baseContext,
                        self->implAsObject(&baseContext),
                        nullptr,
                        const_cast<ProtoList*>(unnamedArgList),
                        const_cast<ProtoSparseList*>(kwargs)
                    );
                    self->space->deallocThread(&baseContext, (ProtoThread*)self->asThread(&baseContext));
                },
                this
            );
        }
    }

    ProtoThreadImplementation::~ProtoThreadImplementation()
    {
        // The unique_ptr in the extension will handle the thread automatically.
    }

    void ProtoThreadImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*callBackMethod)(ProtoContext*, void*, Cell*)
    ) const
    {
        // CRITICAL: Report the extension cell to the GC
        if (this->extension) {
            callBackMethod(context, self, this->extension);
        }

        const ProtoContext* ctx = this->currentContext;
        while (ctx)
        {
            if (ctx->localsBase)
            {
                for (unsigned int i = 0; i < ctx->localsCount; ++i)
                {
                    if (ctx->localsBase[i] && ctx->localsBase[i]->isCell(context))
                    {
                        callBackMethod(context, self, (Cell*)ctx->localsBase[i]->asCell(context));
                    }
                }
            }
            ctx = ctx->previous;
        }

        // Access the attribute cache via the extension
        if (this->extension && this->extension->attributeCache) {
            for (unsigned int i = 0; i < THREAD_CACHE_DEPTH; ++i) {
                const AttributeCacheEntry& entry = this->extension->attributeCache[i];
                if (entry.object && entry.object->isCell(context)) callBackMethod(context, self, (Cell*)entry.object->asCell(context));
                if (entry.attributeName && entry.attributeName->isCell(context)) callBackMethod(context, self, (Cell*)entry.attributeName->asCell(context));
                if (entry.value && entry.value->isCell(context)) callBackMethod(context, self, (Cell*)entry.value->asCell(context));
            }
        }
    }

    Cell* ProtoThreadImplementation::implAllocCell()
    {
        // Access freeCells via the extension
        if (!this->extension->freeCells)
        {
            this->implSynchToGC();
            this->extension->freeCells = this->space->getFreeCells((ProtoThread*)this->asThread(context));
        }

        Cell* newCell = this->extension->freeCells;
        if (newCell)
        {
            this->extension->freeCells = newCell->next;
            newCell->next = nullptr;
        }
        return newCell;
    }

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
    
    // --- Other method implementations ---
    // These methods do not need to change unless they access moved members.

    void ProtoThreadImplementation::implSetUnmanaged() { this->state = THREAD_STATE_UNMANAGED; this->unmanagedCount++; }
    void ProtoThreadImplementation::implSetManaged() { if(this->unmanagedCount > 0) this->unmanagedCount--; if(this->unmanagedCount == 0) this->state = THREAD_STATE_MANAGED; }
    void ProtoThreadImplementation::implDetach(ProtoContext* context) const { if (this->extension && this->extension->osThread && this->extension->osThread->joinable()) this->extension->osThread->detach(); }
    void ProtoThreadImplementation::implJoin(ProtoContext* context) const { if (this->extension && this->extension->osThread && this->extension->osThread->joinable()) this->extension->osThread->join(); }
    ProtoContext* ProtoThreadImplementation::implGetCurrentContext() const { return this->currentContext; }
    void ProtoThreadImplementation::implSetCurrentContext(ProtoContext* context) { this->currentContext = context; }
    const ProtoObject* ProtoThreadImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.threadImplementation = this;
        p.op.pointer_tag = POINTER_TAG_THREAD;
        return p.oid.oid;
    }
    const ProtoThread* ProtoThreadImplementation::asThread(ProtoContext* context) const {
        return (const ProtoThread*)implAsObject(context);
    }
    ProtoThreadImplementation* ProtoThreadImplementation::implGetCurrentThread() {
        // This needs a proper thread-local storage solution, but for now, we leave it as is.
        return nullptr;
    }
    unsigned long ProtoThreadImplementation::getHash(ProtoContext* context) const override { return Cell::getHash(context); }
    void ProtoThreadImplementation::finalize(ProtoContext* context) const override {}

} // namespace proto
