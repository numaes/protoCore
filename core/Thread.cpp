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
            // Mark the thread "running" only once the OS thread actually starts
            // executing.  The constructor previously incremented runningThreads
            // *before* std::thread was spawned, opening a window where the GC
            // could observe a phantom running thread (counted in
            // runningThreads but not yet able to park on stwFlag), and would
            // wait forever for parkedThreads to catch up.
            context->space->runningThreads++;
            // If a stop-the-world is already in progress when this OS thread
            // starts, park immediately so the GC's wait-for-N-parked
            // condition counts us correctly.  Without this, the new thread
            // would sail past STW and run unsynchronised, sometimes leaving
            // the GC waiting indefinitely.
            context->safepoint();
            try {
                method(context, reinterpret_cast<const ProtoObject*>(context->thread), nullptr, args, kwargs);
            } catch (const std::exception& e) {
                if (std::getenv("PROTO_THREAD_DIAG")) {
                    std::cerr << "Uncaught exception in thread: " << e.what() << std::endl;
                }
            }
            context->space->runningThreads--;
            {
                std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
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
        this->mutableValueCache = static_cast<MutableValueCacheEntry*>(
            std::malloc(MUTABLE_VALUE_CACHE_DEPTH * sizeof(MutableValueCacheEntry)));
        for (int i = 0; i < MUTABLE_VALUE_CACHE_DEPTH; ++i) {
            this->mutableValueCache[i] = {0, nullptr, nullptr};
        }
    }

    ProtoThreadExtension::~ProtoThreadExtension() {
        std::free(this->attributeCache);
        std::free(this->mutableValueCache);
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
            if (ProtoObject::isCellPointer(this->attributeCache[i].object)) {
                method(context, self, ProtoObject::asCellPointer(this->attributeCache[i].object));
            }
            if (ProtoObject::isCellPointer(this->attributeCache[i].result)) {
                method(context, self, ProtoObject::asCellPointer(this->attributeCache[i].result));
            }
            if (ProtoObject::isCellPointer(reinterpret_cast<const ProtoObject*>(this->attributeCache[i].name))) {
                method(context, self, ProtoObject::asCellPointer(reinterpret_cast<const ProtoObject*>(this->attributeCache[i].name)));
            }
        }
        // Trace MutableValueCache entries as GC roots: the cached shard_root and current_value
        // must not be reclaimed while still referenced by a live cache entry.
        if (this->mutableValueCache) {
            for (int i = 0; i < MUTABLE_VALUE_CACHE_DEPTH; ++i) {
                if (this->mutableValueCache[i].mutable_ref == 0) continue;
                const ProtoObject* sr = reinterpret_cast<const ProtoObject*>(this->mutableValueCache[i].shard_root);
                if (ProtoObject::isCellPointer(sr)) {
                    method(context, self, ProtoObject::asCellPointer(sr));
                }
                if (ProtoObject::isCellPointer(this->mutableValueCache[i].current_value)) {
                    method(context, self, ProtoObject::asCellPointer(this->mutableValueCache[i].current_value));
                }
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
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            unsigned long threadId = reinterpret_cast<uintptr_t>(this->asThread(context));
            auto* threadsImpl = toImpl<const ProtoSparseListImplementation>(space->threads);
            space->threads = const_cast<ProtoSparseList*>(threadsImpl->implSetAt(context, threadId, (const ProtoObject*)this->asThread(context))->asSparseList(context));
            // NOTE: runningThreads is intentionally NOT incremented here.
            // It is incremented inside thread_main, the moment the OS thread
            // actually starts executing.  This closes the deadlock window
            // between this point and `new std::thread(...)` below, during
            // which GC would otherwise see a phantom running thread that
            // could never park on stwFlag.
        }
        this->extension->osThread = new std::thread(thread_main, this->context, mainFunction, args, kwargs);
    }

    ProtoThreadImplementation::~ProtoThreadImplementation() {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
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
            const Cell* c = this->name->asCell(context);
            if (c) method(context, self, c);
        }
        if (this->args) {
            const Cell* c = this->args->asObject(context)->asCell(context);
            if (c) method(context, self, c);
        }
        if (this->kwargs) {
            const Cell* c = this->kwargs->asObject(context)->asCell(context);
            if (c) method(context, self, c);
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
                std::unique_lock<std::recursive_mutex> lock(ProtoSpace::globalMutex);
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

    void ProtoThread::synchToGC() {
        toImpl<ProtoThreadImplementation>(this)->implSynchToGC();
    }
}
