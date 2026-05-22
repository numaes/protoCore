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
            // Rebuild the immutable threads list OUTSIDE the global
            // mutex, then swap inside — see ProtoThreadImplementation
            // constructor for the recursive_mutex / park deadlock this
            // pattern avoids.
            unsigned long threadId = reinterpret_cast<uintptr_t>(context->thread);
            while (true) {
                const ProtoSparseList* oldThreads = context->space->threads;
                const ProtoSparseList* newThreads =
                    oldThreads->removeAt(context, threadId);
                std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
                if (context->space->threads == oldThreads) {
                    context->space->threads = const_cast<ProtoSparseList*>(newThreads);
                    break;
                }
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
        // Stash the per-thread mutable-value cache pointer in the
        // freshly-created context so resolveMutableState's hot path
        // can reach it with one load (see ProtoObject.cpp).
        this->context->mutableValueCache_ = this->extension->mutableValueCache;
        // Build the new `space->threads` list OUTSIDE the global mutex.
        //
        // Why: `implSetAt` walks the SparseList and allocates new node
        // Cells along the path.  Cell allocation may need to park on
        // STW (stwFlag).  Parking re-acquires `globalMutex` via
        // `unique_lock`, and `condition_variable::wait(lock)` only
        // unlocks ONCE on a recursive_mutex — so if we already held
        // globalMutex from an outer `lock_guard`, the wait leaves the
        // mutex still owned by us at depth 1.  GC then cannot acquire
        // it and waits forever for parkedThreads, while we wait
        // forever for stwFlag to clear.  Doing the allocation here,
        // before the lock, lets the alloc park cleanly.
        //
        // The CAS retry loop handles the rare case where another
        // thread inserts into space->threads between our read and our
        // swap — we recompute newThreads off the current oldThreads.
        unsigned long threadId = reinterpret_cast<uintptr_t>(this->asThread(context));
        const ProtoObject* threadAsObj = (const ProtoObject*)this->asThread(context);
        // GC critical section: implSetAt allocates a chain of new
        // SparseList nodes that are reachable only via `newThreads`
        // (a C++ local) until the publish-store to space->threads
        // happens under globalMutex.  A concurrent STW root scan would
        // miss the chain and a sweep would free it before the publish.
        // Same discipline as ProtoObject::setAttribute (mutable path).
        ProtoContext::CriticalSection cs(context);
        while (true) {
            const ProtoSparseList* oldThreads = space->threads;
            const ProtoSparseList* newThreads =
                oldThreads->setAt(context, threadId, threadAsObj);
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            if (space->threads == oldThreads) {
                space->threads = const_cast<ProtoSparseList*>(newThreads);
                break;
            }
            // Else: another newThread won the race; loop and rebuild
            // off the now-current oldThreads.
        }
        // NOTE: runningThreads is intentionally NOT incremented here.
        // It is incremented inside thread_main, the moment the OS
        // thread actually starts executing.  This closes the deadlock
        // window between this point and `new std::thread(...)` below,
        // during which GC would otherwise see a phantom running thread
        // that could never park on stwFlag.
        this->extension->osThread = new std::thread(thread_main, this->context, mainFunction, args, kwargs);
    }

    /** Adopt-the-main-thread variant.  Builds the extension (attribute
     *  cache + mutable-value cache + freeCells pool) and wires it as
     *  `mainContext->thread`, but does NOT spawn an std::thread and
     *  does NOT allocate a fresh ProtoContext (we adopt mainContext as
     *  our own).  Without this constructor the OS process's main thread
     *  has `context->thread == nullptr`, which silently disables the
     *  attribute cache on every getAttribute / setAttribute / has*
     *  call site — the per-thread cache lookup gates on
     *  `context->thread`, so 0 hits in the steady state. */
    ProtoThreadImplementation::ProtoThreadImplementation(
        AdoptMainThreadTag,
        ProtoContext* mainContext,
        const ProtoString* name,
        ProtoSpace* space
    ) : Cell(mainContext), name(name), space(space), args(nullptr), kwargs(nullptr) {
        this->extension = new (mainContext) ProtoThreadExtension(mainContext);
        this->context = mainContext;
        this->context->thread = (ProtoThread*)this->asThread(mainContext);
        // Stash the per-thread mutable-value cache pointer in the
        // adopted main context so resolveMutableState's hot path can
        // reach it with one load (see ProtoObject.cpp).
        this->context->mutableValueCache_ = this->extension->mutableValueCache;
        // Register in space->threads using the same lock-out-of-the-CAS
        // pattern as the spawning constructor.
        unsigned long threadId = reinterpret_cast<uintptr_t>(this->asThread(mainContext));
        const ProtoObject* threadAsObj = (const ProtoObject*)this->asThread(mainContext);
        // GC critical section: same rationale as the spawning
        // constructor above — newThreads is held in a C++ local across
        // the implSetAt allocations and only published to
        // space->threads at the bottom of the loop.
        ProtoContext::CriticalSection cs(mainContext);
        while (true) {
            const ProtoSparseList* oldThreads = space->threads;
            const ProtoSparseList* newThreads =
                oldThreads->setAt(mainContext, threadId, threadAsObj);
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            if (space->threads == oldThreads) {
                space->threads = const_cast<ProtoSparseList*>(newThreads);
                break;
            }
        }
        // No osThread spawn — we are the OS thread.
        // No runningThreads bump — the main thread is implicit and
        // doesn't participate in stop-the-world parking the same way
        // child threads do (it owns the main loop that drives GC).
    }

    ProtoThreadImplementation::~ProtoThreadImplementation() {
        // Same pattern as the constructor: do the immutable-list rebuild
        // OUTSIDE the global mutex, then swap inside.  implRemoveAt
        // allocates new node Cells and may park; parking under
        // recursive_mutex would leave the lock held at depth 1 (see
        // constructor comment for the full deadlock chain).
        unsigned long threadId = reinterpret_cast<uintptr_t>(this->asThread(this->context));
        while (true) {
            const ProtoSparseList* oldThreads = space->threads;
            const ProtoSparseList* newThreads =
                oldThreads->removeAt(this->context, threadId);
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            if (space->threads == oldThreads) {
                space->threads = const_cast<ProtoSparseList*>(newThreads);
                break;
            }
        }
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
            // getFreeCells takes the context so it can enforce the heap
            // allocation limit (and identify critical-section / GC-thread
            // callers that bypass it).  No per-context spinlock is held on
            // this path, so a GC-wait inside getFreeCells is deadlock-free.
            this->extension->freeCells = this->space->getFreeCells(context);
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
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
            // Same critical-section discipline as ProtoContext::allocCell()
            // and ProtoContext::safepoint(): never park while the current
            // context is mid-construction of a tree that will be CAS'd
            // into a GC root.  Parking here would let the GC start its
            // STW root scan before the construction completes, and the
            // half-built tree's cells (already submitted to dirtySegments
            // via the per-context allocation threshold) would be
            // unreachable from any root and freed.
            if (this->context && this->context->criticalSectionDepth > 0) return;
#endif
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
