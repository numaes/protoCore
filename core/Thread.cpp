/*
 * Thread.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <cstdio>
#include <cstring>
#include <iostream>

namespace proto {

    namespace {
        /**
         * Release everything a managed thread owns, on that thread, as the last
         * thing it does.
         *
         * WHY NOT `finalize`.  The obvious home for this is
         * ProtoThreadImplementation::finalize, and it is the wrong one, for
         * three independent reasons.
         *
         *  1. The finalizer contract forbids it.  docs/GarbageCollector.md
         *     section 7: a finalizer runs on the single GC thread inside the
         *     sweep, concurrently with the mutators; it never allocates, never
         *     publishes to a shared structure with compare-and-swap, never
         *     loops over protoCore data, and must not BLOCK, because a wait
         *     there stalls collection for the whole space.  Returning the
         *     batch takes ProtoSpace::globalMutex (blocks), destroying the root
         *     context submits a young generation (publishes) and joining the
         *     std::thread blocks outright.
         *  2. It would free memory a live thread is still using.  Sweep runs
         *     with the world going; a thread cell becoming garbage is not by
         *     itself proof that the OS thread has stopped touching its caches.
         *  3. It would never run.  The ProtoThreadImplementation and
         *     ProtoThreadExtension cells are allocated on the scratch context
         *     ProtoSpace::newThread creates and never destroys, so their young
         *     chain is never submitted, so they are never sweep candidates and
         *     `finalize` is never reached on them at all.
         *
         * WHY HERE.  The exiting thread is the only party that knows the OS
         * thread is finished with its own cells and caches, and it can block
         * freely: it has already left `runningThreads`, so a collector waiting
         * for the stop-the-world quorum is not waiting for it, and `join` on
         * this thread cannot return until this function has.  Every step below
         * is therefore an ordinary mutator operation on the owning thread, not
         * collector work.
         *
         * ORDER IS LOAD-BEARING.  The root context is destroyed FIRST, because
         * ~ProtoContext is what hands this thread's young generation to the
         * space (the cells `removeAt` just allocated for the new threads list
         * among them - they are reachable from `space->threads`, which is a
         * root, so submitting them is what lets a later cycle account for them
         * instead of losing them).  The batch goes back LAST, because until the
         * context is gone this thread could still allocate from it.
         */
        void releaseExitingThread(ProtoContext* context)
        {
            ProtoSpace* space = context->space;
            auto* impl = context->thread
                ? const_cast<ProtoThreadImplementation*>(
                      toImpl<const ProtoThreadImplementation>(context->thread))
                : nullptr;
            ProtoThreadExtension* ext = impl ? impl->extension : nullptr;

            // Never release the adopted main thread: its "root context" is
            // ProtoSpace::rootContext, which ~ProtoSpace owns, and it has no
            // osThread.  The main thread never runs thread_main, so this is a
            // guard against a future caller, not against today's one.
            if (impl && impl->context == space->rootContext) return;

            // 1. The root ProtoContext.  ~ProtoContext submits the young
            //    generation, returns its own per-context freelist, frees the
            //    automaticLocals array, and sets impl->context to nullptr
            //    (implSetCurrentContext(previous), and previous is null for a
            //    thread root).  Safe here and nowhere else: this thread is out
            //    of `space->threads` already, so no stop-the-world root scan
            //    can be walking this context - the removal above published
            //    under globalMutex, and Phase 2 reads the list under the same
            //    mutex, so the two are serialised.
            delete context;

            if (!ext) return;

            // 2. The two per-thread caches: 32 KiB aligned_alloc plus 24 KiB
            //    malloc, neither of them cells and neither visible to
            //    heapSize.  They are NOT GC roots and the marker never reads
            //    them (ProtoThreadExtension::processReferences reports
            //    nothing), and the only readers are the attribute / mutable
            //    lookup paths reached through a ProtoContext of THIS thread -
            //    all of which are gone by now, the root one at step 1.
            std::free(ext->attributeCache);
            ext->attributeCache = nullptr;
            std::free(ext->mutableValueCache);
            ext->mutableValueCache = nullptr;

            // 3. The unused tail of this thread's allocation batch.  This is
            //    the measured leak: up to one batch per exiting thread,
            //    belonging to no freelist and to no young generation, so no
            //    cycle could ever reclaim it (docs/GarbageCollector.md, known
            //    issues).  Detach it before publishing, so that nothing can
            //    hand out a cell that is already on the space's freelist.
            Cell* batch = ext->freeCells;
            ext->freeCells = nullptr;
            (void) returnUnusedCellBatch(space, batch);

            // `ext->osThread` is deliberately left alone.  The std::thread
            // object must outlive this function - an embedder may still be
            // blocked in ProtoThread::join on it - and it cannot be joined
            // from inside the thread it represents.  ProtoThread::join deletes
            // it once the join has proved the OS thread is gone.
        }

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
            // Everything this thread owns goes back now, on this thread.  After
            // this call `context` is a dangling pointer, so nothing below may
            // touch it - hence the saved `space`.
            ProtoSpace* space = context->space;
            releaseExitingThread(context);

            space->gcCV.notify_all(); // Notify GC that a thread finished
        }
    }

    //=========================================================================
    // ProtoThreadExtension
    //=========================================================================

    ProtoThreadExtension::ProtoThreadExtension(ProtoContext* context)
        : Cell(context), osThread(nullptr), freeCells(nullptr) {
        // Allocate the attribute cache aligned to a 64-byte cache line so
        // that the 32-byte AttributeCacheEntry pair always lands within
        // one line — no split-line loads on lookups.  malloc only
        // guarantees 16-byte alignment; std::aligned_alloc requires the
        // request size to be a multiple of the alignment (a C11
        // requirement it inherits), which holds here because
        // THREAD_CACHE_DEPTH * 32 is a multiple of 64 for any depth >= 2.
        this->attributeCache = static_cast<AttributeCacheEntry*>(
            std::aligned_alloc(64, THREAD_CACHE_DEPTH * sizeof(AttributeCacheEntry)));
        for (int i = 0; i < THREAD_CACHE_DEPTH; ++i) {
            this->attributeCache[i] = {nullptr, nullptr, nullptr, nullptr};
        }
        this->mutableValueCache = static_cast<MutableValueCacheEntry*>(
            std::malloc(MUTABLE_VALUE_CACHE_DEPTH * sizeof(MutableValueCacheEntry)));
        for (int i = 0; i < MUTABLE_VALUE_CACHE_DEPTH; ++i) {
            this->mutableValueCache[i] = {0, nullptr, nullptr};
        }
        // The caches start empty, so they are "cleared" as of the current
        // cycle.  A new thread therefore does not clear again on its first
        // park exit unless a stop-the-world completes after this point.
        this->lastClearedEpoch = (context && context->space)
            ? context->space->gcCycleCount.load(std::memory_order_relaxed)
            : 0;
    }

    void ProtoThreadExtension::clearCachesAfterStopTheWorld(const ProtoSpace* space) {
        // Called only by the owning thread, on its way out of a stop-the-world
        // wait; never on the lookup path.  gcCycleCount advances only under
        // stop-the-world, while every thread that could touch these caches is
        // parked or unmanaged, so a mismatch means a stop-the-world completed
        // since the last clear and any entry may name a cell that the cycle's
        // sweep frees and reuses.  An all-zero entry never matches a lookup
        // (object == nullptr, mutable_ref == 0).
        // A thread that has exited has already freed both caches
        // (releaseExitingThread).  Nothing on that thread can reach this
        // function afterwards, but another thread holding the ProtoThread can,
        // so the null check is the cheap way to make that harmless.
        if (!this->attributeCache || !this->mutableValueCache) return;
        const uint64_t epoch = space->gcCycleCount.load(std::memory_order_relaxed);
        if (epoch == this->lastClearedEpoch) return;
        std::memset(static_cast<void*>(this->attributeCache), 0,
                    THREAD_CACHE_DEPTH * sizeof(AttributeCacheEntry));
        std::memset(static_cast<void*>(this->mutableValueCache), 0,
                    MUTABLE_VALUE_CACHE_DEPTH * sizeof(MutableValueCacheEntry));
        this->lastClearedEpoch = epoch;
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
        // Nothing to report.  The per-thread attributeCache and
        // mutableValueCache are NOT GC roots, and the marker never reads them
        // (their owning thread rewrites them at any time).  The owning thread
        // clears both caches when it resumes after a stop-the-world
        // (clearCachesAfterStopTheWorld), before its next lookup, so no entry
        // can be used after a cell it names could have been freed.  The
        // mutables tree stays the single registry of mutable state.
        (void) context;
        (void) self;
        (void) method;
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

        // Building the NEW thread's root context must not leave the CREATING
        // thread registered against it.
        //
        // ProtoContext's constructor registers every context it builds as the
        // current context of the thread it belongs to, or - when it belongs to
        // no thread - as ProtoSpace::mainContext (core/ProtoContext.cpp, step
        // 3).  `previous == nullptr` makes the new thread's root context look
        // like a thread root to that code, so on the main thread it silently
        // becomes the main thread's current context, and on a worker it
        // silently becomes space->mainContext.  Both of those slots are GC root
        // sources for a thread that does not own this context.
        //
        // That was merely wrong until now (one thread's roots scanned from
        // another thread's context, healed by the creator's next context
        // construction).  It becomes a dangling pointer the moment the exiting
        // thread destroys its own root context, which releaseExitingThread
        // above now does.  So snapshot both slots and put them back.
        auto* callerImpl = (context && context->thread)
            ? toImpl<ProtoThreadImplementation>(context->thread)
            : nullptr;
        ProtoContext* const callerCurrent = callerImpl ? callerImpl->context : nullptr;
        ProtoContext* const savedMainContext = space ? space->mainContext : nullptr;

        this->context = new ProtoContext(space, nullptr, nullptr, nullptr, args, kwargs);
        this->context->thread = (ProtoThread*)this->asThread(context);
        // Stash the per-thread mutable-value cache pointer in the
        // freshly-created context so resolveMutableState's hot path
        // can reach it with one load (see ProtoObject.cpp).
        this->context->mutableValueCache_ = this->extension->mutableValueCache;

        if (callerImpl) callerImpl->implSetCurrentContext(callerCurrent);
        if (space) space->mainContext = savedMainContext;
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
            // Same critical-section discipline as ProtoContext::allocCell()
            // and ProtoContext::safepoint(), in every configuration: never
            // park while the current context is inside a critical section.
            // The thread may be mid-construction of a tree it will CAS into a
            // root, or hold cells read from the mutables tree (a snapshot, a
            // shard root being path-copied) only in C++ locals; a stop-the-
            // world there could free them.
            if (this->context && this->context->criticalSectionDepth > 0) return;
            this->space->parkedThreads++;
            {
                std::unique_lock<std::recursive_mutex> lock(ProtoSpace::globalMutex);
                this->space->gcCV.notify_all(); // Notify GC that a thread parked
                this->space->stopTheWorldCV.wait(lock, [this] { return !this->space->stwFlag.load(); });
            }
            this->space->parkedThreads--;
            // Resumed after a stop-the-world: drop this thread's cache entries
            // before the next lookup (the caches are not GC roots).
            if (this->extension) this->extension->clearCachesAfterStopTheWorld(this->space);
        }
    }

    // 2026-05-25: unmanaged-region API.
    //
    // The thread is about to leave protoCore-managed code (typically for a
    // blocking OS call: read / write / sleep / poll / network I/O). Bump the
    // depth counter; if this is the FIRST entry (0 → 1) the thread also
    // pre-registers as parked in the GC quorum (parkedThreads++), so the
    // collector can complete a stop-the-world phase without waiting for this
    // thread to reach a real safepoint. Notify the GC after the counter bump
    // — it may already have set stwFlag and be waiting on the quorum that
    // this thread was the missing piece of.
    //
    // Nested calls (depth > 1 after increment) do not change parkedThreads —
    // a single thread can only contribute one slot to the quorum regardless
    // of how many transitive unmanaged regions it has open.
    void ProtoThreadImplementation::implGoUnmanaged() {
        if (!this->extension) {
            // Early-bootstrap path: the per-thread extension is not yet
            // wired up. With no extension we have no counter to bump,
            // and (more importantly) no per-thread arena that the GC
            // would need to wait for — the unmanaged region is a no-op
            // until the extension is in place.
            return;
        }
        int prev = this->extension->unmanagedDepth.fetch_add(1, std::memory_order_acq_rel);
        if (prev == 0) {
            // First entry: announce this thread as "out".
            this->space->parkedThreads.fetch_add(1, std::memory_order_acq_rel);
            // The GC may have been waiting for us — kick the quorum check.
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            this->space->gcCV.notify_all();
        }
    }

    // Mirror of implGoUnmanaged. Decrement the counter; if this was the
    // outermost pair (depth went 1 → 0) the thread re-enters the GC quorum.
    // If a stop-the-world phase is in progress at that moment, the thread
    // BLOCKS exactly like a normal safepoint park — it must not touch any
    // ProtoObject* until the GC's scan finishes.
    //
    // Order matters: we decrement parkedThreads first (we are no longer
    // "out"), THEN if STW is set we re-park properly. The brief window where
    // parkedThreads dipped is harmless: if the GC was past its quorum check
    // (i.e. stwFlag set), it is already running and the dip does not affect
    // it; if the GC was NOT yet past quorum, our re-park immediately
    // restores the count.
    void ProtoThreadImplementation::implReturnFromUnmanaged() {
        if (!this->extension) return;  // mirror implGoUnmanaged's early-bootstrap guard
        int prev = this->extension->unmanagedDepth.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) {
            // Last release: un-park.
            this->space->parkedThreads.fetch_sub(1, std::memory_order_acq_rel);
            // If STW is happening, block as a normal safepoint would.
            if (this->space->stwFlag.load(std::memory_order_acquire)) {
                this->space->parkedThreads.fetch_add(1, std::memory_order_acq_rel);
                {
                    std::unique_lock<std::recursive_mutex> lock(ProtoSpace::globalMutex);
                    this->space->gcCV.notify_all();
                    this->space->stopTheWorldCV.wait(lock,
                        [this] { return !this->space->stwFlag.load(); });
                }
                this->space->parkedThreads.fetch_sub(1, std::memory_order_acq_rel);
            }
            // Back in managed code.  While unmanaged the thread counted as
            // parked, so whole cycles may have completed without it: drop its
            // cache entries before its next lookup if a stop-the-world
            // happened (the caches are not GC roots).
            this->extension->clearCachesAfterStopTheWorld(this->space);
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

    // 2026-09-25 (P4): joining leaves the running set for the duration of the
    // block, and the KERNEL does it — not each embedder.
    //
    // The bug this closes is a deadlock, not slow shutdown.  `runningThreads`
    // starts at 1 (the main thread is counted from ProtoSpace construction,
    // core/ProtoSpace.cpp:1123) and every managed thread adds one in
    // thread_main.  A stop-the-world phase cannot begin until
    // `parkedThreads >= runningThreads` (core/ProtoSpace.cpp:322).  A bare
    // std::thread::join reaches no safepoint, so a registered thread blocked
    // there still counts as running: the quorum can never be met, no cycle can
    // start, and every thread that then needs memory waits in
    // waitForHeapHeadroom for a cycle that cannot begin — usually including
    // the very thread being joined, which is why the join never returns
    // either.  Read off a live protoClojure backtrace: four blocking joins
    // (future deref, pmap, shutdownFutures, ActorScheduler::shutdown) each hung
    // to a 90-second timeout and each completed in about three seconds once
    // bracketed.
    //
    // No documentation stated the obligation FOR A JOIN: this function carried
    // no doc comment at all before the fix.  The general rule was written down
    // -- DESIGN.md, "Unmanaged regions" (pre-fix :126-202), documents
    // UnmanagedScope and the quorum formula with a "when to use it" table --
    // but that table's rows are read/write, sleep, poll, accept and
    // "third-party C library that may block", and there was NO ROW for a thread
    // join.  The principle was documented; the instance was not, and the
    // instance was the kernel's own API.  (This comment first claimed no
    // documentation stated it at all; corrected 2026-09-25, see
    // docs/FIELD-NOTES.md case 3.)  `join` is protoCore's OWN blocking call, so
    // an embedder had no way to know it had to bracket a kernel API against the
    // kernel's own quorum.  The guard
    // therefore belongs here: every embedder gets the correct behaviour
    // without knowing the rule exists.  Nesting is safe and idempotent — an
    // embedder that ALSO wraps its join in UnmanagedScope only bumps
    // `unmanagedDepth`, and only the outermost pair moves `parkedThreads`
    // (implGoUnmanaged / implReturnFromUnmanaged above).
    //
    // criticalSectionDepth > 0 is the one case where we must NOT leave the
    // running set.  A critical section means the caller is holding cells that
    // are reachable only from C++ locals — a half-built tree before its CAS
    // into a root, or a snapshot read out of the mutables tree.  Announcing
    // ourselves as parked there would let a stop-the-world root scan proceed
    // while those cells are invisible to it, and the sweep would free them
    // under us.  That trades a deadlock for memory corruption, which is the
    // worse trade, so we refuse: we join WITHOUT leaving the running set,
    // exactly as before this change, and say so once on stderr.  Blocking
    // inside a critical section is itself a contract violation
    // (docs/EMBEDDER-CONFORMANCE.md rule 12); the diagnostic names it rather
    // than hiding it behind either a hang or an abort.
    namespace {
        // Release the std::thread object of a thread that has just been joined.
        // Non-blocking by construction: the join above already returned, so the
        // object is no longer joinable and ~std::thread is a no-op.
        void releaseJoinedOsThread(ProtoThreadExtension* ext) {
            if (!ext) return;
            std::thread* osThread = ext->osThread;
            ext->osThread = nullptr;
            delete osThread;
        }
    }

    void ProtoThread::join(ProtoContext* context) {
        auto* impl = toImpl<ProtoThreadImplementation>(this);
        if (!impl->extension || !impl->extension->osThread ||
            !impl->extension->osThread->joinable())
            return;

        std::thread* osThread = impl->extension->osThread;

        if (context && context->criticalSectionDepth > 0) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true, std::memory_order_relaxed)) {
                std::fprintf(stderr,
                    "protoCore: ProtoThread::join called inside a GC critical "
                    "section (criticalSectionDepth=%u).  The calling thread "
                    "cannot leave the running set there without exposing "
                    "cells held only in C++ locals to the sweep, so the "
                    "stop-the-world quorum is held for the duration of this "
                    "join and a collection cannot start.  Move the join "
                    "outside the critical section: see "
                    "docs/EMBEDDER-CONFORMANCE.md rule 12.\n",
                    context->criticalSectionDepth);
            }
            osThread->join();
            releaseJoinedOsThread(impl->extension);
            return;
        }

        // The normal path: out of the running set, block, back in.  The
        // returning half re-parks properly if a stop-the-world phase is in
        // progress when the join completes (implReturnFromUnmanaged).
        {
            ProtoContext::UnmanagedScope parked(context);
            osThread->join();
        }
        // A completed join is the only proof protoCore ever gets that the OS
        // thread is gone, so it is the only place the std::thread object can be
        // released.  It cannot be done by the thread itself (a thread cannot
        // join itself) nor by a finalizer (join blocks, and the sweep may not).
        // A second join on the same ProtoThread now returns at the osThread
        // null check above instead of touching a freed object; a thread that is
        // never joined still leaks its std::thread, which is the embedder's
        // side of the contract, not the kernel's.
        releaseJoinedOsThread(impl->extension);
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

    void ProtoThread::goUnmanaged() {
        toImpl<ProtoThreadImplementation>(this)->implGoUnmanaged();
    }

    void ProtoThread::returnFromUnmanaged() {
        toImpl<ProtoThreadImplementation>(this)->implReturnFromUnmanaged();
    }
}
