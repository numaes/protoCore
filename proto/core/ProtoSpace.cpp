/*
 * ProtoSpace.cpp
 *
 *  Created on: Jun 20, 2016
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

#include <malloc.h>
#include <stdio.h>
#include <thread>
#include <chrono>
#include <functional>
#include <condition_variable>

using namespace std;
using namespace std::literals::chrono_literals;

namespace proto
{
#define GC_SLEEP_MILLISECONDS           1000
#define BLOCKS_PER_ALLOCATION           1024
#define BLOCKS_PER_MALLOC_REQUEST       8 * BLOCKS_PER_ALLOCATION
#define MAX_ALLOCATED_CELLS_PER_CONTEXT 1024

#define KB                              1024
#define MB                              1024 * KB
#define GB                              1024 * MB
#define MAX_HEAP_SIZE                   512 * MB

    std::mutex ProtoSpace::globalMutex;

    void gcCollectCells(ProtoContext* context, void* self, Cell* value)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)value;

        ProtoSparseListImplementation* cellSet = new(context) ProtoSparseListImplementation(context);

        // Go further in the scanning only if it is a cell and the cell belongs to current context!
        if (p.op.pointer_tag != POINTER_TAG_EMBEDEDVALUE)
        {
            // It is an object pointer with references
            if (!cellSet->implHas(context, p.asHash.hash))
            {
                cellSet = cellSet->implSetAt(context, p.asHash.hash, PROTO_NONE);
                p.cell.cell->processReferences(context, (void*)cellSet, gcCollectCells);
            }
        }
    }

    void gcCollectObjects(ProtoContext* context, void* self, ProtoObject* value)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)value;

        ProtoSparseListImplementation* cellSet = new(context) ProtoSparseListImplementation(context);

        // Go further in the scanning only if it is a cell and the cell belongs to current context!
        if (p.op.pointer_tag != POINTER_TAG_EMBEDEDVALUE)
        {
            // It is an object pointer with references
            if (!cellSet->implHas(context, p.asHash.hash))
            {
                cellSet = cellSet->implSetAt(context, p.asHash.hash, PROTO_NONE);
                p.cell.cell->processReferences(context, (void*)cellSet, gcCollectCells);
            }
        }
    }

    void gcScan(ProtoContext* context, ProtoSpace* space)
    {
        DirtySegment* toAnalize;

        // Acquire space lock and take all dirty segments to analyze

        bool oldValue = false;
        while (space->gcLock.compare_exchange_strong(
            oldValue,
            true
        ))
            std::this_thread::yield();

        toAnalize = space->dirtySegments;
        space->dirtySegments = nullptr;

        space->gcLock.store(false);

        ProtoContext gcContext(context);

        // Stop the world
        // Wait till all managed threads join the stopped state
        // After stopping the world, no managed thread is changing its state

        space->state = SPACE_STATE_STOPPING_WORLD;
        while (space->state == SPACE_STATE_STOPPING_WORLD)
        {
            std::unique_lock<std::mutex> lk(ProtoSpace::globalMutex);
            space->stopTheWorldCV.wait(lk);

            int allStoping = true;
            unsigned long threadsCount = space->threads->getSize(context);
            for (unsigned n = 0; n < threadsCount; n++)
            {
                ProtoThreadImplementation* t = (ProtoThreadImplementation*)space->threads->getAt(&gcContext, n);

                // Be sure no thread is still in managed state
                if (t->state == THREAD_STATE_MANAGED)
                {
                    allStoping = false;
                    break;
                }
            }
            if (allStoping)
                space->state = SPACE_STATE_WORLD_TO_STOP;
        }

        space->restartTheWorldCV.notify_all();

        while (space->state == SPACE_STATE_WORLD_TO_STOP)
        {
            std::unique_lock<std::mutex> lk(ProtoSpace::globalMutex);
            space->stopTheWorldCV.wait(lk);

            int allStopped = true;
            unsigned long threadsCount = space->threads->getSize(context);
            for (unsigned n = 0; n < threadsCount; n++)
            {
                ProtoThreadImplementation* t = (ProtoThreadImplementation*)space->threads->getAt(&gcContext, n);

                if (t->state != THREAD_STATE_STOPPED)
                {
                    allStopped = false;
                    break;
                }
            }
            if (allStopped)
                space->state = SPACE_STATE_WORLD_STOPPED;

            space->restartTheWorldCV.notify_all();
        }

        // cellSet: a set of all referenced Cells

        ProtoSparseListImplementation* cellSet = new(context) ProtoSparseListImplementation(context);

        // Add all mutables to cellSet
        ((ProtoSparseList*)space->mutableRoot.load())->processValues(
            &gcContext,
            &cellSet,
            gcCollectObjects
        );

        // Collect all roots from thread stacks

        unsigned long threadsCount = space->threads->getSize(context);
        while (threadsCount--)
        {
            ProtoThreadImplementation* thread = (ProtoThreadImplementation*)space->threads->getAt(
                &gcContext, threadsCount);

            // Collect allocated objects
            ProtoContext* currentContext = thread->currentContext;
            while (currentContext)
            {
                Cell* currentCell = currentContext->lastAllocatedCell;
                while (currentCell)
                {
                    if (!cellSet->implHas(context, currentCell->getHash(context)))
                    {
                        cellSet = cellSet->implSetAt(
                            context,
                            currentCell->getHash(context),
                            currentCell->asObject(context)
                        );
                    }

                    currentCell = currentCell->nextCell;
                }

                if (currentContext->localsBase)
                {
                    ProtoObjectPointer p;
                    p.oid.oid = *currentContext->localsBase;
                    for (int n = currentContext->localsCount;
                         n > 0;
                         n--)
                    {
                        if (p.op.pointer_tag != POINTER_TAG_EMBEDEDVALUE)
                        {
                            cellSet = cellSet->implSetAt(
                                context,
                                p.asHash.hash,
                                p.cell.cell->asObject(context)
                            );
                        }
                    }
                }

                currentContext = currentContext->previous;
            }
        };

        // Free the world. Let them run
        space->state = SPACE_STATE_RUNNING;
        space->restartTheWorldCV.notify_all();

        // Deep Scan all indirect roots. Deep traversal of cellSet
        cellSet->implProcessValues(context, cellSet, gcCollectObjects);

        // Scan all blocks to analyze and if they are not referenced, free them

        Cell* freeBlocks = nullptr;
        Cell* firstBlock = nullptr;
        int freeCount = 0;
        while (toAnalize)
        {
            Cell* block = toAnalize->cellChain;

            while (block)
            {
                Cell* nextCell = block->nextCell;

                if (!cellSet->implHas(&gcContext, block->getHash(context)))
                {
                    block->~Cell();

                    void** p = (void**)block;
                    for (unsigned i = 0; i < sizeof(BigCell) / sizeof(void*); i++)
                        *p++ = nullptr;

                    if (!firstBlock)
                        firstBlock = block;

                    block->nextCell = freeBlocks;
                    freeBlocks = block;
                    freeCount++;
                }
                block = nextCell;
            }

            DirtySegment* segmentToFree = toAnalize;
            toAnalize = toAnalize->nextSegment;
            delete segmentToFree;
        }

        // Update space freeCells
        oldValue = false;
        while (space->gcLock.compare_exchange_strong(
            oldValue,
            true
        ))
            std::this_thread::yield();

        if (firstBlock)
            firstBlock->nextCell = space->freeCells;

        space->freeCells = freeBlocks;
        space->freeCellsCount += freeCount;

        space->gcLock.store(false);
    };

    void gcThreadLoop(ProtoSpace* space)
    {
        ProtoContext gcContext;

        space->gcStarted = true;
        space->gcCV.notify_one();

        while (space->state != SPACE_STATE_RUNNING)
        {
            std::unique_lock<std::mutex> lk(ProtoSpace::globalMutex);

            space->gcCV.wait_for(lk, std::chrono::milliseconds(space->gcSleepMilliseconds));

            if (space->dirtySegments)
            {
                gcScan(&gcContext, space);
            }
        }
    };

    ProtoSpace::ProtoSpace(
        ProtoMethod mainFunction,
        int argc,
        char** argv
    )
    {
        this->state = SPACE_STATE_RUNNING;

        ProtoContext* creationContext = new ProtoContext(
            nullptr,
            nullptr,
            0,
            nullptr,
            this
        );
        this->threads = creationContext->newSparseList();
        this->tupleRoot = new(creationContext) TupleDictionary(creationContext, nullptr, nullptr, nullptr);

        ProtoList* mainParameters = creationContext->newList();
        mainParameters = (ProtoList*)mainParameters->appendLast(
            creationContext,
            creationContext->fromInteger(argc)
        );
        ProtoList* argvList = creationContext->newList();
        if (argc && argv)
        {
            for (int i = 0; i < argc; i++)
                argvList = (ProtoList*)argvList->appendLast(
                    creationContext,
                    creationContext->fromUTF8String(argv[i])->asObject(creationContext)
                );
        }
        mainParameters = (ProtoList*)mainParameters->appendLast(
            creationContext, argvList->asObject(creationContext)
        );

        this->mainThreadId = std::this_thread::get_id();

        this->mutableLock.store(false);
        this->threadsLock.store(false);
        this->gcLock.store(false);
        this->mutableRoot.store(new(creationContext) ProtoSparseListImplementation(creationContext));

        this->maxAllocatedCellsPerContext = MAX_ALLOCATED_CELLS_PER_CONTEXT;
        this->blocksPerAllocation = BLOCKS_PER_ALLOCATION;
        this->heapSize = 0;
        this->freeCellsCount = 0;
        this->gcSleepMilliseconds = GC_SLEEP_MILLISECONDS;
        this->maxHeapSize = MAX_HEAP_SIZE;
        this->blockOnNoMemory = false;
        this->gcStarted = false;
        this->freeCells = nullptr;

        // Create GC thread and ensure it is working
        this->gcThread = new std::thread(
            (void (*)(ProtoSpace*))(&gcThreadLoop),
            this
        );

        while (!this->gcStarted)
        {
            std::unique_lock<std::mutex> lk(globalMutex);
            this->gcCV.wait_for(lk, 100ms);
        }

        ProtoThread* mainThread = new(creationContext) ProtoThreadImplementation(
            creationContext,
            creationContext->fromUTF8String("Main thread"),
            this,
            mainFunction,
            mainParameters,
            nullptr
        );

        // Wait till the main thread and gcThread end

        mainThread->join(creationContext);
        this->gcThread->join();
    };

    void scanThreads(ProtoContext* context, void* self, ProtoObject* value)
    {
        ProtoList** threadList = (ProtoList**)self;

        *threadList = (ProtoList*)(*threadList)->appendLast(context, value);
    }

    ProtoSpace::~ProtoSpace()
    {
        ProtoContext finalContext(nullptr);

        int threadCount = this->threads->getSize(&finalContext);

        this->state = SPACE_STATE_ENDING;

        // Wait till all threads are ended
        for (int i = 0; i < threadCount; i++)
        {
            ProtoThread* t = (ProtoThread*)this->threads->getAt(
                &finalContext,
                i
            );
            t->join(&finalContext);
        }

        this->triggerGC();

        this->gcThread->join();
    };

    void ProtoSpace::triggerGC()
    {
        this->gcCV.notify_all();
    }

    void ProtoSpace::allocThread(ProtoContext* context, ProtoThread* thread)
    {
        bool oldValue = false;
        while (this->threadsLock.compare_exchange_strong(
            oldValue,
            true
        ))
            std::this_thread::yield();

        if (this->threads)
            this->threads = (ProtoSparseList*)this->threads->setAt(
                context,
                thread->getName(context)->getHash(context),
                thread->asObject(context)
            );

        this->threadsLock.store(false);
    };

    void ProtoSpace::deallocThread(ProtoContext* context, ProtoThread* thread)
    {
        bool oldValue = false;
        while (this->threadsLock.compare_exchange_strong(
            oldValue,
            true
        ))
            std::this_thread::yield();

        int threadCount = this->threads->getSize(context);
        while (threadCount--)
        {
            ProtoThread* t = (ProtoThread*)this->threads->getAt(context, threadCount);
            if (t == thread)
            {
                this->threads = (ProtoSparseList*)this->threads->removeAt(context, threadCount);
                break;
            }
        }

        this->threadsLock.store(false);
    };

    Cell* ProtoSpace::getFreeCells(ProtoThread* currentThread)
    {
        Cell* newBlock = nullptr;
        Cell* previousBlock = nullptr;

        bool oldValue = false;
        while (this->gcLock.compare_exchange_strong(
            oldValue,
            true
        ))
            std::this_thread::yield();

        for (int i = 0; i < BLOCKS_PER_ALLOCATION; i++)
        {
            if (!this->freeCells)
            {
                // Alloc from OS

                int toAllocBytes = sizeof(BigCell) * BLOCKS_PER_MALLOC_REQUEST;
                if (this->maxHeapSize != 0 && !this->blockOnNoMemory &&
                    this->heapSize + toAllocBytes >= this->maxHeapSize)
                {
                    printf(
                        "\nPANIC ERROR: HEAP size will be bigger than configured maximun (%d is over %d bytes)! Exiting ...\n",
                        this->heapSize + toAllocBytes, this->maxHeapSize
                    );
                    std::exit(1);
                }

                if (this->maxHeapSize != 0 && this->blockOnNoMemory &&
                    this->heapSize + toAllocBytes >= this->maxHeapSize)
                {
                    while (!this->freeCells)
                    {
                        this->gcLock.store(false);

                        currentThread->synchToGC();

                        std::this_thread::sleep_for(std::chrono::milliseconds(100));

                        while (this->gcLock.compare_exchange_strong(
                            oldValue,
                            true
                        ))
                            std::this_thread::yield();
                    }
                }
                else
                {
                    printf(
                        "\nmalloc of %d bytes, from current %d already allocated\n",
                        toAllocBytes,
                        this->heapSize
                    );
                    BigCell* newBlocks = static_cast<BigCell*>(malloc(toAllocBytes));
                    if (!newBlocks)
                    {
                        printf("\nPANIC ERROR: Not enough MEMORY! Exiting ...\n");
                        std::exit(1);
                    }

                    BigCell* currentBlock = newBlocks;
                    Cell* lastBlock = this->freeCells;
                    int allocatedBlocks = toAllocBytes / sizeof(BigCell);
                    for (int n = 0; n < allocatedBlocks; n++)
                    {
                        // Clear new allocated block
                        void** p = (void**)currentBlock;
                        for (unsigned long count = 0;
                             count < sizeof(BigCell) / sizeof(void*);
                             count++)
                            *p++ = nullptr;

                        // Chain new blocks as a list
                        currentBlock->nextCell = lastBlock;
                        lastBlock = currentBlock++;
                    }

                    this->freeCells = lastBlock;

                    this->heapSize += toAllocBytes;
                    this->freeCellsCount += allocatedBlocks;
                }
            }

            if (this->freeCells)
            {
                newBlock = this->freeCells;
                this->freeCells = newBlock->nextCell;

                this->freeCellsCount -= 1;
                newBlock->nextCell = previousBlock;
                previousBlock = newBlock;
            }
        }

        this->gcLock.store(false);

        return newBlock;
    };

    void ProtoSpace::analyzeUsedCells(Cell* cellsChain)
    {
        DirtySegment* newChain;

        bool oldValue = false;
        while (this->gcLock.compare_exchange_strong(
            oldValue,
            true
        ))
            std::this_thread::yield();

        newChain = new DirtySegment();
        newChain->cellChain = (BigCell*)cellsChain;
        newChain->nextSegment = this->dirtySegments;
        this->dirtySegments = newChain;

        this->gcLock.store(false);
    };
};
