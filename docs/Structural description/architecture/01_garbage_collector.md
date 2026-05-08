# Architecture Deep Dive: The Low-Latency Garbage Collector

## Introduction

In modern runtime environments, memory management is frequently the primary bottleneck for system latency. Traditional tracing Garbage Collectors (GC)—such as those found in the Java Virtual Machine (e.g., G1, ZGC) or the V8 JavaScript engine—often introduce unpredictable latency spikes known as "stop-the-world" (STW) events. For real-time applications, high-frequency trading platforms, or interactive engines, these pauses are strictly unacceptable.

ProtoCore's Garbage Collector is engineered from the ground up to eradicate these unpredictable pauses. By leveraging immutable cell structures and a lock-free mutable state repository, Proto provides a fully concurrent GC that operates almost entirely in parallel with user programs, delivering near real-time performance guarantees.

## Architectural Paradigm: Beyond Traditional GC

### The Java GC Comparison

To understand Proto's architectural leap, it is useful to contrast it with state-of-the-art enterprise collectors like Java's ZGC or G1. 

Traditional concurrent collectors must deal with a constantly shifting object graph: application threads are constantly modifying object references while the GC is trying to traverse them. To safely mark objects concurrently, traditional VMs employ **Write Barriers**. A write barrier is injected machine code that executes every time an object reference is updated, notifying the GC of the change. This introduces a persistent, systemic overhead to every single memory write operation in the application.

Furthermore, traditional GCs compact memory to prevent fragmentation, meaning objects are physically moved. This requires complex "read barriers" or heavy pointer forwarding mechanisms.

### The ProtoCore Advantage: Zero Write Barriers

Proto completely eliminates both write and read barriers. 

Because the underlying heap structure in Proto consists exclusively of **immutable 64-byte Cells**, the topology of the heap cannot change once allocated. An object's intrinsic memory structure is never modified. Consequently, when the background GC thread marks the object graph, it does so against a stable, immutable snapshot. 

Since the heap is immutable, there are no "mutations" for the GC to track during the marking phase. **Write barriers are entirely unnecessary.** This provides a massive, continuous performance advantage, allowing user programs to execute at native speeds without compiler-injected memory tracking overhead.

### No Memory Compaction

Proto's GC does not compact the heap. Once a Cell is allocated, its physical memory address remains stable until it is reclaimed. This design yields two critical advantages:
1. **Predictable Performance:** The system avoids the massive CPU and memory bandwidth costs associated with copying and moving memory segments.
2. **Foreign Function Interface (FFI) Synergy:** Native C/C++ code can securely hold direct raw pointers to Proto objects without the risk of the GC moving the object out from under them.

## The Collection Life Cycle

### 1. The Trigger
A GC cycle is heuristically triggered when the total memory footprint exceeds a dynamically managed threshold, operating concurrently on a dedicated background thread.

### 2. The Stop-The-World (STW) Phase
This is the only phase where application threads are paused. In Proto, the STW phase is rigorously engineered to be deterministic and exceptionally short—typically in the microsecond range. This brevity allows the system to operate effectively in real-time constraints.

During the STW phase, the GC performs the "Root Scan". The roots are strictly defined and localized:
*   **The Global `mutableRoot`:** Since all mutable state in the entire runtime is centrally stored in a 256-shard lock-free table, the GC can instantaneously capture the active mutation state.
*   **Thread Stacks & `protoContext`:** The local execution state, call stacks, and the local attribute caches inside each thread's `protoContext`.

### 3. Critical Sections and Thread Coordination
To safely enter the STW phase, the GC must bring all user threads to a safe halt. Proto implements cooperative yielding using **Critical Sections**.

When the GC requests a pause, a global flag is set. User threads periodically poll this flag at extremely low-cost, predictable intervals (e.g., at the end of bytecode loop iterations or function calls). When a thread observes the flag, it parks itself, yielding control to the GC. This cooperative model ensures that threads are paused at known, safe execution boundaries where the stack is fully verifiable, rather than arbitrary, unsafe instruction pointers. 

### 4. Concurrent Marking
Once the roots are captured, the STW phase terminates, and user programs immediately resume execution at full speed. 

The GC background thread then traverses the object graph starting from the roots. Because the graph topology (the Cells themselves) is immutable, the GC thread safely traverses the graph in parallel with user threads, identifying all live objects without requiring locking or barriers.

### 5. Concurrent Sweeping
Following the mark phase, the GC performs a sweep. It iterates through the memory allocator blocks, identifying any Cells that were not marked as live, and returns them to the free lists for future allocation.

## External Buffers and Shadow GC

Certain system objects necessitate external memory allocations outside the standard 64-byte Cell grid (e.g., large contiguous IO buffers). Proto handles this via **Shadow GC**.

Cells requiring external memory implement a virtual `finalize()` method. During the sweep phase, before the GC reclaims an unreachable cell, it invokes `finalize()`. The cell then safely executes `std::free` or `std::aligned_alloc` cleanup on its external segment. 

**The Stable-Address Contract:** The raw pointer to an external buffer returned via `ProtoExternalBuffer::getRawPointer(context)` is guaranteed to remain perfectly stable until the object is rigorously proven unreachable and collected. The lack of heap compaction ensures the address never shifts, providing uncompromised stability for high-performance memory mapping and IO operations.
