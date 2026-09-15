# Architecture Overview: The Garbage Collector

This overview summarizes protoCore's garbage collector. [GarbageCollector.md](../../GarbageCollector.md) describes each phase in detail.

## Design in Brief

protoCore uses a concurrent, non-moving mark-and-sweep collector that runs on a dedicated thread. Application threads stop only for a short stop-the-world phase in which the collector captures the roots; marking, sweeping and bulk unmarking then run while the application threads continue.

Two properties of the object model make this possible without write barriers:

- **Cells are immutable after construction.** The marker reads only fields that cannot change, so it traverses a stable graph.
- **All mutable state goes through 256 shard roots** (see [the mutability model](02_mutability_model.md)). A copy of those 256 pointers, taken during the stop-the-world phase, is a complete snapshot of mutable state for the cycle.

The cost of this approach is floating garbage: objects that become unreachable after the snapshot survive until the next cycle.

The collector does not move cells, so a cell keeps its address until it is reclaimed. A raw pointer held by native code does not keep a cell alive, however: the object must stay reachable from a root, such as a context's local variables, a `ProtoRootSet`, or a perpetual (null-context) allocation. [DESIGN.md](../../../DESIGN.md) describes these mechanisms.

## When a Cycle Starts

The GC thread sleeps until a cycle is requested. According to the trigger sources documented in `core/ProtoSpace.cpp`, a cycle is requested:

- when `ProtoSpace::triggerGC()` is called and fewer than 20% of the heap's cells are free;
- when a heap limit is configured with `ProtoSpace::setHeapLimits` and an allocating thread must wait for memory to be reclaimed.

With no heap limit (the default), allocation does not start collections by itself: `getFreeCells` grows the heap with memory from the operating system instead.

## The Stop-the-World Phase

1. The collector sets a stop-the-world flag and waits until every running thread is parked. Threads park at cooperative safepoints: in the allocator (`allocCell`), in `ProtoContext::safepoint()`, or by being inside an unmanaged region (`ProtoContext::UnmanagedScope`) around a blocking system call.
2. With all threads parked, it collects the roots, and only the roots: each thread's context chain (local variables, closure variables, return values, the pending root, and one handle per context for its young cells, the head of its young chain), the global roots (prototypes, the resolution chain, embedder `ProtoRootSet`s), and the snapshot of the 256 mutable-shard roots. It also records the tuple interner's published entry counts, takes the pending `DirtySegment`s for this cycle and, with the survivor re-chain, captures the survivor pen in O(1).
3. It clears the flag, and the threads resume.

Everything reachable from those roots is immutable, so the traversal, the references of young cells included, runs in the concurrent mark. The work in this phase depends on the number of threads and the depth of their context chains, not on the size of the heap or the number of young cells; this is what makes the collector soft real time. [GarbageCollector.md](../../GarbageCollector.md) estimates the cost of each component.

## Critical Sections

A thread that has allocated cells but not yet attached them to a root, for example while building a tree that it will publish with a final compare-and-swap, must not park: the collector could otherwise treat those cells as garbage. `ProtoContext::CriticalSection` is an RAII guard for such code. While a thread is inside a critical section, the safepoint checks skip parking, and the stop-the-world phase waits until the thread leaves the section or reaches a safepoint outside it. Critical sections should therefore be short and must not block.

## Concurrent Phases

- **Mark**: walks the young chains (and, with the survivor re-chain, the survivor pen) captured in the stop-the-world phase, then runs a depth-first traversal from the roots that marks reachable cells with a bit in each cell header. Only the collector uses the mark bit.
- **Sweep**: walks the cells in the segments taken for this cycle. Unmarked cells are finalized and returned to the free pool; marked cells are kept and examined again in later cycles. Segments submitted after the stop-the-world phase wait for the next cycle.
- **Bulk unmark**: clears the mark bits of the cells recorded during marking.

## External Buffers

`ProtoExternalBuffer` holds a contiguous memory segment outside the 64-byte cell heap, allocated with `std::aligned_alloc`. When the sweep reclaims an unreachable buffer cell, it calls the cell's `finalize()` method, which releases the segment with `std::free`. The address returned by `ProtoExternalBuffer::getRawPointer(context)` stays valid while the buffer object is reachable, so code that uses the memory must keep the object reachable for as long as it uses the address.
