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
        // The extension cell needs to report its own chain of free cells
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
        name(name)
    {
        // Create and link the extension cell
        this->extension = new(context) ProtoThreadExtension(context);

        this->space->allocThread(context, (ProtoThread*)this->asThread(context));

        if (method)
        {
            this->extension->osThread = std::make_unique<std::thread>(
                [=](ProtoThreadImplementation* self)
                {
                    ProtoContext baseContext(nullptr, nullptr, 0,
                                             (ProtoThread*)self->asThread(context), self->space);
                    method(
                        &baseContext,
                        self->implAsObject(&baseContext),
                        nullptr,
                        (ProtoList*)unnamedArgList,
                        (ProtoSparseList*)kwargs
                    );
                    self->space->deallocThread(&baseContext, (ProtoThread*)self->asThread(context));
                },
                this
            );
        }
        else
        {
            // Main thread case
            // space->mainThreadId = std::this_thread::get_id(); // Needs facade
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

        // Report the context chain (call stack)
        ProtoContext* ctx = this->currentContext;
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

        // Report the attribute cache
        if (this->extension && this->extension->attributeCache) {
            for (unsigned int i = 0; i < THREAD_CACHE_DEPTH; ++i) {
                const AttributeCacheEntry& entry = this->extension->attributeCache[i];
                if (entry.object && entry.object->isCell(context)) {
                    callBackMethod(context, self, (Cell*)entry.object->asCell(context));
                }
                // ProtoString is also an object that needs to be reported
                if (entry.attributeName && entry.attributeName->isCell(context)) {
                    callBackMethod(context, self, (Cell*)entry.attributeName->asCell(context));
                }
                if (entry.value && entry.value->isCell(context)) {
                    callBackMethod(context, self, (Cell*)entry.value->asCell(context));
                }
            }
        }
    }

    Cell* ProtoThreadImplementation::implAllocCell()
    {
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

    // ... (other methods need to be updated to use this->extension->...)

    void ProtoThreadImplementation::implSynchToGC()
    {
        // This logic needs to be updated to use the new facade from ProtoSpace
        // For now, it's a placeholder.
    }

    // ... (rest of the implementation)
}
