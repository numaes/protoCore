/*
 * Thread.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <iostream>

namespace proto {

    namespace {
        void thread_main(
            ProtoContext* context,
            ProtoMethod method,
            const ProtoList* args,
            const ProtoSparseList* kwargs
        ) {
            try {
                method(context, reinterpret_cast<const ProtoObject*>(context->thread), nullptr, args, kwargs);
            } catch (const std::exception& e) {
                std::cerr << "Uncaught exception in thread: " << e.what() << std::endl;
            }
            context->space->runningThreads--;
            {
                std::lock_guard<std::mutex> lock(ProtoSpace::globalMutex);
                unsigned long threadId = reinterpret_cast<uintptr_t>(context->thread);
                auto* threadsImpl = toImpl<const ProtoSparseListImplementation>(context->space->threads);
                context->space->threads = const_cast<ProtoSparseList*>(threadsImpl->implRemoveAt(context, threadId)->asSparseList(context));
            }
            context->space->gcCV.notify_all(); // Notify GC that a thread finished
        }
    }

    //=========================================================================
    // ProtoThreadExtension
    //=========================================================================

    ProtoThreadExtension::ProtoThreadExtension(ProtoContext* context)
        : Cell(context), osThread(nullptr), freeCells(nullptr) {
        this->attributeCache = static_cast<AttributeCacheEntry*>(
            std::malloc(THREAD_CACHE_DEPTH * sizeof(AttributeCacheEntry)));
        for (int i = 0; i < THREAD_CACHE_DEPTH; ++i) {
            this->attributeCache[i] = {nullptr, nullptr, nullptr};
        }
    }

    ProtoThreadExtension::~ProtoThreadExtension() {
        std::free(this->attributeCache);
        if (osThread && osThread->joinable()) {
            osThread->join();
        }
        delete osThread;
    }

    void ProtoThreadExtension::finalize(ProtoContext* context) const {
        // Nothing to do here
    }

    void ProtoThreadExtension::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
            )
    ) const {
        for (int i = 0; i < THREAD_CACHE_DEPTH; ++i) {
            if (this->attributeCache[i].object && this->attributeCache[i].object->isCell(context)) {
                method(context, self, this->attributeCache[i].object->asCell(context));
            }
            if (this->attributeCache[i].result && this->attributeCache[i].result->isCell(context)) {
                method(context, self, this->attributeCache[i].result->asCell(context));
            }
            if (this->attributeCache[i].name && reinterpret_cast<const ProtoObject*>(this->attributeCache[i].name)->isCell(context)) {
                method(context, self, reinterpret_cast<const ProtoObject*>(this->attributeCache[i].name)->asCell(context));
            }
        }
    }

    const ProtoObject* ProtoThreadExtension::implAsObject(ProtoContext* context) const {
        return PROTO_NONE;
    }


    //=========================================================================
    // ProtoThreadImplementation
    //=========================================================================

    const ProtoObject* ProtoThreadImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.threadImplementation = this;
        p.op.pointer_tag = POINTER_TAG_THREAD;
        return p.oid;
    }

    ProtoThreadImplementation::ProtoThreadImplementation(
        ProtoContext* context,
        const ProtoString* name,
        ProtoSpace* space,
        ProtoMethod mainFunction,
        const ProtoList* args,
        const ProtoSparseList* kwargs
    ) : Cell(context), name(name), space(space), args(args), kwargs(kwargs) {
        this->extension = new (context) ProtoThreadExtension(context);
        this->context = new ProtoContext(space, nullptr, nullptr, nullptr, args, kwargs);
        this->context->thread = (ProtoThread*)this->asThread(context);
        {
            std::lock_guard<std::mutex> lock(ProtoSpace::globalMutex);
            unsigned long threadId = reinterpret_cast<uintptr_t>(this->asThread(context));
            auto* threadsImpl = toImpl<const ProtoSparseListImplementation>(space->threads);
            space->threads = const_cast<ProtoSparseList*>(threadsImpl->implSetAt(context, threadId, (const ProtoObject*)this->asThread(context))->asSparseList(context));
            space->runningThreads++;
        }
        this->extension->osThread = new std::thread(thread_main, this->context, mainFunction, args, kwargs);
    }

    ProtoThreadImplementation::~ProtoThreadImplementation() {
        std::lock_guard<std::mutex> lock(ProtoSpace::globalMutex);
        unsigned long threadId = reinterpret_cast<uintptr_t>(this->asThread(this->context));
        auto* threadsImpl = toImpl<const ProtoSparseListImplementation>(space->threads);
        space->threads = const_cast<ProtoSparseList*>(threadsImpl->implRemoveAt(this->context, threadId)->asSparseList(this->context));
        delete this->context;
    }

    void ProtoThreadImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
            )
    ) const {
        if (this->extension) {
            method(context, self, this->extension);
        }
        if (this->name) {
            method(context, self, this->name->asCell(context));
        }
        if (this->args) {
            method(context, self, this->args->asObject(context)->asCell(context));
        }
        if (this->kwargs) {
            method(context, self, this->kwargs->asObject(context)->asCell(context));
        }
    }

    Cell* ProtoThreadImplementation::implAllocCell(ProtoContext* context) {
        if (!this->extension->freeCells) {
            this->implSynchToGC();
            this->extension->freeCells = this->space->getFreeCells((ProtoThread*)this->asThread(this->context));
        }

        if (!this->extension->freeCells) {
            return nullptr;
        }

        Cell* newCell = this->extension->freeCells;
        if (newCell) {
            this->extension->freeCells = newCell->getNext();
        }
        return newCell;
    }

    void ProtoThreadImplementation::implSynchToGC() {
        if (this->space->stwFlag.load()) {
            this->space->parkedThreads++;
            {
                std::unique_lock<std::mutex> lock(ProtoSpace::globalMutex);
                this->space->gcCV.notify_all(); // Notify GC that a thread parked
                this->space->stopTheWorldCV.wait(lock, [this] { return !this->space->stwFlag.load(); });
            }
            this->space->parkedThreads--;
        }
    }

    void ProtoThreadImplementation::implSetCurrentContext(ProtoContext* context) {
        this->context = context;
    }

    void ProtoThreadImplementation::finalize(ProtoContext* context) const {
        // Nothing to do here
    }

    unsigned long ProtoThreadImplementation::getHash(ProtoContext* context) const {
        return reinterpret_cast<uintptr_t>(this);
    }

    const ProtoThread* ProtoThreadImplementation::asThread(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.threadImplementation = this;
        p.op.pointer_tag = POINTER_TAG_THREAD;
        return p.thread;
    }

    //=========================================================================
    // ProtoThread API
    //=========================================================================

    void ProtoThread::join(ProtoContext* /*context*/) {
        auto* impl = toImpl<ProtoThreadImplementation>(this);
        if (impl->extension && impl->extension->osThread && impl->extension->osThread->joinable())
            impl->extension->osThread->join();
    }

    const ProtoObject* ProtoThread::getName(ProtoContext* context) const {
        return reinterpret_cast<const ProtoObject*>(toImpl<const ProtoThreadImplementation>(this)->name);
    }

    void ProtoThread::setCurrentContext(ProtoContext* context) {
        toImpl<ProtoThreadImplementation>(this)->implSetCurrentContext(context);
    }

    ProtoContext* ProtoThread::getCurrentContext() const {
        return toImpl<const ProtoThreadImplementation>(this)->context;
    }
}
