/*
 * ProtoSpace.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <set>
#include <vector>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <thread>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace proto {

    namespace {
        /** Maximum bytes to request from the OS in a single getFreeCells allocation (16 MiB). */
        constexpr unsigned long kMaxBytesPerOSAllocation = 16u * 1024u * 1024u;
        /** Maximum number of blocks (BigCells) per OS request. */
        constexpr int kMaxBlocksPerOSAllocation = static_cast<int>(kMaxBytesPerOSAllocation / sizeof(BigCell));
        /**
         * Under a hard heap limit, all running threads' refill batches
         * together use at most maxHeapSize / kLimitBatchFraction cells: each
         * refill is capped at maxHeapSize / (kLimitBatchFraction * threads).
         * Cells a thread holds in its private freelist count against the
         * limit but cannot be reclaimed, so this bounds the part of the limit
         * that is out of the collector's reach.
         */
        constexpr long kLimitBatchFraction = 8;
        /**
         * Floor of a capped refill, so that a very small limit or many threads
         * do not turn every few allocations into a refill under globalMutex.
         */
        constexpr long kMinLimitedBatchCells = 512;

        std::atomic<uint64_t> s_getFreeCellsCalls{0};
        static long long diagCurrentTid() {
#if defined(__linux__)
            return static_cast<long long>(syscall(SYS_gettid));
#else
            return static_cast<long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
        }
        std::mutex s_diagTidsMutex;
        std::unordered_set<long long> s_diagTids;
        void diagPrintAllocCount() {
            (void)s_getFreeCellsCalls;
            (void)s_diagTidsMutex;
            (void)s_diagTids;
        }
        const ProtoList* buildDefaultResolutionChain(ProtoContext* ctx) {
            // GC critical section: `chain` accumulates entries across the
            // loop and is held in this C++ local between iterations; each
            // appendLast also enters its own critical section internally,
            // but the outer guard covers the gap between them where
            // chain would otherwise be unrooted.
            ProtoContext::CriticalSection cs(ctx);
            const ProtoList* chain = ctx->newList();
            if (!chain) return nullptr;
#if defined(_WIN32)
            const char* defaults[] = { ".", "C:\\Program Files\\proto\\lib" };
            const int n = 2;
#elif defined(__APPLE__)
            const char* defaults[] = { ".", "/usr/local/lib/proto" };
            const int n = 2;
#else
            const char* defaults[] = { ".", "/usr/lib/proto", "/usr/local/lib/proto" };
            const int n = 3;
#endif
            for (int i = 0; i < n && chain; ++i) {
                const ProtoObject* s = ctx->fromUTF8String(defaults[i]);
                if (s) chain = chain->appendLast(ctx, s);
            }
            return chain;
        }

        // Acquire a FreeChunk struct (from pool, or fresh allocation).
        // Caller holds globalMutex.
        ProtoSpace::FreeChunk* takeFreeChunk(ProtoSpace* space) {
            if (space->freeChunkPool) {
                ProtoSpace::FreeChunk* c = space->freeChunkPool;
                space->freeChunkPool = c->next;
                return c;
            }
            return new ProtoSpace::FreeChunk();
        }

        // Return a FreeChunk struct to the pool for reuse.  Caller holds globalMutex.
        void recycleFreeChunk(ProtoSpace* space, ProtoSpace::FreeChunk* chunk) {
            chunk->head = nullptr;
            chunk->tail = nullptr;
            chunk->count = 0;
            chunk->next = space->freeChunkPool;
            space->freeChunkPool = chunk;
        }

        // Publish an accumulated chunk of dead cells to the global freeChunks
        // list.  Caller MUST hold globalMutex.  The chunk's tail must already
        // have its `next` pointing at nullptr (terminator).
        void publishFreeChunk(ProtoSpace* space, Cell* head, Cell* tail, unsigned long count) {
            if (!head || !tail || count == 0) return;
            tail->internalSetNextRaw(nullptr);
            ProtoSpace::FreeChunk* chunk = takeFreeChunk(space);
            chunk->head = head;
            chunk->tail = tail;
            chunk->count = count;
            chunk->next = space->freeChunks;
            space->freeChunks = chunk;
            space->freeCellsCount += count;
        }

        const char* cellTypeName(CellType type) {
            switch (type) {
                case CellType::None: return "None";
                case CellType::Object: return "Object";
                case CellType::List: return "List";
                case CellType::Tuple: return "Tuple";
                case CellType::String: return "String";
                case CellType::SparseList: return "SparseList";
                case CellType::Method: return "Method";
                case CellType::ExternalPointer: return "ExternalPointer";
                case CellType::ExternalBuffer: return "ExternalBuffer";
                case CellType::Thread: return "Thread";
                case CellType::LargeInteger: return "LargeInteger";
                case CellType::Double: return "Double";
                case CellType::Set: return "Set";
                case CellType::Multiset: return "Multiset";
                case CellType::MethodCell: return "MethodCell";
                case CellType::SparseListIterator: return "SparseListIterator";
                case CellType::ListIterator: return "ListIterator";
                case CellType::TupleIterator: return "TupleIterator";
                case CellType::StringIterator: return "StringIterator";
                case CellType::RangeIterator: return "RangeIterator";
                case CellType::SetIterator: return "SetIterator";
                case CellType::MultisetIterator: return "MultisetIterator";
                case CellType::ReturnReference: return "ReturnReference";
                case CellType::ParentLink: return "ParentLink";
                case CellType::ThreadExtension: return "ThreadExtension";
                case CellType::ByteBuffer: return "ByteBuffer";
                case CellType::TupleDictionary: return "TupleDictionary";
                case CellType::StringLeafNode: return "StringLeafNode";
                case CellType::StringInternalNode: return "StringInternalNode";
                case CellType::ListSmall: return "ListSmall";
                case CellType::SparseListSmall: return "SparseListSmall";
                case CellType::Map: return "Map";
                case CellType::MapSmall: return "MapSmall";
                case CellType::MapIterator: return "MapIterator";
            }
            return "unknown";
        }

        // Push a child that `parent->processReferences` reported onto the
        // mark work list.  Called from the GC thread only.
        //
        // A reported child is never nullptr: every implementation loads each
        // reference field once and reports it only when
        // ProtoObject::asCellPointer yields a cell.  A null here therefore
        // means a broken implementation.  Instrumented and debug builds stop
        // and name the reporting cell's type, so the offender is identified
        // instead of masked; other builds push it, and the work-list pop
        // skips null entries.
        inline void pushReportedReference(std::vector<const Cell*>* workList,
                                          const Cell* parent,
                                          const Cell* ref,
                                          const char* phase) {
            if (reinterpret_cast<uintptr_t>(ref) & 1) {
                std::cerr << "CRITICAL TAGGED POINTER (" << phase << "): " << ref
                          << " from parent " << parent
                          << " type " << (int)parent->getType() << std::endl;
                std::abort();
            }
#if defined(PROTOCORE_GC_INSTRUMENT) || !defined(NDEBUG)
            if (!ref) {
                std::fprintf(stderr,
                    "protoCore GC: null reference reported by a cell of type %d (%s) at %p during %s\n",
                    static_cast<int>(parent->getType()), cellTypeName(parent->getType()),
                    static_cast<void*>(const_cast<Cell*>(parent)), phase);
                std::fflush(stderr);
                std::abort();
            }
#endif
            workList->push_back(ref);
        }

        // Phase 5b: removes from the mutables tree the entries of the mutable
        // objects whose handles this cycle's sweep finalized.  Runs on the GC
        // thread after sweep, unlocked, concurrently with the mutators.
        //
        // ProtoObjectCell::finalize only records a collected handle's
        // mutable_ref in space->gcFinalizedMutableRefs; finalizers never
        // allocate or publish.  Here the refs are grouped by shard.  For each
        // shard the live root is loaded, every recorded ref present in it is
        // removed, and the result is published with one compare-and-swap.  A
        // failed CAS means a mutator published to the shard meanwhile: the
        // root is reloaded and the shard's batch redone from it, so neither
        // side loses an update.  A ref that is absent (a mutable never
        // written) costs a probe and no allocation.
        //
        // Soundness (docs/GarbageCollector.md § "Phase 5b"):
        //  * an unmarked handle is unreachable, and exactly one cell carries
        //    a given mutable_ref (fresh values from nextMutableRef, never
        //    reused; states carry 0), so no live object uses a removed entry;
        //  * gcMutableSnapshot still holds the entries and mark already
        //    traced the states through it, so they survive this cycle and
        //    are freed in the next one;
        //  * a root loaded after the stop-the-world is marked or young, never
        //    a candidate of this cycle, and only this thread frees cells, so
        //    a pending CAS cannot suffer ABA.
        //
        // Path copies are allocated through space->gcContext.  No root scan
        // can observe them half-built, because the GC thread is the only
        // thread that starts a stop-the-world; the context's young generation
        // is submitted at the end, so they are candidates of the next cycle.
        void releaseFinalizedMutableEntries(ProtoSpace* space) {
            std::vector<unsigned long>& refs = space->gcFinalizedMutableRefs;
            if (refs.empty()) return;
            ProtoContext* gc = space->gcContext;
            constexpr unsigned long kShards = ProtoSpace::MUTABLE_ROOT_SHARDS;

            std::sort(refs.begin(), refs.end(), [](unsigned long a, unsigned long b) {
                const unsigned long sa = a % kShards;
                const unsigned long sb = b % kShards;
                return sa != sb ? sa < sb : a < b;
            });

            std::size_t begin = 0;
            while (begin < refs.size()) {
                const unsigned long shard = refs[begin] % kShards;
                std::size_t end = begin + 1;
                while (end < refs.size() && refs[end] % kShards == shard) ++end;

                auto& slot = space->mutableRoot[shard].root;
                for (int attempt = 1;; ++attempt) {
                    ProtoSparseList* oldRoot = slot.load(std::memory_order_acquire);
                    if (!oldRoot) break;
                    const ProtoSparseList* newRoot = oldRoot;
                    for (std::size_t i = begin; i < end; ++i) {
                        if (sparseListGetRaw(gc, newRoot, refs[i]) == nullptr) continue;
                        newRoot = newRoot->removeAt(gc, refs[i]);
                    }
                    if (newRoot == oldRoot) break;  // none of the refs is present
                    ProtoSparseList* expected = oldRoot;
                    if (slot.compare_exchange_strong(expected, const_cast<ProtoSparseList*>(newRoot),
                                                     std::memory_order_acq_rel)) {
                        break;
                    }
                    // A mutator published to this shard: redo the batch from
                    // the new root, yielding now and then like the writers.
                    if ((attempt & 31) == 0) std::this_thread::yield();
                }
                begin = end;
            }
            refs.clear();  // keeps the capacity for the next cycle

            // Hand the path copies, including those of lost attempts, to the
            // collector: the same handover as the threshold submission in
            // ProtoContext::safepoint.
            while (gc->lock.test_and_set(std::memory_order_acquire)) {}
            Cell* chain = gc->lastAllocatedCell;
            gc->lastAllocatedCell = nullptr;
            gc->allocatedCellsCount = 0;
            gc->lock.clear(std::memory_order_release);
            if (chain) space->submitYoungGeneration(chain);
        }

        void gcThreadLoop(ProtoSpace* space) {
            std::unique_lock<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            GC_LOCK_TRACE("gcLoop ACQ(init)");
#ifdef PROTOCORE_GC_INSTRUMENT
            // Per-phase running totals + cycle count, printed every 5
            // cycles when env var PROTOCORE_GC_PROFILE is set.  Compile
            // out by default; enable with cmake -DPROTOCORE_GC_INSTRUMENT=ON.
            static std::atomic<uint64_t> dbg_total_phase1_us{0};
            static std::atomic<uint64_t> dbg_total_phase2_us{0};
            static std::atomic<uint64_t> dbg_total_phase4_us{0};
            static std::atomic<uint64_t> dbg_total_phase5_us{0};
            static std::atomic<uint64_t> dbg_total_phase6_us{0};
            // Phase 5b, the release of mutables-tree entries (REL= in the line).
            static std::atomic<uint64_t> dbg_total_release_us{0};
            static std::atomic<uint64_t> dbg_total_cells_marked{0};
            static std::atomic<uint64_t> dbg_total_segments_swept{0};
            const bool dbg_profile = std::getenv("PROTOCORE_GC_PROFILE") != nullptr;
#endif
            while (space->state != SPACE_STATE_ENDING) {
                // Wait for a GC trigger or space ending
                space->gcCV.wait(lock, [space] {
                    return space->gcStarted || space->state == SPACE_STATE_ENDING;
                });
                GC_LOCK_TRACE("gcLoop ACQ(gcStarted)");
                if (space->state == SPACE_STATE_ENDING) break;
#ifdef PROTOCORE_GC_INSTRUMENT
                auto t_phase1_start = std::chrono::steady_clock::now();
#endif
                
                // --- PHASE 1: STOP THE WORLD ---
                space->stwFlag.store(true);
                // We need to wait until ALL other threads are parked
                // runningThreads includes all application threads.
                // GC thread doesn't increment/decrement runningThreads.
                // We need to wait until ALL other threads are parked
                // runningThreads includes all application threads.
                // GC thread doesn't increment/decrement runningThreads.
                space->gcCV.wait(lock, [space] {
                    return space->parkedThreads.load() >= space->runningThreads.load() || space->state == SPACE_STATE_ENDING;
                });
                GC_LOCK_TRACE("gcLoop ACQ(parked)");
                if (space->state == SPACE_STATE_ENDING) break;
#ifdef PROTOCORE_GC_INSTRUMENT
                auto t_phase2_start = std::chrono::steady_clock::now();
                dbg_total_phase1_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        t_phase2_start - t_phase1_start).count(),
                    std::memory_order_relaxed);
#endif

                // --- PHASE 2: COLLECT ROOTS ---
                std::vector<const Cell*> workList;
                // One root handle per context for its young generation: the
                // chain head.  The chains are walked after the world resumes.
                std::vector<const Cell*> youngChainHeads;
                auto addRootObj = [&](const ProtoObject* obj) {
                    if (ProtoObject::isCellPointer(obj)) {
                        workList.push_back(ProtoObject::asCellPointer(obj));
                    }
                };

                auto scanContexts = [&](ProtoContext* currentCtx) {
                    while (currentCtx) {
                        // Roots: Automatic locals
                        for (unsigned int i = 0; i < currentCtx->getAutomaticLocalsCount(); ++i) {
                            addRootObj(currentCtx->getAutomaticLocals()[i]);
                        }
                        // Roots: Closure locals
                        if (currentCtx->closureLocals) {
                            addRootObj(reinterpret_cast<const ProtoObject*>(currentCtx->closureLocals));
                        }
                        // Roots: Return value
                        if (currentCtx->returnValue) {
                            addRootObj(currentCtx->returnValue);
                        }
                        // Push pending root
                        if (currentCtx->pendingRoot) {
                            workList.push_back(currentCtx->pendingRoot);
                        }
                        
                        // Roots: the young generation (cells allocated in this
                        // context and not yet submitted).  They are not
                        // candidates of this cycle, because they are not in the
                        // segments captured below, and they are not marked.
                        // Only the chain head is recorded here, O(1) per
                        // context; the concurrent mark walks the chain and
                        // pushes the references of its cells (see the young-
                        // chain walk after the world resumes).
                        while (currentCtx->lock.test_and_set(std::memory_order_acquire)) {}
                        const Cell* youngHead = currentCtx->lastAllocatedCell;
                        currentCtx->lock.clear(std::memory_order_release);
                        if (youngHead) youngChainHeads.push_back(youngHead);

                        currentCtx = currentCtx->previous;
                    }
                };

                // 1. Scan Thread Stacks.  The threads list may now be in
                // either form (Small with ≤ 3 entries — typical for the
                // common 1-3-thread process; AVL otherwise).  Branch on
                // pointer tag and walk inline pairs for Small, AVL nodes
                // otherwise.
                if (space->threads) {
                    const ProtoObject* rootObj = reinterpret_cast<const ProtoObject*>(space->threads);
                    if (ProtoObject::isCellPointer(rootObj)) {
                        ProtoObjectPointer pa{}; pa.oid = rootObj;
                        if (pa.op.pointer_tag == POINTER_TAG_SPARSE_LIST_SMALL) {
                            const auto* small = toImpl<const ProtoSparseListSmallImplementation>(rootObj);
                            for (unsigned i = 0; i < ProtoSparseListSmallImplementation::MAX_INLINE; ++i) {
                                if (small->keys[i] == 0) continue;
                                const ProtoObject* v = small->values[i];
                                if (v && v->asThread(space->rootContext)) {
                                    scanContexts(toImpl<const ProtoThreadImplementation>(v->asThread(space->rootContext))->context);
                                }
                            }
                        } else {
                            std::vector<const ProtoSparseListImplementation*> stack;
                            stack.push_back(toImpl<const ProtoSparseListImplementation>(rootObj));
                            while (!stack.empty()) {
                                const ProtoSparseListImplementation* node = stack.back();
                                stack.pop_back();
                                if (!node->isEmpty && node->value && node->value->asThread(space->rootContext)) {
                                    scanContexts(toImpl<const ProtoThreadImplementation>(node->value->asThread(space->rootContext))->context);
                                }
                                if (node->previous) stack.push_back(node->previous);
                                if (node->next) stack.push_back(node->next);
                            }
                        }
                    }
                }
                
                // 2. Global Roots
                addRootObj(space->objectPrototype);
                addRootObj(space->booleanPrototype);
                addRootObj(space->unicodeCharPrototype);
                addRootObj(space->listPrototype);
                addRootObj(space->listIteratorPrototype);
                addRootObj(space->tuplePrototype);
                addRootObj(space->tupleIteratorPrototype);
                addRootObj(space->stringPrototype);
                addRootObj(space->stringIteratorPrototype);
                addRootObj(space->sparseListPrototype);
                addRootObj(space->sparseListIteratorPrototype);
                addRootObj(space->mapPrototype);
                addRootObj(space->setPrototype);
                addRootObj(space->setIteratorPrototype);
                addRootObj(space->multisetPrototype);
                addRootObj(space->multisetIteratorPrototype);
                addRootObj(space->rangeIteratorPrototype);
                addRootObj(space->smallIntegerPrototype);
                addRootObj(space->largeIntegerPrototype);
                addRootObj(space->floatPrototype);
                addRootObj(space->doublePrototype);
                addRootObj(space->bytePrototype);
                addRootObj(space->nonePrototype);
                addRootObj(space->methodPrototype);
                addRootObj(space->bufferPrototype);
                addRootObj(space->pointerPrototype);
                addRootObj(space->datePrototype);
                addRootObj(space->timestampPrototype);
                addRootObj(space->timedeltaPrototype);
                addRootObj(space->threadPrototype);
                addRootObj(space->rootObject);
                // literalData is a strong Symbol — covered by the SymbolTable sweep below
                if (space->resolutionChain_) addRootObj(space->resolutionChain_->asObject(space->rootContext));

                {
                    std::lock_guard<std::mutex> modLock(space->moduleRootsMutex);
                    for (const ProtoObject* mod : space->moduleRoots) {
                        addRootObj(mod);
                    }
                }

                // Tuple interner.  Interned tuples are perennial and the
                // table is a root.  Record only each shard's published
                // entry count here — O(shards) under STW; mark (Phase 4)
                // walks those entries outside the STW window while
                // mutators keep appending.  See TupleInterner.
                if (space->tupleInterner) space->tupleInterner->captureForGC();

                // stringInternMap is intentionally NOT scanned.  The map
                // is held empty for the lifetime of the process:
                // internString() is dead code (never called from any
                // current path — see core/ProtoString.cpp lines 290 and
                // 1195 for why interning was abandoned), and the
                // canonical symbol table is SymbolTable, whose entries
                // are perennial and also not scanned (see comment
                // above).  Scanning an always-empty set was pure GC
                // overhead.  The map infrastructure is left in place
                // (no ABI change) but never iterated.

                // Symbols are NOT scanned here.  Every interned string is
                // perennial: SymbolTable::intern always builds the symbol
                // with a null ProtoContext, so its Cells are allocated via
                // posix_memalign directly — never enrolled in any thread
                // freelist or context young chain.  The GC's mark/sweep
                // machinery therefore never sees a symbol Cell as a
                // candidate, there is nothing to protect, and iterating
                // every SymbolTable shard on every GC cycle would be pure
                // overhead.  (There is no longer any "weak"/collectible
                // symbol variant.)

                // Phase 2 — capture the mutable-shard snapshot under STW.
                //
                // This is the formal "snapshot at the beginning" for the
                // concurrent mark that follows.  Workers are parked here
                // (Phase 1 quorum reached), so each per-shard load is
                // atomic by construction — no fence beyond `acquire` is
                // required.  Post-STW the workers will resume and may
                // CAS-swap shard roots freely; the marker never reads
                // the live table again, only this snapshot.
                //
                // Each shard root is also pushed to workList so mark
                // traces the entire mutable graph as it existed at STW
                // time.  The graph reachable from the snapshot is fully
                // immutable (every Cell field is const-qualified after
                // construction), so the marker walks a frozen view.
                //
                // Cost: O(MUTABLE_ROOT_SHARDS) atomic reads, ~2 KB store.
                // Independent of heap size or live-object count.  No write
                // barriers anywhere in the runtime — the snapshot replaces
                // them.  See docs/STW_ELIMINATION_RESEARCH.md § 13.
                for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
                    ProtoSparseList* r =
                        space->mutableRoot[s].root.load(std::memory_order_acquire);
                    space->gcMutableSnapshot[s] = r;
                    if (r) addRootObj(reinterpret_cast<const ProtoObject*>(r));
                }
                if (space->threads) addRootObj(reinterpret_cast<const ProtoObject*>(space->threads));
                
                // Scan embedder-registered root sets.  Each set owns a
                // private collection of `ProtoObject*` pinned by a
                // runtime built on protoCore (e.g. protoJS callbacks
                // captured into C++ lambdas).  See ProtoRootSet for
                // the rationale; we are inside STW here, mutators are
                // parked, so no add/remove can race with iteration.
                //
                // The visit functions are non-capturing C function
                // pointers because the ProtoSpace API only exposes
                // that signature (kept C-friendly for FFIs).  We
                // route the addRootObj closure through a stack
                // context structure passed as `user`.
                struct RootSetVisitCtx {
                    std::vector<const Cell*>* workList;
                };
                RootSetVisitCtx rsCtx{&workList};
                space->forEachRootSet(
                    [](void* user, ProtoRootSet* rs) {
                        rs->forEachRoot(
                            [](void* u, const ProtoObject* obj) {
                                auto* c = static_cast<RootSetVisitCtx*>(u);
                                if (ProtoObject::isCellPointer(obj)) {
                                    c->workList->push_back(
                                        ProtoObject::asCellPointer(obj));
                                }
                            },
                            user);
                    },
                    &rsCtx);

                // 3. Scan the Main Thread stack
                scanContexts(space->mainContext);

                // 6. Capture the heap snapshot (segments to process)
                // This MUST be done during STW to ensure we only sweep what existed at root collection.
                //
                // Survivor pen — staggered re-chain.  The previous sweep
                // pushed survivors into space->survivorPen instead of
                // dirtySegments.  Stop-the-world only captures the pen, in
                // O(1); the work on its segments runs after the world
                // resumes.  Two treatments, mutually exclusive within a
                // cycle:
                //
                //   FOLD CYCLE  (gcCycleCount % stagger == 0):
                //     Take the whole pen (exchange).  After the world
                //     resumes, its segments are linked in front of this
                //     cycle's segmentsToProcess, so their cells are
                //     candidates again.  Mark from roots reaches the live
                //     ones; outgoing references are traced via the normal
                //     candidate path.
                //
                //   NON-FOLD CYCLE (stagger > 1 only):
                //     Pen cells stay in the pen this cycle (skip mark and
                //     sweep cost).  But if a pen cell references a cell
                //     that IS a candidate, mark must still trace through
                //     it or the referenced cell is freed and the pen
                //     reference dangles.  Record the pen head; after the
                //     world resumes, mark walks each pen cell and pushes
                //     its outgoing references, as for the young chains.
                //     The pen cell itself is not pushed (not a candidate,
                //     no liveness check).
                //
                // With stagger == 1 (default) every cycle is a fold cycle.
                // With stagger > 1, only every Nth cycle folds; survivors
                // skip mark/sweep cost in the meantime, at the price of
                // delayed reclamation by up to N cycles.
                // Count every cycle, whatever the survivor configuration:
                // getGCCycleCount() and heap-limit reclamation waits read it.
                [[maybe_unused]] const uint64_t newCycle =
                    space->gcCycleCount.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
                const unsigned int stagger = space->survivorStagger ? space->survivorStagger : 1;
                const bool foldThisCycle = ((newCycle % stagger) == 0);
                // At most one of the two is non-null.
                DirtySegment* penToFold = nullptr;
                const DirtySegment* penToScan = nullptr;
                if (foldThisCycle) {
                    penToFold = space->survivorPen.exchange(nullptr, std::memory_order_acquire);
                } else {
                    penToScan = space->survivorPen.load(std::memory_order_acquire);
                }
#endif

                DirtySegment* segmentsToProcess = space->dirtySegments.exchange(nullptr, std::memory_order_acquire);

                // --- PHASE 3: RESUME THE WORLD ---
                //
                // The STW window closes HERE — before the mark phase, not
                // after it.  Mark now runs concurrent with user threads,
                // safely, because:
                //
                //   1. The mutable-shard snapshot captured in Phase 2
                //      (gcMutableSnapshot[]) gives the marker a frozen
                //      view of every reachable mutable.  Workers may CAS
                //      mutableRoot[s] freely; the marker never reads the
                //      live table.
                //
                //   2. Every Cell field traced by processReferences is
                //      const-qualified after construction.  Workers
                //      cannot mutate the fields the marker reads.
                //
                //   3. The mark bit on Cell::next_and_flags is touched
                //      ONLY by the GC thread.  Workers never call
                //      mark() / unmark() / isMarked().
                //
                //   4. segmentsToProcess was exchange()d atomically
                //      above; new dirty segments submitted by workers
                //      post-STW go to a fresh space->dirtySegments and
                //      survive to the next cycle.
                //
                //   5. New cells allocated by workers post-STW live in
                //      their per-context young chain, never in this
                //      cycle's segmentsToProcess — sweep does not see
                //      them.
                //
                // The change is described in detail in
                // docs/GarbageCollector.md § "Concurrent Mark Without
                // Barriers" and docs/STW_ELIMINATION_RESEARCH.md § 13.
                space->stwFlag.store(false);
                space->stopTheWorldCV.notify_all();
                GC_LOCK_TRACE("gcLoop REL(mark)");
                lock.unlock(); // Mark, sweep, and bulk-unmark all run unlocked.
#ifdef PROTOCORE_GC_INSTRUMENT
                // P2 ends here, where the world resumes, so P1 + P2 is the
                // stop-the-world pause.  Everything below counts as mark (P4).
                auto t_phase4_start = std::chrono::steady_clock::now();
                dbg_total_phase2_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        t_phase4_start - t_phase2_start).count(),
                    std::memory_order_relaxed);
#endif

                // Interned tuples recorded by Phase 2 are roots.
                if (space->tupleInterner) {
                    space->tupleInterner->forEachCaptured(&workList, [](void* user, const Cell* tuple) {
                        static_cast<std::vector<const Cell*>*>(user)->push_back(tuple);
                    });
                }

                // Young chains captured in Phase 2.  Their cells are neither
                // candidates of this cycle nor marked; the references they
                // hold are pushed so mark reaches every object a young cell
                // refers to.  The walk runs while the mutators run, over a
                // stable view:
                //   * a chain grows only by prepending (addCell2Context sets
                //     the new cell's next to the current head, then publishes
                //     the new cell as head), so cells allocated after the
                //     capture sit in front of the captured head and no link
                //     behind it is rewritten;
                //   * submitting a chain (context destruction, safepoint
                //     threshold) hands its head to a DirtySegment without
                //     touching links; a segment pushed after the capture
                //     belongs to the next cycle, which cannot start before
                //     this mark ends;
                //   * the other writers of cell links are the freelist paths,
                //     which own free cells, and this cycle's sweep, which runs
                //     after mark on the segments captured in Phase 2 —
                //     disjoint from live young chains, because a submission
                //     detaches the chain from its context;
                //   * a reference field is written once, at construction, so
                //     a cell still being constructed at the capture is read
                //     with either its initial null or its final value.
                for (const Cell* head : youngChainHeads) {
                    struct YoungWalkState { std::vector<const Cell*>* wl; const Cell* parent; } yst = {&workList, head};
                    for (const Cell* scanCell = head; scanCell; scanCell = scanCell->getNext()) {
                        yst.parent = scanCell;
                        scanCell->processReferences(space->rootContext, &yst, [](ProtoContext* ctx, void* self, const Cell* ref) {
                            auto* s = static_cast<YoungWalkState*>(self);
                            pushReportedReference(s->wl, s->parent, ref, "Phase4(young)");
                        });
                    }
                }

#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
                // Survivor pen captured in Phase 2.
                //
                // Fold: the exchanged pen is private to this thread.  Linking
                // it in front of segmentsToProcess builds the list sweep
                // walks; the candidate set itself was fixed by the two
                // exchanges under stop-the-world.
                if (penToFold) {
                    DirtySegment* penTail = penToFold;
                    while (penTail->next) penTail = penTail->next;
                    penTail->next = segmentsToProcess;
                    segmentsToProcess = penToFold;
                }
                // Non-fold: walk the pen captured in Phase 2 and push the
                // references of its cells.  The view is stable: mutators
                // never touch the pen or the links of its cells, this
                // cycle's sweep runs after mark and only prepends new
                // survivor segments at the pen head, and the pen is folded
                // only under a later cycle's stop-the-world.
                for (const DirtySegment* penSeg = penToScan; penSeg; penSeg = penSeg->next) {
                    struct PenScanState { std::vector<const Cell*>* wl; const Cell* parent; } pst = {&workList, penSeg->cellChain};
                    for (const Cell* scanCell = penSeg->cellChain; scanCell; scanCell = scanCell->getNext()) {
                        pst.parent = scanCell;
                        scanCell->processReferences(space->rootContext, &pst, [](ProtoContext* ctx, void* self, const Cell* ref) {
                            auto* s = static_cast<PenScanState*>(self);
                            pushReportedReference(s->wl, s->parent, ref, "Phase4(pen)");
                        });
                    }
                }
#endif

                // --- PHASE 4: MARK (concurrent with mutators) ---
                //
                // Sweep only clears mark bits on cells inside
                // segmentsToProcess; cells outside that set (young
                // cells whose owning context never submitted, perpetual
                // prototypes, tuple/string interner entries) used to
                // retain stale mark=1 from prior cycles, which made
                // the next cycle's `if (!isMarked())` skip them and
                // their transitive closure — corrupting the live
                // graph (see commit 0441247e).
                //
                // The previous fix (pre-mark pass that walked the live
                // graph and unmarked every reachable cell, deduped via
                // std::set<const Cell*>) restored correctness but at
                // O(N log N) per cycle on the std::set inserts.  That
                // dominated CPU time on attribute-heavy workloads:
                // perf record on `binary_trees(10)` showed
                // _Rb_tree::_M_insert_unique alone at 15.8 % of total
                // CPU, with gcThreadLoop frame-time at 26 %, and the
                // bench dropped from 2.2 s/iter (2026-05-01) to ~12 s
                // /iter (2026-05-04).
                //
                // Replacement: track the cells we mark right here in
                // Phase 4 (one push_back per Cell, deduped naturally
                // by the existing `if (!isMarked())` guard), then
                // unmark them in a separate post-sweep pass.  Same
                // tricolour invariant restored, no std::set, O(N)
                // pure pushes — and the markedList is contiguous
                // memory so traversing it post-sweep is cache-
                // friendly.
                //
                // Why post-sweep instead of pre-mark next cycle: it
                // localises the work to "the GC cycle that produced
                // the marks", so a single iteration of gcThreadLoop
                // is self-contained — no cross-iteration state
                // beyond the cell mark bits themselves.
                std::vector<const Cell*> markedList;
                while (!workList.empty()) {
                    const Cell* cell = workList.back();
                    workList.pop_back();
                    // The single filter for null work-list entries, whatever
                    // pushed them: a root holding a tagged null (non-embedded
                    // tag, no pointer bits) or, in builds that do not abort on
                    // it, a null child reported by processReferences.
                    if (!cell) continue;
                    // Prefetch the NEXT cell to be popped — the mark
                    // phase, like sweep, is a pointer-chasing loop
                    // where each iteration loads
                    // `cell->next_and_flags` to read the mark bit.
                    // Prefetching the lookahead pop overlaps the
                    // cache-line miss with the current cell's
                    // mark + processReferences work.
                    if (!workList.empty()) {
                        const Cell* nextCell = workList.back();
                        if (nextCell && (reinterpret_cast<uintptr_t>(nextCell) & 0x3F) == 0) {
                            __builtin_prefetch(nextCell, 1, 1);
                        }
                    }

                    if (!cell->isMarked()) {
                        const_cast<Cell*>(cell)->mark();
                        markedList.push_back(cell);
                        struct GCLambdaState { std::vector<const Cell*>* wl; const Cell* parent; } state = {&workList, cell};
                        cell->processReferences(space->rootContext, &state, [](ProtoContext* ctx, void* self, const Cell* ref) {
                            auto* s = static_cast<GCLambdaState*>(self);
                            pushReportedReference(s->wl, s->parent, ref, "Phase4(mark)");
                        });
                    }
                }

                // Phase 3 (Resume The World) already happened above —
                // BEFORE the mark loop, not after.  Mark ran concurrent
                // with the mutators using gcMutableSnapshot.  Sweep
                // below continues to run unlocked, as it always did.
#ifdef PROTOCORE_GC_INSTRUMENT
                auto t_phase5_start = std::chrono::steady_clock::now();
                dbg_total_phase4_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        t_phase5_start - t_phase4_start).count(),
                    std::memory_order_relaxed);
                dbg_total_cells_marked.fetch_add(
                    markedList.size(), std::memory_order_relaxed);
                {
                    DirtySegment* s = segmentsToProcess;
                    uint64_t segCount = 0;
                    while (s) { segCount++; s = s->next; }
                    dbg_total_segments_swept.fetch_add(segCount, std::memory_order_relaxed);
                }
#endif

                // --- PHASE 5: SWEEP ---
                //
                // Cross-segment chunk accumulator (path #5 v2).  Dead cells
                // from each segment are chained into a running chunk; when
                // the chunk reaches CELL_CHUNK_SIZE, it is published to
                // `space->freeChunks` and a fresh chunk starts.  At the end
                // of sweep the partial trailing chunk is published with its
                // actual count.  This converts getFreeCells from an O(N)
                // walk-and-cut into an O(1) chunk pop.
                Cell* chunkHead = nullptr;
                Cell* chunkTail = nullptr;
                unsigned long chunkCount = 0;
                // Total cells reclaimed this cycle, across all published
                // chunks — the out-of-memory signal (see reclaimedLastCycle).
                unsigned long reclaimedThisCycle = 0;

                DirtySegment* currentSeg = segmentsToProcess;
                while (currentSeg) {
                    Cell* cell = currentSeg->cellChain;

                    Cell* batchHead = nullptr;
                    Cell* batchTail = nullptr;
                    int batchCount = 0;
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
                    // Cells that survived this cycle (were reachable from
                    // roots) are chained here and re-pushed to dirtySegments
                    // so the next cycle includes them in its candidate set.
                    // Without this re-inclusion, a cell that survives once
                    // is dropped from analysis forever and leaks when it
                    // later becomes unreachable.
                    Cell* survHead = nullptr;
#endif

                    while (cell) {
                        Cell* nextCell = cell->getNext();
                        if (!cell->isMarked()) {
                            cell->finalize(space->rootContext);

                            cell->internalSetNextRaw(batchHead);
                            if (!batchTail) batchTail = cell;
                            batchHead = cell;
                            batchCount++;
                        } else {
                            cell->unmark();
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
                            // Prepend to the survivor chain.  Use setNext
                            // (not internalSetNextRaw) so the cached
                            // cellType bits in next_and_flags survive — the
                            // cell stays live and queryable.  The old next
                            // pointer is no longer needed: we already saved
                            // the iteration cursor in `nextCell` above and
                            // currentSeg->cellChain is being torn down.
                            cell->setNext(survHead);
                            survHead = cell;
#endif
                        }
                        cell = nextCell;
                    }

                    if (batchHead) {
                        // Merge this segment's batch into the running chunk.
                        // The previous batchTail terminator is overwritten
                        // when chaining onto the next batch (chunkHead is
                        // prepended).  Final terminator is set inside
                        // publishFreeChunk.
                        if (chunkHead) {
                            // Prepend: batch's tail points at the previous
                            // chunkHead; chunkHead becomes batch's head.
                            // Order doesn't matter for the freelist —
                            // mutators consume cells one by one regardless.
                            batchTail->internalSetNextRaw(chunkHead);
                        } else {
                            chunkTail = batchTail;
                        }
                        chunkHead = batchHead;
                        chunkCount += batchCount;
                        reclaimedThisCycle += batchCount;

                        // If we've crossed the chunk-size threshold, publish.
                        // Most segments are small (~5.86 cells avg) so this
                        // typically fires only once per ~1400 segments, well
                        // below the per-segment lock cost we used to pay.
                        if (chunkCount >= ProtoSpace::CELL_CHUNK_SIZE) {
                            GC_LOCK_TRACE("gcLoop ACQ(chunk)");
                            std::lock_guard<std::recursive_mutex> chunkLock(ProtoSpace::globalMutex);
                            publishFreeChunk(space, chunkHead, chunkTail, chunkCount);
                            chunkHead = chunkTail = nullptr;
                            chunkCount = 0;
                            GC_LOCK_TRACE("gcLoop REL(chunk)");
                        }
                    }

                    DirtySegment* nextSeg = currentSeg->next;
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
                    if (survHead) {
                        // Repurpose currentSeg as a survivor segment and
                        // push it to the survivor pen.  When stagger == 1
                        // (default) the pen is folded back into
                        // dirtySegments at the start of every cycle, so
                        // this is equivalent to the previous direct push
                        // to dirtySegments — only one extra atomic-list
                        // hop on the cold path.  When stagger > 1 the pen
                        // is folded only every Nth cycle, so survivors
                        // skip mark cost in the meantime (at the price of
                        // delayed reclamation; see survivorStagger doc).
                        currentSeg->cellChain = survHead;
                        currentSeg->next = space->survivorPen.load(std::memory_order_relaxed);
                        while (!space->survivorPen.compare_exchange_weak(
                                currentSeg->next, currentSeg,
                                std::memory_order_release,
                                std::memory_order_relaxed)) {
                            // currentSeg->next updated by compare_exchange_weak on failure
                        }
                    } else {
                        // No survivors: recycle to the free pool as before.
                        currentSeg->cellChain = nullptr;
                        currentSeg->next = space->dirtySegmentFreePool.load(std::memory_order_relaxed);
                        while (!space->dirtySegmentFreePool.compare_exchange_weak(
                                currentSeg->next, currentSeg,
                                std::memory_order_release,
                                std::memory_order_relaxed)) {
                            // currentSeg->next updated by compare_exchange_weak on failure
                        }
                    }
#else
                    // Return the segment to the lock-free free pool so the
                    // next submitYoungGeneration() can recycle it.  The GC
                    // is the sole consumer here and the only thread that
                    // pushes during sweep, so a single CAS is enough.
                    currentSeg->cellChain = nullptr;
                    currentSeg->next = space->dirtySegmentFreePool.load(std::memory_order_relaxed);
                    while (!space->dirtySegmentFreePool.compare_exchange_weak(
                            currentSeg->next, currentSeg,
                            std::memory_order_release,
                            std::memory_order_relaxed)) {
                        // currentSeg->next updated by compare_exchange_weak on failure
                    }
#endif
                    currentSeg = nextSeg;
                }

                // Publish the trailing partial chunk (count < CELL_CHUNK_SIZE).
                // Common at end of sweep when the last few segments did not
                // accumulate enough cells to fill a chunk — the chunk is
                // valid at any size, so we simply hand it over.
                if (chunkHead) {
                    GC_LOCK_TRACE("gcLoop ACQ(chunk-tail)");
                    std::lock_guard<std::recursive_mutex> chunkLock(ProtoSpace::globalMutex);
                    publishFreeChunk(space, chunkHead, chunkTail, chunkCount);
                    GC_LOCK_TRACE("gcLoop REL(chunk-tail)");
                }

#ifdef PROTOCORE_GC_INSTRUMENT
                auto t_release_start = std::chrono::steady_clock::now();
                dbg_total_phase5_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        t_release_start - t_phase5_start).count(),
                    std::memory_order_relaxed);
#endif

                // --- PHASE 5b: RELEASE MUTABLES-TREE ENTRIES ---
                //
                // Sweep finalized the handles of unreachable mutable objects;
                // their finalizers recorded the mutable_refs.  Remove those
                // entries from mutableRoot now, one compare-and-swap per shard,
                // allocating through the collector's own context.  The states
                // they held were marked through this cycle's snapshot and are
                // freed next cycle.  See releaseFinalizedMutableEntries.
                releaseFinalizedMutableEntries(space);

#ifdef PROTOCORE_GC_INSTRUMENT
                auto t_phase6_start = std::chrono::steady_clock::now();
                dbg_total_release_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        t_phase6_start - t_release_start).count(),
                    std::memory_order_relaxed);
#endif

                // --- PHASE 6: BULK UNMARK (replaces the pre-mark pass) ---
                //
                // Walk the markedList collected in Phase 4 and unmark
                // every entry.  After this completes, ALL reachable
                // cells (segments-side and outside-segments alike)
                // satisfy mark=0, so the next cycle's mark phase sees
                // a clean tricolour state.
                //
                // Sweep already cleared the mark bit on the survivors
                // it kept (the `else { cell->unmark(); }` branch in
                // Phase 5).  For those entries this loop's
                // fetch_and(~0x1) is a benign no-op — atomic and
                // cheap.  The interesting work is on the cells that
                // were marked but live OUTSIDE segmentsToProcess
                // (perpetuals, unsubmitted-young, interners) — sweep
                // never touched their mark bit, and without this loop
                // they would carry mark=1 into the next cycle and
                // re-trigger the bug 0441247e fixed.
                //
                // Runs OUTSIDE the STW window (lock was released
                // before Phase 5) — same locking regime as sweep.
                // Mutators may be allocating fresh cells in parallel;
                // those new cells are not in markedList and start
                // with mark=0, so they are unaffected.  unmark() is
                // a single atomic fetch_and, safe regardless of
                // contention with mutator threads.
                for (const Cell* m : markedList) {
                    if (m && (reinterpret_cast<uintptr_t>(m) & 0x3F) == 0) {
                        const_cast<Cell*>(m)->unmark();
                    }
                }

                // --- PHASE 7: CLEAR MUTABLE SNAPSHOT ---
                //
                // The snapshot was the formal mark-time dereference
                // table for THIS cycle only.  Clear all entries so the
                // next cycle's Phase 2 starts from a known nullptr
                // baseline, and so any stray GC-side read of the table
                // outside a cycle observes nullptr instead of a stale
                // shard root.
                //
                // Safe to run unlocked (same regime as the bulk-unmark
                // loop above): only the GC thread reads or writes the
                // snapshot table.
                for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
                    space->gcMutableSnapshot[s] = nullptr;
                }
#ifdef PROTOCORE_GC_INSTRUMENT
                auto t_phase6_end = std::chrono::steady_clock::now();
                dbg_total_phase6_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        t_phase6_end - t_phase6_start).count(),
                    std::memory_order_relaxed);
                if (dbg_profile) {
                    uint64_t cycles = space->gcCycleCount.load(std::memory_order_relaxed);
                    // 2026-05-23 night: print every cycle (was every 5) to
                    // catch short benchmarks. Revert before merging if a
                    // long-running test floods stderr.
                    if (cycles > 0) {
                        std::fprintf(stderr,
                            "[GC-PROFILE] cycles=%lu  P1=%luus  P2=%luus  P4=%luus  P5=%luus  REL=%luus  P6=%luus  marked=%lu  swept_segs=%lu\n",
                            (unsigned long)cycles,
                            (unsigned long)dbg_total_phase1_us.load(),
                            (unsigned long)dbg_total_phase2_us.load(),
                            (unsigned long)dbg_total_phase4_us.load(),
                            (unsigned long)dbg_total_phase5_us.load(),
                            (unsigned long)dbg_total_release_us.load(),
                            (unsigned long)dbg_total_phase6_us.load(),
                            (unsigned long)dbg_total_cells_marked.load(),
                            (unsigned long)dbg_total_segments_swept.load());
                    }
                }
#endif

                GC_LOCK_TRACE("gcLoop ACQ(after-sweep)");
                lock.lock(); // Re-acquire for next wait
                space->gcStarted = false;
                // Publish the cycle's accounting for the heap-limit path:
                //  * reclaimedLastCycle — cells the sweep returned to the
                //    freelist; the authoritative out-of-memory signal
                //    (waitForHeapHeadroom: two zero-reclaim cycles == OOM).
                //  * liveCellsLastCycle — mark-phase reachable count, used
                //    only for the OOM abort's diagnostic message.
                // Then wake any thread parked waiting for the sweep to refill
                // the freelist (allocation-limit path).
                space->reclaimedLastCycle.store(reclaimedThisCycle,
                                                std::memory_order_relaxed);
                space->liveCellsLastCycle.store(markedList.size(),
                                                std::memory_order_relaxed);
                space->memoryReclaimedCV.notify_all();
                space->gcCV.notify_all();
            }
        }
    }
    
    std::recursive_mutex ProtoSpace::globalMutex;

    ProtoSpace::ProtoSpace() :
        state(SPACE_STATE_RUNNING),
        gcThread(nullptr),
        booleanPrototype(nullptr),
        unicodeCharPrototype(nullptr),
        listPrototype(nullptr),
        sparseListPrototype(nullptr),
        tuplePrototype(nullptr),
        stringPrototype(nullptr),
        setPrototype(nullptr),
        multisetPrototype(nullptr),
        rangeIteratorPrototype(nullptr),
        attributeNotFoundGetCallback(nullptr),
        nonMethodCallback(nullptr),
        parameterTwiceAssignedCallback(nullptr),
        parameterNotFoundCallback(nullptr),
        runningThreads(1), // Main thread starts running
        stwFlag(false),
        parkedThreads(0),
        blocksPerAllocation(8192),  // Larger default batch to reduce getFreeCells calls during script load (was 1024; 1151 calls observed for multithread benchmark)
        heapSize(0),
        maxHeapSize(0),
        softHeapLimit(0),
        freeCellsCount(0),
        gcSleepMilliseconds(10),
        tupleRoot(nullptr),
        stringInternMap(nullptr),
        dirtySegments(nullptr),
        dirtySegmentFreePool(nullptr),
        survivorPen(nullptr),
        survivorStagger(SURVIVOR_STAGGER_DEFAULT),
        gcCycleCount(0),
        liveCellsLastCycle(0),
        reclaimedLastCycle(0),
        freeCells(nullptr),
        freeCellsTail(nullptr),
        freeChunks(nullptr),
        freeChunkPool(nullptr),
        gcStarted(false),
        mainContext(nullptr),
        nextMutableRef(1),
        resolutionChain_(nullptr),
        rootContext(nullptr)
    {
        // Record the OS thread that created this space.  ProtoContext uses this
        // to auto-inherit the main thread's ProtoThreadImplementation when a
        // context is created with previous == nullptr (i.e. every runtime's
        // bootstrap context), enabling per-thread attribute and mutable-value
        // caches without requiring each runtime to pass rootContext explicitly.
        this->mainThreadId = std::this_thread::get_id();

        // The tuple interner must exist before the first tuple is built.
        this->tupleInterner = new TupleInterner();

        // Per-context GC threshold: env var override, fall back to default.
        // Only consumed when PROTOCORE_GC_REINCLUDE_SURVIVORS is enabled, but
        // initialised unconditionally so the field is well-defined.
        this->maxAllocatedCellsPerContext = CONTEXT_GC_THRESHOLD_DEFAULT;
        if (const char* envThreshold = std::getenv("PROTOCORE_GC_CONTEXT_THRESHOLD")) {
            char* endPtr = nullptr;
            unsigned long parsed = std::strtoul(envThreshold, &endPtr, 10);
            if (endPtr && *endPtr == '\0' && parsed > 0 && parsed <= UINT_MAX) {
                this->maxAllocatedCellsPerContext = static_cast<unsigned int>(parsed);
            }
        }

        // Survivor-pen stagger: how many GC cycles between successive
        // folds of the survivor pen back into dirtySegments.  Default 1
        // (re-check every cycle, no stagger).  Higher values reduce mark
        // cost on workloads with stable working sets at the price of
        // delayed reclamation (RSS may grow up to N × working set).
        this->survivorStagger = SURVIVOR_STAGGER_DEFAULT;
        if (const char* envStagger = std::getenv("PROTOCORE_GC_SURVIVOR_STAGGER")) {
            char* endPtr = nullptr;
            unsigned long parsed = std::strtoul(envStagger, &endPtr, 10);
            if (endPtr && *endPtr == '\0' && parsed >= 1 && parsed <= 256) {
                this->survivorStagger = static_cast<unsigned int>(parsed);
            }
        }

        // Pre-allocate a pool of DirtySegments so submitYoungGeneration's
        // hot path (per-context threshold submission, every context-
        // destruction) can claim one without falling back to a system
        // malloc.  Each DirtySegment is 16 bytes (two pointers), so the
        // initial reservation is small and amortises the cost of every
        // future submission against this single startup burst.
        //
        // Profile of protoPython str_concat_loop with the threshold
        // submission active showed _int_malloc inside libc as a top
        // hotspot — every miss in the pool walked
        // posix_memalign / new — when the burst rate exceeded the GC's
        // sweep cadence (sweep is the only path that returns segments).
        // The pre-allocation keeps the pool warm so steady-state
        // submission is allocation-free, and the existing fallback in
        // submitYoungGeneration still covers pathological pressure.
        constexpr int kInitialDirtySegmentPool = 128;
        for (int i = 0; i < kInitialDirtySegmentPool; ++i) {
            DirtySegment* seg = new DirtySegment();
            seg->cellChain = nullptr;
            seg->next = this->dirtySegmentFreePool.load(std::memory_order_relaxed);
            while (!this->dirtySegmentFreePool.compare_exchange_weak(
                    seg->next, seg,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                // seg->next reloaded by compare_exchange_weak on failure.
            }
        }

        // Initialize prototypes
        this->rootContext = new ProtoContext(this, nullptr, nullptr, nullptr, nullptr, nullptr);
        this->booleanPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->unicodeCharPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->listPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->sparseListPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->mapPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        // Mutable so embedders (protoJS Object.prototype, protoPython
        // object.__class__, etc.) can install methods and accept user-
        // level setattr without forking the identity on every write.
        // The cell still serves as the root of the prototype chain; it
        // is allocated through the root context so the GC tracks it.
        this->objectPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(true));
        this->tuplePrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->stringPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->setPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->multisetPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->rangeIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->threads = reinterpret_cast<ProtoSparseList*>(const_cast<ProtoObject*>(this->rootContext->newSparseList()->asObject(this->rootContext)));

        // Initialize all other prototypes to a basic object for now
        // This prevents null dereferences if getPrototype is called on an uninitialized type
        this->smallIntegerPrototype = this->objectPrototype;
        this->largeIntegerPrototype = this->objectPrototype;
        this->floatPrototype = this->objectPrototype; // Assuming float is not yet implemented as distinct from double
        this->bytePrototype = this->objectPrototype;
        this->nonePrototype = this->objectPrototype; // PROTO_NONE is a special constant, but its prototype can be objectPrototype
        this->methodPrototype = this->objectPrototype;
        this->bufferPrototype = this->objectPrototype;
        this->pointerPrototype = this->objectPrototype;
        this->doublePrototype = this->objectPrototype;
        this->datePrototype = this->objectPrototype;
        this->timestampPrototype = this->objectPrototype;
        this->timedeltaPrototype = this->objectPrototype;
        this->threadPrototype = this->objectPrototype;
        this->rootObject = this->objectPrototype; // Root object can be the base object prototype

        this->listIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false)->addParent(this->rootContext, this->objectPrototype));
        this->tupleIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false)->addParent(this->rootContext, this->objectPrototype));
        this->stringIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false)->addParent(this->rootContext, this->objectPrototype));
        this->sparseListIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false)->addParent(this->rootContext, this->objectPrototype));
        this->setIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false)->addParent(this->rootContext, this->objectPrototype));
        this->multisetIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false)->addParent(this->rootContext, this->objectPrototype));

        // Initialize all mutableRoot shards to an empty SparseList.
        // Each shard holds objects with mutable_ref % MUTABLE_ROOT_SHARDS == shard_index.
        // Also zero the per-cycle GC snapshot of the shard table; the snapshot
        // is populated under STW at every Phase 2 and cleared at the end of
        // every cycle, so steady-state it is always nullptr outside the cycle.
        // The constructor-time zero just guarantees the well-defined initial
        // state on the first cycle.
        for (int s = 0; s < MUTABLE_ROOT_SHARDS; ++s) {
            auto* emptyRaw = new(this->rootContext) ProtoSparseListImplementation(this->rootContext, 0, PROTO_NONE, nullptr, nullptr, true);
            this->mutableRoot[s].root.store(const_cast<ProtoSparseList*>(emptyRaw->asSparseList(this->rootContext)));
            this->gcMutableSnapshot[s] = nullptr;
        }
        
        symbolTable = new SymbolTable();
        initStringInternMap(this);
        this->literalData         = const_cast<ProtoString*>(ProtoString::createSymbol(this->rootContext, "__data__"));
        this->literalSetAttribute = const_cast<ProtoString*>(ProtoString::createSymbol(this->rootContext, "setAttribute"));
        this->literalCallMethod   = const_cast<ProtoString*>(ProtoString::createSymbol(this->rootContext, "callMethod"));

        this->resolutionChain_ = buildDefaultResolutionChain(this->rootContext);

        // Adopt the OS process's main thread so the per-thread caches
        // (attribute, mutable-value, free-cells) are reachable through
        // `context->thread` on every getAttribute / setAttribute call
        // from the main thread.  Without this the main thread silently
        // ran with cache=nullptr, which translated to 0 hits and made
        // every attribute read fall through to the prototype-chain
        // walk + AVL implGetAt traversal — observed as 113.6× geomean
        // on pyperformance and 14.5% getAttribute / 11.2% implGetAt
        // in perf profiles of richards_lite.
        new (this->rootContext)
            ProtoThreadImplementation(ProtoThreadImplementation::AdoptMainThreadTag{},
                                       this->rootContext, /*name=*/nullptr, this);

        // The collector's own allocation context.  Built before the GC
        // thread starts, which is the only thread that allocates through it.
        this->gcContext = new ProtoContext(ProtoContext::GCOwnedTag{}, this);

        this->gcThread = std::make_unique<std::thread>(gcThreadLoop, this);

        // Heap limit from the environment:
        //   PROTOCORE_HEAP_LIMIT_CELLS=<hard>          hard ceiling only
        //   PROTOCORE_HEAP_LIMIT_CELLS=<soft>,<hard>   soft watermark and ceiling
        // in cells.  Without a limit protoCore starts no collection cycle by
        // itself, so this is how an embedder runs under a low memory limit
        // (in tests, for example) without calling setHeapLimits.
        //
        // A single value sets only the hard ceiling (soft 0): a soft
        // watermark equal to the ceiling is never consulted, because
        // getFreeCells enters the soft path only while the heap is below the
        // ceiling, so soft 0 states the actual behaviour.  With the ceiling
        // alone the heap grows up to <hard> cells and, from then on, a thread
        // that needs cells waits for a cycle to reclaim them.
        //
        // Applied last, once the space is fully built, so the bootstrap
        // above never waits on a limit.  Each part must be decimal digits
        // with a value up to INT_MAX; a hard part of 0 means no limit.  Any
        // other value is ignored silently, like the other PROTOCORE_*
        // variables read here.
        if (const char* envLimit = std::getenv("PROTOCORE_HEAP_LIMIT_CELLS")) {
            auto parseCells = [](const char* begin, const char* end, int& out) {
                if (begin == end) return false;
                long long value = 0;
                for (const char* p = begin; p != end; ++p) {
                    if (*p < '0' || *p > '9') return false;
                    value = value * 10 + (*p - '0');
                    if (value > INT_MAX) return false;
                }
                out = static_cast<int>(value);
                return true;
            };
            const char* end = envLimit;
            const char* comma = nullptr;
            for (; *end; ++end) {
                if (*end == ',' && !comma) comma = end;
            }
            int softCells = 0;
            int hardCells = 0;
            const bool valid = comma
                ? parseCells(envLimit, comma, softCells) && parseCells(comma + 1, end, hardCells)
                : parseCells(envLimit, end, hardCells);
            if (valid && hardCells > 0) {
                this->setHeapLimits(softCells, hardCells);
            }
        }
    }

    ProtoSpace::~ProtoSpace() {
        {
            std::lock_guard<std::recursive_mutex> lock(globalMutex);
            this->state = SPACE_STATE_ENDING;
            this->gcCV.notify_all();
            this->stopTheWorldCV.notify_all();
        }
        if (gcThread && gcThread->joinable()) {
            gcThread->join();
        }
        // Free any embedder root sets that the embedder didn't
        // explicitly destroy.  Doing this after the GC thread has
        // joined means no concurrent forEachRootSet can fire.
        {
            std::lock_guard<std::mutex> lock(rootSetsMutex_);
            for (auto* rs : rootSets_) delete rs;
            rootSets_.clear();
        }
        // The GC thread has joined, so nothing allocates through its context.
        delete this->gcContext;
        this->gcContext = nullptr;
        delete this->rootContext;
        freeStringInternMap(this);
        delete symbolTable;
        symbolTable = nullptr;
        delete tupleInterner;
        tupleInterner = nullptr;

        // Drain DirtySegment lists (live, free pool, and survivor pen)
        // and free the underlying heap nodes.  GC thread is already
        // joined above, so no concurrent access remains.
        for (auto* head : { this->dirtySegments.load(std::memory_order_relaxed),
                            this->dirtySegmentFreePool.load(std::memory_order_relaxed),
                            this->survivorPen.load(std::memory_order_relaxed) }) {
            DirtySegment* seg = head;
            while (seg) {
                DirtySegment* nextSeg = seg->next;
                delete seg;
                seg = nextSeg;
            }
        }
        this->dirtySegments.store(nullptr, std::memory_order_relaxed);
        this->dirtySegmentFreePool.store(nullptr, std::memory_order_relaxed);
        this->survivorPen.store(nullptr, std::memory_order_relaxed);
    }

    const ProtoObject* ProtoSpace::getResolutionChain() const {
        if (!resolutionChain_) {
            ProtoSpace* self = const_cast<ProtoSpace*>(this);
            self->resolutionChain_ = buildDefaultResolutionChain(self->rootContext);
        }
        return resolutionChain_ ? resolutionChain_->asObject(rootContext) : PROTO_NONE;
    }

    void ProtoSpace::setResolutionChain(const ProtoObject* newChain) {
        if (!newChain || newChain == PROTO_NONE) {
            resolutionChain_ = buildDefaultResolutionChain(rootContext);
            return;
        }
        const ProtoList* list = newChain->asList(rootContext);
        if (!list) {
            resolutionChain_ = buildDefaultResolutionChain(rootContext);
            return;
        }
        const unsigned long size = list->getSize(rootContext);
        for (unsigned long i = 0; i < size; ++i) {
            const ProtoObject* el = list->getAt(rootContext, static_cast<int>(i));
            if (!el || !el->isString(rootContext)) {
                resolutionChain_ = buildDefaultResolutionChain(rootContext);
                return;
            }
        }
        resolutionChain_ = list;
    }

    const ProtoObject* ProtoSpace::getImportModule(ProtoContext* context, const char* logicalPath, const char* attrName2create) {
        if (std::getenv("PROTO_RESOLVE_DIAG")) {
            fprintf(stderr, "TRACE: getImportModule(%s)\n", logicalPath);
        }
        return getImportModuleImpl(this, context, logicalPath, attrName2create);
    }

    const ProtoThread* ProtoSpace::newThread(
        ProtoContext* context,
        const ProtoString* name,
        ProtoMethod mainFunction,
        const ProtoList* args,
        const ProtoSparseList* kwargs
    ) {
        auto* c = new ProtoContext(this, nullptr, nullptr, nullptr, args, kwargs);
        auto* newThreadImpl = new(c) ProtoThreadImplementation(c, name, this, mainFunction, args, kwargs);
        // runningThreads is incremented in ProtoThreadImplementation constructor
        return newThreadImpl->asThread(c);
    }

    // Wait for the GC to complete a collection cycle, GC-safely.  The caller
    // holds `lock` (globalMutex).  The waiting thread leaves the stop-the-world
    // running set — so the GC quorum (parkedThreads >= runningThreads) is
    // computed without it and it cannot stall a collection — sleeps until a
    // cycle finishes (or a 50 ms watchdog fires, so a missed notify costs
    // latency, not a hang), rejoins the running set, and parks at a safepoint
    // if a stop-the-world began while it slept.  See
    // docs/archive/design-specs/2026-05-22-allocation-limit-oom-design.md.
    static void reclaimWaitLocked(ProtoSpace* space,
                                  std::unique_lock<std::recursive_mutex>& lock,
                                  ProtoContext* ctx) {
        const uint64_t startCycle =
            space->gcCycleCount.load(std::memory_order_relaxed);
        // Make sure a collection will actually run.
        if (!space->gcStarted) space->gcStarted = true;
        space->gcCV.notify_all();
        // Leave the running set.  A GC already parked on the Phase-1 quorum
        // must re-evaluate it against the lowered bound, hence the notify.
        space->runningThreads.fetch_sub(1, std::memory_order_acq_rel);
        space->gcCV.notify_all();
        space->memoryReclaimedCV.wait_for(
            lock, std::chrono::milliseconds(50),
            [space, startCycle] {
                return space->gcCycleCount.load(std::memory_order_relaxed)
                           != startCycle
                    || space->state == SPACE_STATE_ENDING;
            });
        space->runningThreads.fetch_add(1, std::memory_order_acq_rel);
        // Rejoin the stop-the-world protocol: park if a stop-the-world began
        // while this thread was out of the running set.  Park ONLY, never
        // ProtoContext::safepoint().  This wait runs from the heap checkpoint
        // of an outermost critical section (or the depth-0 refill path), inside
        // native code that may hold a half-built structure only in C++ locals
        // and in the context's young chain — for example a list a primitive is
        // about to turn into a tuple.  safepoint() hands that chain to the
        // collector once the context crosses maxAllocatedCellsPerContext, which
        // makes those cells candidates while nothing references them; the next
        // cycle frees them.  Young generations are submitted only when a
        // context is destroyed or at a safepoint() the embedder calls.
        //
        // Parking re-locks globalMutex and waits on the stop-the-world cv.
        // Doing that while we still hold globalMutex would leave the lock owned
        // across the park (recursive_mutex drops only one level) and wedge the
        // GC, so globalMutex is released around it.
        lock.unlock();
        if (ctx && ctx->thread) {
            // The thread's park-only entry.
            ctx->thread->synchToGC();
            // A whole cycle may have run while this thread was out of the
            // running set, with no stop-the-world left to park for when it
            // returns, so synchToGC's own clear may not have run: drop this
            // thread's cache entries if a stop-the-world completed (the caches
            // are not GC roots).  A context without a thread has no caches.
            if (auto* ext = toImpl<ProtoThreadImplementation>(ctx->thread)->extension) {
                ext->clearCachesAfterStopTheWorld(space);
            }
        } else if (ctx && space->stwFlag.load() &&
                   !(space->gcThread && std::this_thread::get_id() == space->gcThread->get_id())
                   && ctx->criticalSectionDepth == 0) {
            // A context without a thread parks the same way, keyed on the
            // context's own critical-section depth.
            space->parkedThreads++;
            {
                std::unique_lock<std::recursive_mutex> parkLock(ProtoSpace::globalMutex);
                space->gcCV.notify_all();
                space->stopTheWorldCV.wait(parkLock, [space] { return !space->stwFlag.load(); });
            }
            space->parkedThreads--;
        }
        lock.lock();
    }

    void ProtoSpace::waitForHeapHeadroom(ProtoContext* ctx) {
        // No limit configured, the GC thread itself, or a contextless caller:
        // nothing to enforce — these cannot, or need not, wait for the GC.
        if (this->maxHeapSize <= 0 || !ctx) return;
        if (this->gcThread &&
            std::this_thread::get_id() == this->gcThread->get_id()) return;

        std::unique_lock<std::recursive_mutex> lock(globalMutex);
        int  oomStrikes      = 0;
        bool oomCallbackUsed = false;
        uint64_t lastSeenCycle =
            this->gcCycleCount.load(std::memory_order_relaxed);
        for (;;) {
            if (this->state == SPACE_STATE_ENDING) return;
            // Room left to grow the heap, or recycled cells already waiting in
            // the freelist — the next allocation can be served without
            // crossing the ceiling.
            if (this->heapSize < this->maxHeapSize) return;
            if (this->freeChunks || this->freeCells) return;
            // At the ceiling with an empty freelist: let the GC reclaim, then
            // re-check.  reclaimWaitLocked leaves the running set for the wait
            // so this thread never stalls the stop-the-world quorum.
            reclaimWaitLocked(this, lock, ctx);
            if (this->freeChunks || this->freeCells) return;

            // The freelist is still empty.  Distinguish "no cycle has
            // completed yet" (reclaimWaitLocked returned on its 50 ms
            // timeout) from "a cycle completed and reclaimed nothing".  Only
            // the latter is evidence of out of memory.
            uint64_t cycle = this->gcCycleCount.load(std::memory_order_relaxed);
            if (cycle == lastSeenCycle) continue;  // collection still pending
            lastSeenCycle = cycle;

            // A full cycle completed without refilling the freelist.  OOM is
            // genuine when the cycle reclaimed nothing: no future cycle can
            // do better while the live set is unchanged.  Reclamation — not
            // mark-phase reachability — is the metric, because a live
            // context's un-submitted young generation fills the heap without
            // ever entering markedList.  Two consecutive zero-reclaim cycles
            // confirm it, absorbing a single cycle's timing races.
            unsigned long reclaimed =
                this->reclaimedLastCycle.load(std::memory_order_relaxed);
            if (reclaimed == 0) {
                if (++oomStrikes >= 2) {
                    if (!oomCallbackUsed && this->outOfMemoryCallback) {
                        // Give the embedder one chance to free its caches.
                        oomCallbackUsed = true;
                        oomStrikes = 0;
                        lock.unlock();
                        this->outOfMemoryCallback(ctx);
                        lock.lock();
                    } else {
                        // Confirmed, unrecoverable out of memory.
                        unsigned long live = this->liveCellsLastCycle.load(
                            std::memory_order_relaxed);
                        lock.unlock();
                        std::fprintf(stderr,
                            "protoCore: heap hard limit %d cells reached; "
                            "live set %lu cells, last cycle reclaimed 0 — "
                            "out of memory\n", this->maxHeapSize, live);
                        std::fflush(stderr);
                        std::abort();
                    }
                }
            } else {
                oomStrikes = 0;  // the GC freed cells — reclamation works
            }
        }
    }

    Cell* ProtoSpace::getFreeCells(ProtoContext* ctx) {
        std::unique_lock<std::recursive_mutex> lock(globalMutex);
        GC_LOCK_TRACE("getFreeCells ACQ");

        const bool isGcThread = this->gcThread &&
            std::this_thread::get_id() == this->gcThread->get_id();
        // The GC thread cannot wait on its own collection, and a contextless
        // caller is not a managed mutator — both bypass the ceiling.  Ordinary
        // mutator allocations honour it, including those inside a critical
        // section: the blocking enforcement runs at critical-section
        // boundaries (ProtoContext::heapLimitCheckpoint), and the OS-request
        // clamp below keeps heapSize from crossing maxHeapSize regardless.
        // With maxHeapSize == 0 (the default) the limit is disabled and this
        // whole path is the historical unbounded allocator, bit-for-bit.
        const bool limitExempt = this->maxHeapSize <= 0 || isGcThread || !ctx;

        // One-shot soft-zone wait: the soft watermark biases toward
        // reclamation once per getFreeCells call, never blocking past it.
        bool softWaited = false;

        for (;;) {
            // Effective batch size — larger when multiple threads run so each
            // refill amortises the lock acquisition over more cells.
            int batchSize = this->blocksPerAllocation;
            if (this->runningThreads > 1) {
                int scaled = this->blocksPerAllocation
                    * static_cast<int>(this->runningThreads.load()) * 4;
                if (scaled < 60000) scaled = 60000;
                if (scaled > 65536) scaled = 65536;
                batchSize = scaled;
            }
            // The OS request below keeps this unlimited size.
            const int unlimitedBatchSize = batchSize;

            // Under a hard heap limit, a refill is capped so that all running
            // threads' batches together use at most 1/kLimitBatchFraction of
            // the limit.  A batch counts against the limit as soon as it is
            // handed out, but no cycle can reclaim the cells a thread holds,
            // so unbounded batches (up to 65,536 cells each with several
            // threads) can exhaust a small limit while the live set is far
            // below it.  Without a limit nothing changes.
            const bool limitedBatches = this->maxHeapSize > 0;
            long limitCap = 0;
            if (limitedBatches) {
                const long threads = std::max(1, this->runningThreads.load());
                limitCap = static_cast<long>(this->maxHeapSize) / (kLimitBatchFraction * threads);
                if (limitCap < kMinLimitedBatchCells) limitCap = kMinLimitedBatchCells;
                if (batchSize > limitCap) batchSize = static_cast<int>(limitCap);
            }

            // Path #5 v2: chunked freelist — O(1) chunk pop.
            if (this->freeChunks) {
                FreeChunk* chunk = this->freeChunks;
                // Sweep publishes chunks of any size (a segment full of dead
                // cells can exceed CELL_CHUNK_SIZE), and without a limit a
                // chunk is handed out whole.  Under a limit, split only a
                // chunk larger than the cap: when the cap does not bind (a
                // generous limit) the chunk is still handed out whole.
                if (limitedBatches && chunk->count > static_cast<unsigned long>(limitCap)) {
                    // Hand out only batchSize cells of this chunk: cut its
                    // chain after batchSize cells and leave the remainder
                    // (same tail) on the free-chunk list.
                    Cell* batchHead = chunk->head;
                    Cell* last = batchHead;
                    for (int i = 1; i < batchSize; ++i) last = last->getNext();
                    chunk->head = last->getNext();
                    chunk->count -= static_cast<unsigned long>(batchSize);
                    last->internalSetNextRaw(nullptr);
                    this->freeCellsCount -= batchSize;
                    GC_LOCK_TRACE("getFreeCells REL(chunk-split)");
                    return batchHead;
                }
                this->freeChunks = chunk->next;
                Cell* batchHead = chunk->head;
                this->freeCellsCount -= static_cast<int>(chunk->count);
                recycleFreeChunk(this, chunk);
                GC_LOCK_TRACE("getFreeCells REL(chunk)");
                return batchHead;
            }

            // Legacy flat freelist fallback — populated by the per-context
            // destructor return path and by OS-allocation overflow.
            if (this->freeCells) {
                if (this->freeCellsCount <= batchSize) {
                    Cell* batchHead = this->freeCells;
                    this->freeCells = nullptr;
                    this->freeCellsTail = nullptr;
                    this->freeCellsCount = 0;
                    GC_LOCK_TRACE("getFreeCells REL(flat-all)");
                    return batchHead;
                }
                Cell* batchHead = this->freeCells;
                Cell* current = batchHead;
                int count = 1;
                while (count < batchSize) {
                    Cell* next = current->getNext();
                    if (!next) break;
                    current = next;
                    count++;
                }
                this->freeCells = current->getNext();
                current->setNext(nullptr);
                this->freeCellsCount -= count;
                if (!this->freeCells) this->freeCellsTail = nullptr;
                GC_LOCK_TRACE("getFreeCells REL(flat-partial)");
                return batchHead;
            }

            // No free cells. Policy (2026-05-23 night): the clean
            // reaction is to refill from the OS — that is what
            // posix_memalign exists for. We do NOT trigger a GC
            // cycle here. Triggering on every freelist exhaustion,
            // independent of how full the heap is, was the historical
            // default but produced a perverse interaction with the
            // concurrent collector: the GC thread woke up,
            // immediately entered Phase-1 stop-the-world, and sat
            // there for the rest of the run because the mutator
            // workers don't park during a drain (no STW safepoint in
            // the bytecode dispatch). The Phase-1 barrier never
            // closed; mark + sweep ran exactly once at the very end
            // of the run. See
            // https://github.com/gamarino/protoST/blob/main/docs/archive/design-specs/2026-05-23-saturation-experiment.md
            // for the measurement (Phase-1 waited 2.27 s of a 2.5 s
            // run at workers=8).
            //
            // The GC is now driven exclusively by:
            //   * the soft/hard cap paths (waitForHeapHeadroom /
            //     reclaimWaitLocked) — those only kick in when a
            //     maxHeapSize is configured AND heapSize crosses
            //     softHeapLimit or maxHeapSize;
            //   * triggerGC(), the public freeRatio-driven entry
            //     point that callers can invoke explicitly;
            //   * ~ProtoSpace, which notifies on shutdown.
            // With the default maxHeapSize == 0 (no cap), getFreeCells
            // never triggers — the runtime grows by OS allocations
            // and the user gets the throughput they paid for. Set a
            // cap if you want the GC to kick in.

            // Size the OS allocation (unchanged sizing policy).  With a heap
            // limit the thread still receives only the capped batchSize; the
            // rest of the request is published as free chunks below.
            int blocksToAllocate;
            if (this->runningThreads > 1) {
                blocksToAllocate = unlimitedBatchSize;
            } else {
                blocksToAllocate = this->blocksPerAllocation * 50;
            }
            if (blocksToAllocate > kMaxBlocksPerOSAllocation)
                blocksToAllocate = kMaxBlocksPerOSAllocation;

            // --- Allocation-limit enforcement -------------------------------
            // Skipped entirely for the exempt callers and when no limit is
            // set (maxHeapSize == 0) — then the loop falls straight through
            // to the OS-allocation path, exactly as before this feature.
            if (!limitExempt) {
                long headroom = static_cast<long>(this->maxHeapSize)
                              - static_cast<long>(this->heapSize);
                if (headroom <= 0) {
                    // HARD zone: the heap is at its ceiling.
                    if (ctx->criticalSectionDepth == 0) {
                        // A depth-0 caller holds no half-built tree, so it may
                        // block for the GC.  waitForHeapHeadroom returns once
                        // the freelist refills (or escalates a genuine OOM);
                        // re-check the freelist afterwards.
                        lock.unlock();
                        this->waitForHeapHeadroom(ctx);
                        lock.lock();
                        continue;
                    }
                    // Inside a critical section the thread cannot block — that
                    // would expose a helper's un-anchored cells to a STW
                    // cycle.  The critical-section entry checkpoint already
                    // enforced the limit; allow a bounded one-batch overshoot
                    // to satisfy this in-flight allocation.
                    blocksToAllocate = batchSize;
                } else {
                    if (this->softHeapLimit > 0
                        && this->heapSize >= this->softHeapLimit
                        && !softWaited
                        && ctx->criticalSectionDepth == 0) {
                        // SOFT zone: prefer reclamation over growth.  Wait one
                        // cycle, then re-check; grow only if that did not help.
                        softWaited = true;
                        reclaimWaitLocked(this, lock, ctx);
                        continue;
                    }
                    // Clamp the OS request so heapSize never crosses
                    // maxHeapSize.
                    if (static_cast<long>(blocksToAllocate) > headroom)
                        blocksToAllocate = static_cast<int>(headroom);
                    // The partitioning below hands the first `batchSize` cells
                    // to the caller; keep batchSize within the (possibly
                    // clamped) allocation so it never indexes past the block.
                    if (batchSize > blocksToAllocate)
                        batchSize = blocksToAllocate;
                }
            }

            // --- OS allocation ----------------------------------------------
            // Do the expensive posix_memalign + chaining outside the lock so
            // other threads can make progress.
            lock.unlock();
            GC_LOCK_TRACE("getFreeCells REL(OS alloc)");

            Cell* newMemory = nullptr;
            int result = posix_memalign(reinterpret_cast<void**>(&newMemory),
                                        64,
                                        blocksToAllocate * sizeof(BigCell));
            if (result != 0) {
                // The OS itself is exhausted — beneath any protoCore ceiling.
                if (this->outOfMemoryCallback)
                    this->outOfMemoryCallback(ctx);
                std::fprintf(stderr,
                    "protoCore: OS allocation of %d cells failed (result %d) "
                    "— out of memory\n", blocksToAllocate, result);
                std::fflush(stderr);
                std::abort();
            }

            BigCell* bigCellPtr = reinterpret_cast<BigCell*>(newMemory);
            // Build the OS allocation as one contiguous chained block.
            for (int i = 0; i < blocksToAllocate - 1; ++i) {
                reinterpret_cast<Cell*>(&bigCellPtr[i])->internalSetNextRaw(
                    reinterpret_cast<Cell*>(&bigCellPtr[i+1]));
            }
            reinterpret_cast<Cell*>(&bigCellPtr[blocksToAllocate - 1])
                ->internalSetNextRaw(nullptr);

            // The first batchSize cells go directly to the calling thread.
            Cell* batchHead = reinterpret_cast<Cell*>(newMemory);
            reinterpret_cast<Cell*>(&bigCellPtr[batchSize - 1])
                ->internalSetNextRaw(nullptr);

            lock.lock();
            GC_LOCK_TRACE("getFreeCells ACQ(OS done)");
            this->heapSize += blocksToAllocate;

            // Partition the remainder into CELL_CHUNK_SIZE chunks so the next
            // getFreeCells lands in the O(1) chunked fast path.
            int remainderStart = batchSize;
            while (remainderStart < blocksToAllocate) {
                int remaining = blocksToAllocate - remainderStart;
                int chunkSize = (remaining > static_cast<int>(CELL_CHUNK_SIZE))
                    ? static_cast<int>(CELL_CHUNK_SIZE) : remaining;
                Cell* chunkHead = reinterpret_cast<Cell*>(&bigCellPtr[remainderStart]);
                Cell* chunkTail = reinterpret_cast<Cell*>(
                    &bigCellPtr[remainderStart + chunkSize - 1]);
                chunkTail->internalSetNextRaw(nullptr);
                publishFreeChunk(this, chunkHead, chunkTail,
                                 static_cast<unsigned long>(chunkSize));
                remainderStart += chunkSize;
            }

            GC_LOCK_TRACE("getFreeCells REL(return OS)");
            return batchHead;
        }
    }

    void ProtoSpace::setHeapLimits(int softCells, int hardCells) {
        std::lock_guard<std::recursive_mutex> lock(globalMutex);
        if (softCells < 0) softCells = 0;
        if (hardCells < 0) hardCells = 0;
        // A soft watermark above the hard ceiling is meaningless — clamp it.
        if (softCells > 0 && hardCells > 0 && softCells > hardCells)
            softCells = hardCells;
        this->softHeapLimit = softCells;
        this->maxHeapSize   = hardCells;
    }

    void ProtoSpace::submitYoungGeneration(const Cell* cell) {
        if (!cell) return;

        // Try to recycle a DirtySegment from the free pool before falling back
        // to a fresh heap allocation.  The pool is a lock-free LIFO; the GC
        // returns segments here after sweep, so steady-state submit/return is
        // alloc-free.  Single CAS on the pop, single CAS on the dirty-list
        // push — no system malloc on the hot path.
        DirtySegment* segment = this->dirtySegmentFreePool.load(std::memory_order_acquire);
        while (segment) {
            DirtySegment* nextFree = segment->next;
            if (this->dirtySegmentFreePool.compare_exchange_weak(
                    segment, nextFree,
                    std::memory_order_acquire,
                    std::memory_order_acquire)) {
                break;  // claimed `segment`
            }
            // segment reloaded by compare_exchange_weak on failure; retry.
        }
        if (!segment) {
            segment = new DirtySegment();
        }

        segment->cellChain = const_cast<Cell*>(cell);
        // Lock-free push so threads do not contend on globalMutex on every context destroy.
        segment->next = this->dirtySegments.load(std::memory_order_relaxed);
        while (!this->dirtySegments.compare_exchange_weak(
                segment->next, segment,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            // segment->next updated by compare_exchange_weak on failure
        }
    }

    void ProtoSpace::triggerGC() {
        // Assume globalMutex is already held if called from getFreeCells.
        // If called from elsewhere, we might need a lock.
        // Actually, let's just make the check atomic and notify without the lock if possible,
        // or just ensure callers hold the lock.
        
        // Let's use a simpler approach: check thresholds and notify.
        // gcCV.notify_all() doesn't need a lock.
        
        double freeRatio = (this->heapSize > 0) ? (static_cast<double>(this->freeCellsCount) / this->heapSize) : 1.0;
        
        if (freeRatio < 0.2 || this->gcStarted) { 
            this->gcStarted = true;
            this->gcCV.notify_all();
        }
    }
}
