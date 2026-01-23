# Garbage Collector Implementation in protoCore

This document describes the design and implementation of the Garbage Collector (GC) in the `protoCore` project.

## Overview

The `protoCore` GC is a concurrent Mark & Sweep collector with a Stop-The-World (STW) phase for root collection. It is designed to work in a multi-threaded environment where each thread manages its own "young generation" of objects before submitting them to the global heap.

## Key Components

### 1. ProtoSpace
The `ProtoSpace` manages the global heap, the free list, and the GC thread. It coordinates the Stop-The-World synchronization and the mark-and-sweep cycle.

### 2. ProtoContext
Each `ProtoContext` (representing a call stack frame) tracks objects allocated within its execution scope. It maintains a linked list of "young" cells (`lastAllocatedCell`).

### 3. DirtySegment
When a context is destroyed upon function return, its "young generation" chain (`lastAllocatedCell`) is submitted to `ProtoSpace` as a `DirtySegment`. This ensures that objects remain safe and local to the context while the method is executing, and only become candidates for collection once the context is gone.

## The GC Cycle

The GC runs in a dedicated background thread (`gcThreadLoop`) and follows these phases:

### Phase 1: Stop-The-World (STW)
- Sets the `stwFlag` to `true`.
- Waits for all application threads to reach a "parked" state (synchronization points).
- Application threads check this flag during memory allocation or explicit synchronization calls (`synchToGC`).

### Phase 2: Root Collection
While the world is stopped, the GC identifies all root objects:
- **Global Roots**: Key prototypes and global objects in `ProtoSpace`.
- **Context Stacks**:
    - Automatic and closure locals are scanned.
    - The `lastAllocatedCell` chain of every active context is scanned for references. **Crucially**, the cells in this chain are NOT promoted or marked themselves during this phase; instead, only the objects they reference are added to the mark list. This "pins" the young objects while keeping them unmarked, allowing them to be reclaimed in the first GC cycle after their context is destroyed.
- **Heap Snapshot**: The list of `DirtySegments` (already promoted from previously destroyed contexts) is captured for the mark-and-sweep phases.

### Phase 3: Resume The World
- Once roots are safely collected into a work-list, the `stwFlag` is cleared.
- Application threads are resumed and can continue execution and allocation while the GC proceeds to marking.

### Phase 4: Mark
- Performs a depth-first traversal starting from the roots.
- Uses the `processReferences` virtual method on `Cell` implementations to discover reachable objects.
- Reachable objects are marked using a bit-flag in the `Cell::next_and_flags` atomic member (bit 0). This is highly efficient as it avoids the allocation and hashing overhead of a separate set.

### Phase 5: Sweep
- Iterates through the **captured snapshot** of `DirtySegments`.
- For each cell in a segment:
    - If it was **not** marked, it is finalized (`finalize()`) and returned to the global `freeCells` list.
    - If it was marked, the flag is cleared (`unmark()`), and it remains in the heap for the next cycle.
- Cleans up processed `DirtySegments` from the snapshot. New segments added by threads during the Mark phase are ignored and will be processed in the next cycle.

## Optimization Features
- **Inline Caching**: Per-thread attribute caches (`ProtoThreadExtension::attributeCache`) speed up prototype chain traversals.
- **Bit-Marking**: Efficient marking using the lower bits of aligned pointers in the cell chain.
- **Atomic References**: Thread-safe `mutable_ref` generation using an atomic counter in `ProtoSpace`.

## Future Improvements
- **Multi-threaded Marking**: Parallelize the mark phase to handle very large heaps.

## Synchronization Mechanisms

- `globalMutex`: Protects access to shared memory structures like `freeCells`, `dirtySegments`, and the thread list.
- `stwFlag` & `parkedThreads`: Manage the Stop-The-World synchronization.
- `gcCV`: Condition variable to trigger the GC thread and coordinate parked threads.

## Memory Allocation

- Threads request batches of cells from `ProtoSpace` (`getFreeCells`).
- If the free list is low, the GC is triggered.
- If no free cells are available even after a GC, `ProtoSpace` allocates a new chunk of memory from the OS using `posix_memalign`.

## How to use

The GC is mostly automatic. However, threads must be "managed" by `ProtoSpace` to participate in the STW protocol. Use `ProtoThread` and its synchronization methods to ensure proper GC behavior in custom threading scenarios.

```cpp
// Example of manual synchronization if needed
thread->synchToGC();
```
