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

    // This mutex is for condition variables, a necessary exception for STW coordination.
    std::mutex ProtoSpace::globalMutex;

    // Helper function for the GC's mark phase.
    // Recursively finds all reachable objects and adds them to the live set.
    void gcMarkReachable(ProtoContext* context, void* self, Cell* value)
    {
        // self is a pointer to the live set (ProtoSparseListImplementation**)
        auto** liveSetPtr = static_cast<ProtoSparseListImplementation**>(self);
        auto* liveSet = *liveSetPtr;

        ProtoObjectPointer p{};
        p.cell.cell = value;

        // If it's not a cell (e.g., embedded value) or already marked, stop.
        if (!value || !value->isCell(context) || liveSet->implHas(context, p.asHash.hash))
        {
            return;
        }

        // Mark the object as live by adding it to the set.
        *liveSetPtr = liveSet->implSetAt(context, p.asHash.hash, PROTO_NONE);

        // Recursively process all references held by this object.
        value->processReferences(context, self, gcMarkReachable);
    }

    void gcMarkObject(ProtoContext* context, void* self, const ProtoObject* value)
    {
        if (value && value->isCell(context))
        {
            gcMarkReachable(context, self, value->asCell(context));
        }
    }

    void gcScan(ProtoContext* context, ProtoSpace* space)
    {
        DirtySegment* toAnalize{};

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

        // --- Fase 1: Pausa Sincronizada (Stop-The-World) ---
        // El objetivo es pausar todos los hilos de usuario el tiempo mínimo
        // indispensable para recolectar las raíces de manera segura.
        space->state = SPACE_STATE_STOPPING_WORLD;
        {
            std::unique_lock lk(ProtoSpace::globalMutex);
            // Esperar hasta que todos los hilos activos hayan notificado que están en pausa.
            // Cada hilo, al pausarse en implSynchToGC, decrementa el contador runningThreads.
            space->stopTheWorldCV.wait(lk, [&]
            {
                return space->runningThreads.load() == 0;
            });
        }
        space->state = SPACE_STATE_WORLD_STOPPED;

        // --- Fase 2: Recolección de Raíces (AÚN DENTRO DE STW) ---
        // Se crea el conjunto de objetos vivos inicial. Este es el único trabajo
        // que se realiza con el mundo detenido.
        auto* liveSet = new(context) ProtoSparseListImplementation(context);

        // Raíz 1: El `mutableRoot` del espacio.
        toImpl<const ProtoSparseListImplementation>(space->mutableRoot.load())->processValues(
            context,
            &liveSet,
            gcMarkObject
        );

        // Raíz 2: Las pilas y contextos de todos los hilos.
        const ProtoList* threads = space->getThreads(context);
        const unsigned long threadsCount = threads->getSize(context);
        for (unsigned long i = 0; i < threadsCount; ++i)
        {
            const auto* thread = threads->getAt(context, i)->asThread(context);
            const auto* threadImpl = toImpl<const ProtoThreadImplementation>(thread);
            // processReferences recorrerá la pila de contextos y el cache de atributos del hilo.
            threadImpl->processReferences(context, &liveSet, gcMarkReachable);
        }

        // --- Fin de STW: Reanudación Inmediata del Mundo ---
        space->state = SPACE_STATE_RUNNING;
        space->restartTheWorldCV.notify_all();

        // --- Fase 3: Marcado Concurrente ---
        // Con los hilos de usuario ya en ejecución, el GC ahora explora el grafo
        // de objetos completo partiendo de las raíces que recolectó.
        // La inmutabilidad garantiza que este grafo no cambiará mientras lo leemos.
        liveSet->processValues(context, &liveSet, gcMarkObject);

        // --- Fase 4: Barrido Concurrente ---
        // Scan all segments and free any cell not in the live set.
        Cell* freeBlocks = nullptr;
        Cell* lastFreeBlock = nullptr;
        int freeCount = 0;
        while (toAnalize)
        {
            Cell* currentCell = toAnalize->cellChain;
            while (currentCell)
            {
                Cell* nextCell = currentCell->next;

                if (!liveSet->implHas(context, currentCell->getHash(context)))
                {
                    // Finalize and add to the free list.
                    currentCell->finalize(context);

                    if (!freeBlocks)
                    {
                        freeBlocks = currentCell;
                    }
                    if (lastFreeBlock)
                    {
                        lastFreeBlock->next = currentCell;
                    }
                    lastFreeBlock = currentCell;
                    freeCount++;
                }
                currentCell = nextCell;
            }

            DirtySegment* segmentToFree = toAnalize;
            toAnalize = toAnalize->nextSegment;
            delete segmentToFree;
        }

        // Add the newly freed blocks to the global free list.
        oldValue = false;
        while (space->gcLock.compare_exchange_strong(
            oldValue,
            true
        ))
            std::this_thread::yield();

        if (lastFreeBlock)
        {
            lastFreeBlock->next = space->freeCells;
        }

        space->freeCells = freeBlocks;
        space->freeCellsCount += freeCount;

        space->gcLock.store(false);
    };

    void gcThreadLoop(ProtoSpace* space)
    {
      ProtoContext gcContext(space);

      space->gcStarted = true;
      space->gcCV.notify_one();

      // The GC thread must run as long as the space is not shutting down.
      while (space->state != SPACE_STATE_ENDING)
      {
          std::unique_lock<std::mutex> lk(ProtoSpace::globalMutex);

          space->gcCV.wait_for(lk, std::chrono::milliseconds(space->gcSleepMilliseconds));

          if (space->dirtySegments)
          {
              gcScan(&gcContext, space);
          }
      }
    };

    ProtoSpace::ProtoSpace()
    {
        this->state = SPACE_STATE_RUNNING;

        auto* creationContext = new ProtoContext(this); // This context will be managed by the main thread stack
        this->threads = creationContext->newSparseList();

        this->mainThreadId = std::this_thread::get_id();
        this->mutableLock.store(false);
        this->threadsLock.store(false);
        this->gcLock.store(false);
        this->mutableRoot.store(new(creationContext) ProtoSparseListImplementation(creationContext));

        this->runningThreads.store(0);
        this->maxAllocatedCellsPerContext = MAX_ALLOCATED_CELLS_PER_CONTEXT;
        this->blocksPerAllocation = BLOCKS_PER_ALLOCATION;
        this->heapSize = 0;
        this->freeCellsCount = 0;
        this->gcSleepMilliseconds = GC_SLEEP_MILLISECONDS;
        this->maxHeapSize = MAX_HEAP_SIZE;
        this->blockOnNoMemory = false;
        this->gcStarted = false;
        this->freeCells = nullptr;

        // Pre-allocate the emergency buffer for OOM handling.
        constexpr size_t EMERGENCY_BUFFER_SIZE = 100 * 1024; // 100KB
        this->emergency_buffer = new(std::nothrow) char[EMERGENCY_BUFFER_SIZE];
        if (!this->emergency_buffer) {
            // If we can't even allocate the emergency buffer, it's a fatal, unrecoverable error.
            fprintf(stderr, "FATAL: Could not allocate the initial emergency memory buffer.\n");
            std::exit(1);
        }
        this->emergency_ptr = this->emergency_buffer;
        this->emergency_end = this->emergency_buffer + EMERGENCY_BUFFER_SIZE;
        this->emergency_allocator_active.store(false);

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
            ProtoString::fromUTF8String(creationContext, "Main thread"),
            this
        );
        this->allocThread(creationContext, const_cast<ProtoThread*>(mainThread));
        this->rootContext = creationContext;

    };

    ProtoSpace::~ProtoSpace()
    {
        ProtoContext finalContext(nullptr);

        const unsigned long threadCount = this->threads->getSize(&finalContext);

        this->state = SPACE_STATE_ENDING;

        // Wait till all threads are ended
        for (int i = 0; i < threadCount; i++)
        {
            auto t = this->threads->getAt(
                (ProtoContext*)this->rootContext,
                i
            )->asThread(this->rootContext);
            t->join(this->rootContext);
        }

        this->triggerGC();

        this->gcThread->join();

        // Free the emergency buffer
        delete[] this->emergency_buffer;
    };

    void ProtoSpace::triggerGC() const
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
            this->threads = this->threads->setAt(
                context,
                thread->getName(context)->getHash(context),
                thread->asObject(context)
            );

        this->threadsLock.store(false);
        this->runningThreads.fetch_add(1);
    };

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

    // This mutex is for condition variables, a necessary exception for STW coordination.
    std::mutex ProtoSpace::globalMutex;

    // Helper function for the GC's mark phase.
    // Recursively finds all reachable objects and adds them to the live set.
    void gcMarkReachable(ProtoContext* context, void* self, Cell* value)
    {
        // self is a pointer to the live set (ProtoSparseListImplementation**)
        auto** liveSetPtr = static_cast<ProtoSparseListImplementation**>(self);
        auto* liveSet = *liveSetPtr;

        ProtoObjectPointer p{};
        p.cell.cell = value;

        // If it's not a cell (e.g., embedded value) or already marked, stop.
        if (!value || !value->isCell(context) || liveSet->implHas(context, p.asHash.hash))
        {
            return;
        }

        // Mark the object as live by adding it to the set.
        *liveSetPtr = liveSet->implSetAt(context, p.asHash.hash, PROTO_NONE);

        // Recursively process all references held by this object.
        value->processReferences(context, self, gcMarkReachable);
    }

    void gcMarkObject(ProtoContext* context, void* self, const ProtoObject* value)
    {
        if (value && value->isCell(context))
        {
            gcMarkReachable(context, self, value->asCell(context));
        }
    }

    void gcScan(ProtoContext* context, ProtoSpace* space)
    {
        DirtySegment* toAnalize{};

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

        // --- Fase 1: Pausa Sincronizada (Stop-The-World) ---
        // El objetivo es pausar todos los hilos de usuario el tiempo mínimo
        // indispensable para recolectar las raíces de manera segura.
        space->state = SPACE_STATE_STOPPING_WORLD;
        {
            std::unique_lock lk(ProtoSpace::globalMutex);
            // Esperar hasta que todos los hilos activos hayan notificado que están en pausa.
            // Cada hilo, al pausarse en implSynchToGC, decrementa el contador runningThreads.
            space->stopTheWorldCV.wait(lk, [&]
            {
                return space->runningThreads.load() == 0;
            });
        }
        space->state = SPACE_STATE_WORLD_STOPPED;

        // --- Fase 2: Recolección de Raíces (AÚN DENTRO DE STW) ---
        // Se crea el conjunto de objetos vivos inicial. Este es el único trabajo
        // que se realiza con el mundo detenido.
        auto* liveSet = new(context) ProtoSparseListImplementation(context);

        // Raíz 1: El `mutableRoot` del espacio.
        toImpl<const ProtoSparseListImplementation>(space->mutableRoot.load())->processValues(
            context,
            &liveSet,
            gcMarkObject
        );

        // Raíz 2: Las pilas y contextos de todos los hilos.
        const ProtoList* threads = space->getThreads(context);
        const unsigned long threadsCount = threads->getSize(context);
        for (unsigned long i = 0; i < threadsCount; ++i)
        {
            const auto* thread = threads->getAt(context, i)->asThread(context);
            const auto* threadImpl = toImpl<const ProtoThreadImplementation>(thread);
            // processReferences recorrerá la pila de contextos y el cache de atributos del hilo.
            threadImpl->processReferences(context, &liveSet, gcMarkReachable);
        }

        // --- Fin de STW: Reanudación Inmediata del Mundo ---
        space->state = SPACE_STATE_RUNNING;
        space->restartTheWorldCV.notify_all();

        // --- Fase 3: Marcado Concurrente ---
        // Con los hilos de usuario ya en ejecución, el GC ahora explora el grafo
        // de objetos completo partiendo de las raíces que recolectó.
        // La inmutabilidad garantiza que este grafo no cambiará mientras lo leemos.
        liveSet->processValues(context, &liveSet, gcMarkObject);

        // --- Fase 4: Barrido Concurrente ---
        // Scan all segments and free any cell not in the live set.
        Cell* freeBlocks = nullptr;
        Cell* lastFreeBlock = nullptr;
        int freeCount = 0;
        while (toAnalize)
        {
            Cell* currentCell = toAnalize->cellChain;
            while (currentCell)
            {
                Cell* nextCell = currentCell->next;

                if (!liveSet->implHas(context, currentCell->getHash(context)))
                {
                    // Finalize and add to the free list.
                    currentCell->finalize(context);

                    if (!freeBlocks)
                    {
                        freeBlocks = currentCell;
                    }
                    if (lastFreeBlock)
                    {
                        lastFreeBlock->next = currentCell;
                    }
                    lastFreeBlock = currentCell;
                    freeCount++;
                }
                currentCell = nextCell;
            }

            DirtySegment* segmentToFree = toAnalize;
            toAnalize = toAnalize->nextSegment;
            delete segmentToFree;
        }

        // Add the newly freed blocks to the global free list.
        oldValue = false;
        while (space->gcLock.compare_exchange_strong(
            oldValue,
            true
        ))
            std::this_thread::yield();

        if (lastFreeBlock)
        {
            lastFreeBlock->next = space->freeCells;
        }

        space->freeCells = freeBlocks;
        space->freeCellsCount += freeCount;

        space->gcLock.store(false);
    };

    void gcThreadLoop(ProtoSpace* space)
    {
      ProtoContext gcContext(space);

      space->gcStarted = true;
      space->gcCV.notify_one();

      // The GC thread must run as long as the space is not shutting down.
      while (space->state != SPACE_STATE_ENDING)
      {
          std::unique_lock<std::mutex> lk(ProtoSpace::globalMutex);

          space->gcCV.wait_for(lk, std::chrono::milliseconds(space->gcSleepMilliseconds));

          if (space->dirtySegments)
          {
              gcScan(&gcContext, space);
          }
      }
    };

    ProtoSpace::ProtoSpace()
    {
        this->state = SPACE_STATE_RUNNING;

        auto* creationContext = new ProtoContext(this); // This context will be managed by the main thread stack
        this->threads = creationContext->newSparseList();

        this->mainThreadId = std::this_thread::get_id();
        this->mutableLock.store(false);
        this->threadsLock.store(false);
        this->gcLock.store(false);
        this->mutableRoot.store(new(creationContext) ProtoSparseListImplementation(creationContext));

        this->runningThreads.store(0);
        this->maxAllocatedCellsPerContext = MAX_ALLOCATED_CELLS_PER_CONTEXT;
        this->blocksPerAllocation = BLOCKS_PER_ALLOCATION;
        this->heapSize = 0;
        this->freeCellsCount = 0;
        this->gcSleepMilliseconds = GC_SLEEP_MILLISECONDS;
        this->maxHeapSize = MAX_HEAP_SIZE;
        this->blockOnNoMemory = false;
        this->gcStarted = false;
        this->freeCells = nullptr;

        // Pre-allocate the emergency buffer for OOM handling.
        constexpr size_t EMERGENCY_BUFFER_SIZE = 100 * 1024; // 100KB
        this->emergency_buffer = new(std::nothrow) char[EMERGENCY_BUFFER_SIZE];
        if (!this->emergency_buffer) {
            // If we can't even allocate the emergency buffer, it's a fatal, unrecoverable error.
            fprintf(stderr, "FATAL: Could not allocate the initial emergency memory buffer.\n");
            std::exit(1);
        }
        this->emergency_ptr = this->emergency_buffer;
        this->emergency_end = this->emergency_buffer + EMERGENCY_BUFFER_SIZE;
        this->emergency_allocator_active.store(false);

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
            ProtoString::fromUTF8String(creationContext, "Main thread"),
            this
        );
        this->allocThread(creationContext, const_cast<ProtoThread*>(mainThread));
        this->rootContext = creationContext;

    };

    ProtoSpace::~ProtoSpace()
    {
        ProtoContext finalContext(nullptr);

        const unsigned long threadCount = this->threads->getSize(&finalContext);

        this->state = SPACE_STATE_ENDING;

        // Wait till all threads are ended
        for (int i = 0; i < threadCount; i++)
        {
            auto t = this->threads->getAt(
                (ProtoContext*)this->rootContext,
                i
            )->asThread(this->rootContext);
            t->join(this->rootContext);
        }

        this->triggerGC();

        this->gcThread->join();

        // Free the emergency buffer
        delete[] this->emergency_buffer;
    };

    void ProtoSpace::triggerGC() const
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
            this->threads = this->threads->setAt(
                context,
                thread->getName(context)->getHash(context),
                thread->asObject(context)
            );

        this->threadsLock.store(false);
        this->runningThreads.fetch_add(1);
    };

    void ProtoSpace::deallocThread(ProtoContext* context, ProtoThread* thread)
    {
        bool oldValue = false;
        while (this->threadsLock.compare_exchange_strong(
            oldValue,
            true
        ))
            std::this_thread::yield();

        unsigned long threadCount = this->threads->getSize(context);
        while (threadCount--)
        {
            const ProtoThread* t = this->threads->getAt(context, threadCount)->asThread(context);
            if (t == thread)
            {
                this->threads = this->threads->removeAt(context, threadCount);
                break;
            }
        }

        this->threadsLock.store(false);
        this->runningThreads.fetch_sub(1);
    };

    void ProtoSpace::deallocMutable(unsigned long mutable_ref)
    {
        ProtoSparseList* currentRoot;
        ProtoSparseList* newRoot;

        do
        {
            currentRoot = this->mutableRoot.load();
            // Create a new root with the reference removed.
            newRoot = const_cast<ProtoSparseList*>(currentRoot->removeAt(nullptr, mutable_ref));
        }
        // Atomically update the root pointer.
        while (!this->mutableRoot.compare_exchange_strong(
            currentRoot,
            newRoot
        ));
    }


    Cell* ProtoSpace::getFreeCells(const ProtoThread* currentThread)
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

                        const_cast<ProtoThread*>(currentThread)->synchToGC();

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
                    // printf(
                    //    "\nmalloc of %d bytes, from current %d already allocated\n",
                    //    toAllocBytes,
                    //    this->heapSize
                    // );

                    BigCell* newBlocks = static_cast<BigCell*>(malloc(toAllocBytes));


                    if (!newBlocks) {
                        if (this->emergency_allocator_active.load()) {
                            // We've already entered emergency mode. A failure to get a large
                            // block of memory now is truly fatal and unrecoverable.
                            fprintf(stderr, "FATAL: Out of memory while trying to replenish cell pool, even after entering emergency mode.\n");
                            std::exit(1);
                        }

                        // This is the first OOM failure. Activate emergency mode and notify the application.
                        this->emergency_allocator_active.store(true);
                        throw std::bad_alloc();
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

                        // Chain new blocks as a list.
                        ((Cell*)currentBlock)->next = lastBlock;
                        lastBlock = currentBlock++;
                    }

                    this->freeCells = (Cell*)lastBlock;

                    this->heapSize += toAllocBytes;
                    this->freeCellsCount += allocatedBlocks;
                }
            }

            if (this->freeCells)
            {
                newBlock = this->freeCells;
                this->freeCells = newBlock->next;

                this->freeCellsCount -= 1;
                newBlock->next = previousBlock;
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

    const ProtoList* ProtoSpace::getThreads(ProtoContext *c)
    {
        return (const ProtoList*)this->threads;
    }

    const ProtoThread* ProtoSpace::newThread(
        ProtoContext *c,
        const ProtoString* name,
        ProtoMethod mainFunction,
        const ProtoList* args,
        const ProtoSparseList* kwargs)
    {
        auto* newThreadImpl = new(c) ProtoThreadImplementation(
            c,
            name,
            this,
            mainFunction,
            args,
            kwargs
        );
        const auto* newThread = newThreadImpl->asThread(c);
        this->allocThread(c, const_cast<ProtoThread*>(newThread));
        return newThread;
    }

    void ProtoSpace::deallocThread(ProtoContext* context, ProtoThread* thread)
    {
        bool oldValue = false;
        while (this->threadsLock.compare_exchange_strong(
            oldValue,
            true
        ))
            std::this_thread::yield();

        unsigned long threadCount = this->threads->getSize(context);
        while (threadCount--)
        {
            const ProtoThread* t = this->threads->getAt(context, threadCount)->asThread(context);
            if (t == thread)
            {
                this->threads = this->threads->removeAt(context, threadCount);
                break;
            }
        }

        this->threadsLock.store(false);
        this->runningThreads.fetch_sub(1);
    };

    Cell* ProtoSpace::getFreeCells(const ProtoThread* currentThread)
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

                        const_cast<ProtoThread*>(currentThread)->synchToGC();

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
                    // printf(
                    //    "\nmalloc of %d bytes, from current %d already allocated\n",
                    //    toAllocBytes,
                    //    this->heapSize
                    // );

                    BigCell* newBlocks = static_cast<BigCell*>(malloc(toAllocBytes));

                    
                    if (!newBlocks) {
                        if (this->emergency_allocator_active.load()) {
                            // We've already entered emergency mode. A failure to get a large
                            // block of memory now is truly fatal and unrecoverable.
                            fprintf(stderr, "FATAL: Out of memory while trying to replenish cell pool, even after entering emergency mode.\n");
                            std::exit(1);
                        }

                        // This is the first OOM failure. Activate emergency mode and notify the application.
                        this->emergency_allocator_active.store(true);
                        throw std::bad_alloc();
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

                        // Chain new blocks as a list.
                        ((Cell*)currentBlock)->next = lastBlock;
                        lastBlock = currentBlock++;
                    }

                    this->freeCells = (Cell*)lastBlock;

                    this->heapSize += toAllocBytes;
                    this->freeCellsCount += allocatedBlocks;
                }
            }

            if (this->freeCells)
            {
                newBlock = this->freeCells;
                this->freeCells = newBlock->next;

                this->freeCellsCount -= 1;
                newBlock->next = previousBlock;
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

    const ProtoList* ProtoSpace::getThreads(ProtoContext *c)
    {
        return (const ProtoList*)this->threads;
    }

    const ProtoThread* ProtoSpace::newThread(
        ProtoContext *c,
        const ProtoString* name,
        ProtoMethod mainFunction,
        const ProtoList* args,
        const ProtoSparseList* kwargs)
    {
        auto* newThreadImpl = new(c) ProtoThreadImplementation(
            c,
            name,
            this,
            mainFunction,
            args,
            kwargs
        );
        const auto* newThread = newThreadImpl->asThread(c);
        this->allocThread(c, const_cast<ProtoThread*>(newThread));
        return newThread;
    }

};
