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

    void ProtoThreadExtension::finalize(ProtoContext* context) const override
    {
        if (this->osThread && this->osThread->joinable()) {
            this->osThread->detach();
        }
    }

    void ProtoThreadExtension::processReferences(
        ProtoContext* context,
        void* self,
        void (*callBackMethod)(ProtoContext*, void*, const Cell* cell)
    ) const override
    {
        const Cell* currentFree = this->freeCells;
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
        extension(nullptr)
    {
        this->extension = new(context) ProtoThreadExtension(context);
        this->space->allocThread(context, (ProtoThread*)this->asThread(context));

        if (method)
        {
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
    }

    void ProtoThreadImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*callBackMethod)(ProtoContext*, void*, const Cell* cell)
    ) const
    {
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
                        callBackMethod(context, self, ctx->localsBase[i]->asCell(context));
                    }
                }
            }
            ctx = ctx->previous;
        }

        if (this->extension && this->extension->attributeCache) {
            for (unsigned int i = 0; i < THREAD_CACHE_DEPTH; ++i) {
                const AttributeCacheEntry& entry = this->extension->attributeCache[i];
                if (entry.object && entry.object->isCell(context)) callBackMethod(context, self, entry.object->asCell(context));
                if (entry.attributeName && entry.attributeName->isCell(context)) callBackMethod(context, self, entry.attributeName->asCell(context));
                if (entry.value && entry.value->isCell(context)) callBackMethod(context, self, entry.value->asCell(context));
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
    
    // ... (rest of the implementation)
}
