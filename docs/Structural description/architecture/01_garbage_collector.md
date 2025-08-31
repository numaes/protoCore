# Architecture Deep Dive: The Low-Latency Garbage Collector

## Introduction

In many high-level languages, garbage collection (GC) is a source of unpredictable pauses, often called "stop-the-world" (STW) events. For latency-sensitive applications like real-time trading systems, game engines, or interactive tools, these pauses can be unacceptable. Proto's garbage collector is engineered from the ground up to minimize this disruption, providing a foundation for systems that require smooth, predictable performance.

## Core Principles

### Concurrent Collection

The core of Proto's GC design is concurrency. The majority of the GC work—specifically the marking of live objects and the sweeping of dead ones—happens in a background thread, running in parallel with the application's own threads. This means the application can continue to execute logic, allocate memory, and respond to events while the GC is cleaning up.

### Minimal Stop-The-World (STW)

While most of the GC cycle is concurrent, there is a brief, mandatory STW phase: **root scanning**. This is the only moment the application threads are paused. Proto's architecture is designed to make this phase as fast as possible. By carefully managing how and where mutable state can exist, the number of roots the GC needs to scan is dramatically reduced.

### No Compaction

Proto's GC does not move or compact objects in memory. Once an object is allocated, it stays at that memory address for its entire lifetime. This design choice has two major benefits:
1.  **FFI Simplicity:** C/C++ code can hold direct pointers to Proto objects without fear that the GC will invalidate them by moving the underlying memory. This makes the Foreign Function Interface (FFI) significantly simpler and more performant.
2.  **Predictable Performance:** Memory compaction is a costly operation. By avoiding it, the GC's behavior is more predictable and its impact on application performance is minimized.

## The Collection Cycle

### Triggering a GC

A garbage collection cycle is typically triggered automatically when the total memory allocated by the runtime exceeds a certain threshold.

### Root Scanning (The STW Phase)

This is the critical, synchronous part of the cycle. The GC pauses all application threads and scans for "roots"—the set of objects that are directly accessible by the application. In Proto, the roots are:

1.  **Thread Stacks & Contexts:** The local variables and execution state of each running thread.
2.  **The Global `mutableRoot`:** A single, global table that holds all shared, mutable state. This is a key architectural feature that simplifies root scanning immensely.
3.  **The Thread `method_cache`:** A cache used to speed up method lookups. It's crucial to scan this cache, as it may contain references to objects that would otherwise be considered dead, preventing potential `use-after-free` bugs.

### Concurrent Marking & Sweeping

Once the root scan is complete, the application threads are resumed. The GC's background thread then begins the concurrent **mark phase**, traversing the object graph starting from the roots to find all live objects. Following that, the **sweep phase** reclaims the memory of any unmarked, unreachable objects.

## Code Reference

To see how this is implemented, you can look at the following key areas in the source code:

**`Thread.cpp` - Processing Roots on the Stack:**
The `processReferences` method is responsible for iterating over the stack of a single thread to find root objects.

```cpp
// Illustrative snippet from Thread.cpp
void Thread::processReferences(std::function<void(const ProtoObject)> processor) {
    // ... logic to iterate over the current thread's stack ...
    for (auto& frame : call_stack) {
        for (auto& local : frame.locals) {
            processor(local);
        }
    }
    // ... also processes the method_cache ...
}
```

**`ProtoSpace.cpp` - The Main GC Scan Logic:**
The `gcScan` method orchestrates the STW phase, gathering roots from all threads and the global `mutableRoot`.

```cpp
// Illustrative snippet from ProtoSpace.cpp
void ProtoSpace::gcScan() {
    // 1. Pause all threads (The STW start)
    // ...

    // 2. Scan global roots
    processor(this->mutableRoot);

    // 3. Scan roots from each thread
    for (auto& thread : all_threads) {
        thread->processReferences(processor);
    }

    // 4. Resume all threads (The STW end)
    // ...
}
```
