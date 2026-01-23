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

namespace proto {

    namespace {
        void gcThreadLoop(ProtoSpace* space) {
            std::unique_lock<std::mutex> lock(ProtoSpace::globalMutex);
            while (space->state != SPACE_STATE_ENDING) {
                // Wait for a GC trigger or space ending
                space->gcCV.wait(lock, [space] { 
                    return space->gcStarted || space->state == SPACE_STATE_ENDING; 
                });
                
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
                
                if (space->state == SPACE_STATE_ENDING) break;

                // --- PHASE 2: COLLECT ROOTS ---
                // Before collecting roots, we must "submit" all young generations
                // so they are considered part of the heap for the mark phase.
                auto submitContextYoungGens = [&](ProtoContext* currentCtx) {
                    while (currentCtx) {
                        if (currentCtx->lastAllocatedCell) {
                            DirtySegment* seg = new DirtySegment();
                            seg->cellChain = currentCtx->lastAllocatedCell;
                            seg->next = space->dirtySegments;
                            space->dirtySegments = seg;
                            
                            currentCtx->lastAllocatedCell = nullptr;
                            currentCtx->allocatedCellsCount = 0;
                        }
                        currentCtx = currentCtx->previous;
                    }
                };

                // 1. Submit for all threads
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
                            const ProtoThread* thread = node->value->asThread(space->rootContext);
                            submitContextYoungGens(toImpl<const ProtoThreadImplementation>(thread)->context);
                        }
                        if (node->previous) stack.push_back(node->previous);
                        if (node->next) stack.push_back(node->next);
                    }
                }
                // 2. Submit for main thread
                submitContextYoungGens(space->mainContext);

                std::vector<const Cell*> workList;
                auto addRootObj = [&](const ProtoObject* obj) {
                    if (obj && obj->isCell(space->rootContext)) {
                        workList.push_back(obj->asCell(space->rootContext));
                    }
                };

                // 3. Scan Thread Stacks
                if (space->threads) {
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
                            currentCtx = currentCtx->previous;
                        }
                    };

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
                
                // 4. Global Roots
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

                if (space->mutableRoot.load()) addRootObj(reinterpret_cast<const ProtoObject*>(space->mutableRoot.load()));
                if (space->threads) addRootObj(reinterpret_cast<const ProtoObject*>(space->threads));
                
                // 5. Scan the Main Thread stack
                ProtoContext* rootCtx = space->mainContext;
                while (rootCtx) {
                    for (unsigned int i = 0; i < rootCtx->getAutomaticLocalsCount(); ++i) {
                        addRootObj(rootCtx->getAutomaticLocals()[i]);
                    }
                    if (rootCtx->closureLocals) addRootObj(reinterpret_cast<const ProtoObject*>(rootCtx->closureLocals));
                    rootCtx = rootCtx->previous;
                }
                
                // --- PHASE 3: RESUME THE WORLD ---
                space->stwFlag.store(false);
                space->stopTheWorldCV.notify_all();
                lock.unlock(); // Allow threads to run while we mark and sweep
                
                // --- PHASE 4: MARK ---
                std::unordered_set<const Cell*> visited;
                while (!workList.empty()) {
                    const Cell* cell = workList.back();
                    workList.pop_back();
                    
                    if (visited.find(cell) == visited.end()) {
                        visited.insert(cell);
                        cell->processReferences(space->rootContext, &workList, [](ProtoContext* ctx, void* self, const Cell* ref) {
                            static_cast<std::vector<const Cell*>*>(self)->push_back(ref);
                        });
                    }
                }
                
                // --- PHASE 5: SWEEP ---
                // We need to lock dirtySegments access
                space->gcStarted = false; // Reset here
                
                DirtySegment* segmentsToProcess = nullptr;
                {
                    std::lock_guard<std::mutex> sweepLock(ProtoSpace::globalMutex);
                    segmentsToProcess = space->dirtySegments;
                    space->dirtySegments = nullptr;
                }

                DirtySegment* currentSeg = segmentsToProcess;
                while (currentSeg) {
                    Cell* cell = currentSeg->cellChain;
                    while (cell) {
                        Cell* nextCell = cell->next;
                        if (visited.find(cell) == visited.end()) {
                            cell->finalize(space->rootContext);
                            
                            std::lock_guard<std::mutex> freeLock(ProtoSpace::globalMutex);
                            cell->next = space->freeCells;
                            space->freeCells = cell;
                            space->freeCellsCount++;
                        }
                        cell = nextCell;
                    }
                    
                    DirtySegment* nextSeg = currentSeg->next;
                    delete currentSeg;
                    currentSeg = nextSeg;
                }

                lock.lock(); // Re-acquire for next wait
            }
        }
    }
    
    std::mutex ProtoSpace::globalMutex;

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
        attributeNotFoundGetCallback(nullptr),
        nonMethodCallback(nullptr),
        parameterTwiceAssignedCallback(nullptr),
        parameterNotFoundCallback(nullptr),
        runningThreads(1), // Main thread starts running
        stwFlag(false),
        parkedThreads(0),
        blocksPerAllocation(1024),
        heapSize(0),
        freeCellsCount(0),
        gcSleepMilliseconds(10),
        dirtySegments(nullptr),
        freeCells(nullptr),
        gcStarted(false),
        mainContext(nullptr)
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

        this->gcThread = std::make_unique<std::thread>(gcThreadLoop, this);
    }

    ProtoSpace::~ProtoSpace() {
        {
            std::lock_guard<std::mutex> lock(globalMutex);
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

    const ProtoThread* ProtoSpace::newThread(
        ProtoContext* context,
        const ProtoString* name,
        ProtoMethod mainFunction,
        const ProtoList* args,
        const ProtoSparseList* kwargs
    ) {
        auto* c = context ? context : new ProtoContext(this, nullptr, nullptr, nullptr, args, kwargs);
        auto* newThreadImpl = new(c) ProtoThreadImplementation(c, name, this, mainFunction, args, kwargs);
        this->runningThreads++;
        return newThreadImpl->asThread(c);
    }

    Cell* ProtoSpace::getFreeCells(const ProtoThread* thread) {
        if (this->freeCellsCount < this->heapSize * 0.2 && !this->gcStarted) {
            this->gcStarted = true;
            this->gcCV.notify_all();
        }
        std::lock_guard<std::mutex> lock(globalMutex);
        
        // If we have free cells in the global list, return a batch
        if (this->freeCells) {
            Cell* batchHead = this->freeCells;
            Cell* current = batchHead;
            int count = 1;
            
            // Try to get blocksPerAllocation (or as many as we have)
            while (current->next && count < this->blocksPerAllocation) {
                current = current->next;
                count++;
            }
            
            this->freeCells = current->next;
            current->next = nullptr;
            this->freeCellsCount -= count;
            return batchHead;
        }

        // No free cells, ask the OS
        Cell* newMemory = nullptr;
        // The user specified a configurable amount of blocks to ask the OS
        // Let's use maxAllocatedCellsPerContext or a similar configuration if available
        // but here it seems we should allocate a "chunk"
        int blocksToAllocate = this->blocksPerAllocation * 10; // For example
        
        int result = posix_memalign(reinterpret_cast<void**>(&newMemory), 64, blocksToAllocate * sizeof(BigCell));
        if (result != 0) {
            if (this->outOfMemoryCallback)
                (this->outOfMemoryCallback(nullptr));
            std::cerr << "NO MORE MEMORY!!: " << result << std::endl;
            exit(-1);
        }

        this->heapSize += blocksToAllocate;

        // Chain the newly allocated cells and add them to the global free list
        // but we return the first blocksPerAllocation to the requesting thread
        BigCell* bigCellPtr = reinterpret_cast<BigCell*>(newMemory);
        
        // Batch to return
        for (int i = 0; i < this->blocksPerAllocation - 1; ++i) {
            reinterpret_cast<Cell*>(&bigCellPtr[i])->next = reinterpret_cast<Cell*>(&bigCellPtr[i+1]);
        }
        reinterpret_cast<Cell*>(&bigCellPtr[this->blocksPerAllocation - 1])->next = nullptr;
        
        // Remaining go to global free list
        if (blocksToAllocate > this->blocksPerAllocation) {
            Cell* globalHead = reinterpret_cast<Cell*>(&bigCellPtr[this->blocksPerAllocation]);
            for (int i = this->blocksPerAllocation; i < blocksToAllocate - 1; ++i) {
                reinterpret_cast<Cell*>(&bigCellPtr[i])->next = reinterpret_cast<Cell*>(&bigCellPtr[i+1]);
            }
            reinterpret_cast<Cell*>(&bigCellPtr[blocksToAllocate - 1])->next = this->freeCells;
            this->freeCells = globalHead;
            this->freeCellsCount += (blocksToAllocate - this->blocksPerAllocation);
        }

        this->triggerGC();
        return reinterpret_cast<Cell*>(newMemory);
    }

    void ProtoSpace::submitYoungGeneration(const Cell* cell) {
        if (!cell) return;
        
        std::lock_guard<std::mutex> lock(globalMutex);
        DirtySegment* segment = new DirtySegment(); // Note: This might need careful allocation if GC is running, but let's assume standard new is fine for now or allocate from a pool later.
        segment->cellChain = const_cast<Cell*>(cell);
        segment->next = this->dirtySegments;
        this->dirtySegments = segment;
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
