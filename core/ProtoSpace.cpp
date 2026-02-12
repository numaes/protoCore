/*
 * ProtoSpace.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <iostream>
#include <cstdlib>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>
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
            uint64_t n = s_getFreeCellsCalls.load();
            if (n > 0) {
                std::lock_guard<std::mutex> lock(s_diagTidsMutex);
                std::cerr << "[proto-alloc-diag] getFreeCells total calls=" << n
                          << " distinct_os_threads=" << s_diagTids.size() << std::endl;
            }
        }
        const ProtoList* buildDefaultResolutionChain(ProtoContext* ctx) {
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

        void gcThreadLoop(ProtoSpace* space) {
            std::unique_lock<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            GC_LOCK_TRACE("gcLoop ACQ(init)");
            while (space->state != SPACE_STATE_ENDING) {
                // Wait for a GC trigger or space ending
                space->gcCV.wait(lock, [space] { 
                    return space->gcStarted || space->state == SPACE_STATE_ENDING; 
                });
                GC_LOCK_TRACE("gcLoop ACQ(gcStarted)");
                if (space->state == SPACE_STATE_ENDING) break;
                
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

                // --- PHASE 2: COLLECT ROOTS ---
                std::vector<const Cell*> workList;
                auto addRootObj = [&](const ProtoObject* obj) {
                    if (obj && obj->isCell(space->rootContext)) {
                        workList.push_back(obj->asCell(space->rootContext));
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
                        // Roots: Young generation (pinned objects allocated in this context)
                        // These are safe from collection because they are not in captured segments.
                        // We scan their references to find pointers to older objects, but we don't
                        // mark the young objects themselves yet. This allows them to be collected
                        // in the very first GC cycle after they are promoted to DirtySegments.
                        Cell* youngCell = currentCtx->lastAllocatedCell;
                        while (youngCell) {
                            youngCell->processReferences(space->rootContext, &workList, [](ProtoContext* ctx, void* self, const Cell* ref) {
                                static_cast<std::vector<const Cell*>*>(self)->push_back(ref);
                            });
                            youngCell = youngCell->getNext();
                        }
                        currentCtx = currentCtx->previous;
                    }
                };

                // 1. Scan Thread Stacks
                if (space->threads) {
                    std::vector<const ProtoSparseListImplementation*> stack;
                    const ProtoObject* rootObj = reinterpret_cast<const ProtoObject*>(space->threads);
                    if (rootObj && rootObj->isCell(space->rootContext)) {
                        stack.push_back(toImpl<const ProtoSparseListImplementation>(rootObj));
                    }
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
                if (space->literalData) addRootObj(space->literalData->asObject(space->rootContext));

                {
                    std::lock_guard<std::mutex> modLock(space->moduleRootsMutex);
                    for (const ProtoObject* mod : space->moduleRoots) {
                        addRootObj(mod);
                    }
                }

                if (space->mutableRoot.load()) addRootObj(reinterpret_cast<const ProtoObject*>(space->mutableRoot.load()));
                if (space->threads) addRootObj(reinterpret_cast<const ProtoObject*>(space->threads));
                
                // 3. Scan the Main Thread stack
                scanContexts(space->mainContext);

                // 6. Capture the heap snapshot (segments to process)
                // This MUST be done during STW to ensure we only sweep what existed at root collection.
                DirtySegment* segmentsToProcess = space->dirtySegments.exchange(nullptr, std::memory_order_acquire);
                
                // --- PHASE 3: RESUME THE WORLD ---
                space->stwFlag.store(false);
                space->stopTheWorldCV.notify_all();
                GC_LOCK_TRACE("gcLoop REL(mark-sweep)");
                lock.unlock(); // Allow threads to run while we mark and sweep
                
                // --- PHASE 4: MARK ---
                while (!workList.empty()) {
                    const Cell* cell = workList.back();
                    workList.pop_back();

                    if (!cell->isMarked()) {
                        const_cast<Cell*>(cell)->mark();
                        cell->processReferences(space->rootContext, &workList, [](ProtoContext* ctx, void* self, const Cell* ref) {
                            static_cast<std::vector<const Cell*>*>(self)->push_back(ref);
                        });
                    }
                }
                
                // --- PHASE 5: SWEEP ---
                DirtySegment* currentSeg = segmentsToProcess;
                while (currentSeg) {
                    Cell* cell = currentSeg->cellChain;
                    
                    Cell* batchHead = nullptr;
                    Cell* batchTail = nullptr;
                    int batchCount = 0;

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
                        }
                        cell = nextCell;
                    }

                    if (batchHead) {
                        GC_LOCK_TRACE("gcLoop ACQ(freeList)");
                        std::lock_guard<std::recursive_mutex> freeLock(ProtoSpace::globalMutex);
                        batchTail->internalSetNextRaw(space->freeCells);
                        space->freeCells = batchHead;
                        space->freeCellsCount += batchCount;
                        GC_LOCK_TRACE("gcLoop REL(freeList)");
                    }

                    DirtySegment* nextSeg = currentSeg->next;
                    delete currentSeg;
                    currentSeg = nextSeg;
                }

                GC_LOCK_TRACE("gcLoop ACQ(after-sweep)");
                lock.lock(); // Re-acquire for next wait
                space->gcStarted = false;
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
        freeCellsCount(0),
        gcSleepMilliseconds(10),
        dirtySegments(nullptr),
        freeCells(nullptr),
        gcStarted(false),
        mainContext(nullptr),
        nextMutableRef(1),
        resolutionChain_(nullptr)
    {
        // Initialize prototypes
        // Initialize prototypes
        this->rootContext = new ProtoContext(this, nullptr, nullptr, nullptr, nullptr, nullptr);
        this->booleanPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->unicodeCharPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->listPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->sparseListPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->objectPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
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

        this->listIteratorPrototype = this->objectPrototype;
        this->tupleIteratorPrototype = this->objectPrototype;
        this->stringIteratorPrototype = this->objectPrototype;
        this->sparseListIteratorPrototype = this->objectPrototype;
        this->setIteratorPrototype = this->objectPrototype;
        this->multisetIteratorPrototype = this->objectPrototype;

        // Initialize mutableRoot
        auto* emptyRaw = new(this->rootContext) ProtoSparseListImplementation(this->rootContext, 0, PROTO_NONE, nullptr, nullptr, true);
        this->mutableRoot = const_cast<ProtoSparseList*>(emptyRaw->asSparseList(this->rootContext));
        
        initStringInternMap(this);
        this->literalData = const_cast<ProtoString*>(ProtoString::fromUTF8String(this->rootContext, "__data__"));

        this->resolutionChain_ = buildDefaultResolutionChain(this->rootContext);

        this->gcThread = std::make_unique<std::thread>(gcThreadLoop, this);
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
        delete this->rootContext;
        freeStringInternMap(this);
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
        return getImportModuleImpl(this, context, logicalPath, attrName2create);
    }

    const ProtoThread* ProtoSpace::newThread(
        ProtoContext* context,
        const ProtoString* name,
        ProtoMethod mainFunction,
        const ProtoList* args,
        const ProtoSparseList* kwargs
    ) {
        auto* c = context ? context : new ProtoContext(this, nullptr, nullptr, nullptr, args, kwargs);
        auto* newThreadImpl = new(c) ProtoThreadImplementation(c, name, this, mainFunction, args, kwargs);
        // runningThreads is incremented in ProtoThreadImplementation constructor
        return newThreadImpl->asThread(c);
    }

    Cell* ProtoSpace::getFreeCells(const ProtoThread* thread) {
        if (std::getenv("PROTO_ALLOC_DIAG")) {
            s_getFreeCellsCalls++;
            {
                long long tid = diagCurrentTid();
                std::lock_guard<std::mutex> lock(s_diagTidsMutex);
                s_diagTids.insert(tid);
            }
            static std::once_flag once;
            std::call_once(once, []{ std::atexit(diagPrintAllocCount); });
        }
        std::unique_lock<std::recursive_mutex> lock(globalMutex);
        GC_LOCK_TRACE("getFreeCells ACQ");

        // Larger batch when multiple threads run: fewer lock acquisitions per thread.
        // Use at least 60k so one refill covers typical benchmark chunk (e.g. sum(range(50k))).
        int batchSize = this->blocksPerAllocation;
        if (this->runningThreads > 1) {
            int scaled = this->blocksPerAllocation * static_cast<int>(this->runningThreads.load()) * 4;
            if (scaled < 60000) scaled = 60000;
            if (scaled > 65536) scaled = 65536;
            batchSize = scaled;
        }

        // If we have free cells in the global list, return a batch (do not trigger GC here;
        // otherwise we could wake the GC thread and then leave without parking, causing a deadlock)
        if (this->freeCells) {
            Cell* batchHead = this->freeCells;
            Cell* current = batchHead;
            int count = 1;

            while (current->getNext() && count < batchSize) {
                current = current->getNext();
                count++;
            }

            this->freeCells = current->getNext();
            current->setNext(nullptr);
            this->freeCellsCount -= count;
            GC_LOCK_TRACE("getFreeCells REL(return batch)");
            return batchHead;
        }

        // No free cells: trigger GC and park so the GC thread can run, unless multiple threads
        // are running — then skip GC and allocate from OS to avoid stop-the-world (main thread
        // is typically in join() and cannot park, so parking all workers would deadlock or
        // serialize progress). With runningThreads > 1 we rely on larger OS allocation to
        // keep the global list populated.
        const bool multiThreaded = this->runningThreads > 1;
        if (!multiThreaded && this->gcThread && std::this_thread::get_id() != this->gcThread->get_id()) {
            if (!this->gcStarted) {
                this->gcStarted = true;
                this->gcCV.notify_all();
            }
            this->parkedThreads++;
            this->gcCV.notify_all();
            GC_LOCK_TRACE("getFreeCells REL(park)");
            this->gcCV.wait(lock, [this] { return !this->gcStarted.load(); });
            GC_LOCK_TRACE("getFreeCells ACQ(wake)");
            this->parkedThreads--;
            // Re-check free list after GC may have replenished it
            if (this->freeCells) {
                Cell* batchHead = this->freeCells;
                Cell* current = batchHead;
                int count = 1;
                while (current->getNext() && count < batchSize) {
                    current = current->getNext();
                    count++;
                }
                this->freeCells = current->getNext();
                current->setNext(nullptr);
                this->freeCellsCount -= count;
                GC_LOCK_TRACE("getFreeCells REL(return after GC)");
                return batchHead;
            }
        }
        // Multi-threaded or gcThread null: fall through to OS allocation.

        // No free cells: allocate from OS. Do the expensive work (posix_memalign + chaining)
        // outside the lock so other threads can make progress; only hold the lock to update
        // the global free list. Otherwise 4 threads would serialize on one lock for ~50ms+ each.
        int blocksToAllocate;
        if (this->runningThreads > 1) {
            blocksToAllocate = batchSize;
        } else {
            int osAllocationMultiplier = 50;
            blocksToAllocate = this->blocksPerAllocation * osAllocationMultiplier;
        }
        if (blocksToAllocate > kMaxBlocksPerOSAllocation)
            blocksToAllocate = kMaxBlocksPerOSAllocation;

        lock.unlock();
        GC_LOCK_TRACE("getFreeCells REL(OS alloc)");

        Cell* newMemory = nullptr;
        int result = posix_memalign(reinterpret_cast<void**>(&newMemory), 64, blocksToAllocate * sizeof(BigCell));
        if (result != 0) {
            if (this->outOfMemoryCallback)
                (this->outOfMemoryCallback(nullptr));
            std::cerr << "NO MORE MEMORY!!: " << result << std::endl;
            exit(-1);
        }

        BigCell* bigCellPtr = reinterpret_cast<BigCell*>(newMemory);
        for (int i = 0; i < blocksToAllocate - 1; ++i) {
            reinterpret_cast<Cell*>(&bigCellPtr[i])->internalSetNextRaw(reinterpret_cast<Cell*>(&bigCellPtr[i+1]));
        }
        reinterpret_cast<Cell*>(&bigCellPtr[blocksToAllocate - 1])->internalSetNextRaw(nullptr);
        Cell* batchHead = reinterpret_cast<Cell*>(newMemory);
        Cell* remainderHead = (blocksToAllocate > batchSize) ? reinterpret_cast<Cell*>(&bigCellPtr[batchSize]) : nullptr;
        reinterpret_cast<Cell*>(&bigCellPtr[batchSize - 1])->internalSetNextRaw(nullptr);

        lock.lock();
        GC_LOCK_TRACE("getFreeCells ACQ(OS done)");
        this->heapSize += blocksToAllocate;
        if (remainderHead) {
            reinterpret_cast<Cell*>(&bigCellPtr[blocksToAllocate - 1])->internalSetNextRaw(this->freeCells);
            this->freeCells = remainderHead;
            this->freeCellsCount += (blocksToAllocate - batchSize);
        }
        GC_LOCK_TRACE("getFreeCells REL(return OS)");
        return batchHead;
    }

    void ProtoSpace::submitYoungGeneration(const Cell* cell) {
        if (!cell) return;
        DirtySegment* segment = new DirtySegment();
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
