# Architecture Deep Dive: Lock-Free Mutability and Mutable Yarding

## Introduction

In concurrent systems, the orchestration of shared mutable state is classically the most complex and performance-degrading challenge. Traditional threading models rely heavily on mutual exclusion locks (mutexes) to synchronize access to shared data, preventing corruption. However, mutexes inherently induce serialization, context-switching overhead, and the constant threat of deadlocks, neutralizing the benefits of multi-core architectures.

ProtoCore completely circumvents these bottlenecks by implementing a highly sophisticated, entirely lock-free mutability model. By strictly decoupling an object's structural identity from its logical state, Proto guarantees extreme scalability and thread safety under heavy concurrent mutation.

## The Paradigm: Identity vs. State Separation

In traditional object-oriented memory models (e.g., C++ or Java), an object's state is encapsulated within its allocated memory block. Mutating an attribute requires directly overwriting that memory, necessitating locks if multiple threads are involved.

Proto rejects this design. In Proto, every object allocated on the heap is structurally **immutable**. An object serves only as an immutable identity—a persistent reference handle.

The actual, mutable state of the object is stored externally in a global, highly concurrent map known as the `mutableRoot`. When a thread updates an attribute, it does not write to the object's memory. Instead, it atomically publishes a new state association for that object's identity within the `mutableRoot`.

## The `mutableRoot`: Sharded Lock-Free Architecture

To ensure the `mutableRoot` does not itself become a bottleneck, it is structurally engineered for massive parallel throughput.

The `mutableRoot` is partitioned into **256 independent shards**. Each shard is a discrete lock-free data structure represented by a `std::atomic<ProtoSparseList*>`.

*   **Deterministic Sharding:** When an object requires mutation, its identity (the `mutable_ref` integer) is bitwise-ANDed (`mutable_ref & 0xFF`) to deterministically resolve its corresponding shard.
*   **Zero Hardware Contention:** Because the 256 shards operate entirely independently, 256 distinct hardware threads can theoretically mutate 256 different objects concurrently without a single CPU cache line collision or atomic wait cycle. This translates to near-linear scaling on massive multi-core hardware.

### Mutable Yarding

To manage the lifecycle and allocation of the persistent data structures (like the `ProtoSparseList` nodes) that make up the `mutableRoot`'s state trees, ProtoCore employs a technique called **Mutable Yarding**.

A "Yard" is a specialized, localized memory allocator optimized specifically for rapid, lock-free allocation of mutation nodes. Instead of taxing the main garbage-collected heap for every tiny attribute update, state updates are allocated out of these yards. 

Mutable Yarding ensures that the memory backing the object states is tightly packed, highly cache-local, and rapidly allocatable. When the Garbage Collector runs, it can efficiently sweep these yards, quickly identifying and reclaiming state nodes that are no longer referenced by the active `mutableRoot` shards.

## The Lock-Free Update Protocol: Compare-And-Swap (CAS)

The engine driving the concurrent state updates is the optimistic Compare-And-Swap (CAS) algorithm. When a thread invokes an operation like `setAttribute`, the following deterministic sequence occurs entirely without locks:

1.  **Snapshot Loading:** The thread atomically loads the current root pointer of the target object's shard.
2.  **Structural Sharing (Copy-on-Write):** The thread creates a new, modified version of the shard's tree incorporating the new attribute value. Crucially, because the trees are immutable, this operation utilizes structural sharing, reusing the vast majority of the existing tree structure. This makes the "copy" operation exceptionally fast ($O(\log N)$).
3.  **Atomic Commitment:** The thread attempts to atomically swap the old shard pointer with the new shard pointer using `std::atomic::compare_exchange_strong`.
4.  **Optimistic Retry:** If the `compare_exchange` fails—meaning another thread successfully published an update to the exact same shard in the intervening microseconds—the thread immediately retries the operation from step 1, applying its update to the *newest* snapshot.

This CAS loop mathematically guarantees that all updates are safely committed without ever suspending a thread or invoking the OS scheduler. 

### Architectural Advantages

*   **Maximum Reader Throughput:** Threads reading state never block, never acquire locks, and never wait for writers. They simply read the most recently published snapshot.
*   **Immunity to Deadlocks:** The complete absence of locking mechanisms renders deadlocks mathematically impossible.
*   **Cache-Line Optimization:** The sharded architecture prevents "false sharing" and cache-line bouncing across CPU cores, maximizing L1/L2 cache efficiency.

## GC Root Scanning Synergy

The separation of state into the `mutableRoot` is the primary catalyst for Proto's ultra-low-latency Garbage Collector.

In a traditional model, the GC must painstakingly scan the entire heap, inspecting every object to find mutable pointers to other objects. In Proto, because the heap itself is immutable, the GC only needs to scan the `mutableRoot`. The `mutableRoot` acts as a single, comprehensive ledger of all inter-object mutability in the entire system. 

By scanning the shards of the `mutableRoot`, the GC instantly captures the exact topology of the live mutable state, drastically reducing the "Stop-The-World" root scanning pause to mere microseconds.
