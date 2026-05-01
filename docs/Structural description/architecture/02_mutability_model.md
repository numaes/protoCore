# Architecture Deep Dive: Lock-Free Mutability

## Introduction

Managing shared mutable state is one of the most difficult challenges in concurrent programming. Traditional approaches rely on locks (like mutexes) to protect data from being corrupted by multiple threads accessing it simultaneously. However, locks introduce their own problems: performance bottlenecks, reduced scalability, and the potential for deadlocks. Proto avoids these issues by adopting a lock-free model for all state mutations.

## Proto's Solution: Identity vs. State

Proto's model is built on a fundamental separation between an object's **identity** and its **state**. An object's identity is a fixed, immutable reference. Its state, however, is not stored within the object itself but in a global, concurrent map called `mutableRoot`.

When you change an attribute of an object, you are not modifying the object's memory directly. Instead, you are atomically updating the `mutableRoot` table to map the object's identity to its new state.

### The `mutableRoot` Sharded Table

The `mutableRoot` is the heart of Proto's concurrency model. To eliminate contention on high-core-count machines, it is organized into **256 independent shards**. Each shard is a `std::atomic<ProtoSparseList*>`.

*   **Sharding by ID**: The index of the shard is determined by `mutable_ref & 0xFF`.
*   **Isolation**: Modifications to an object only affect its specific shard, allowing 256 different threads to mutate 256 different objects simultaneously with zero hardware contention.
*   **Snapshot Caching**: Every thread maintains a local 1024-entry cache of resolved snapshots. This transforms the global atomic lookup into a local $O(1)$ check, providing elite performance for the common "own-thread" access pattern.

## The Lock-Free Update Pattern

To ensure thread-safe updates without locks, Proto uses an optimistic, atomic update pattern known as "compare-and-swap" (CAS). The `setAttribute` function is the canonical example of this pattern.

The process works like this:
1.  Atomically read the current `mutableRoot` pointer.
2.  Create a *copy* of the table.
3.  Modify the copy with the new attribute value.
4.  Attempt to atomically swap the original `mutableRoot` pointer with the pointer to the newly modified table. This operation (`compare_exchange_strong`) will only succeed if no other thread has modified the `mutableRoot` in the meantime.
5.  If it fails (meaning another thread "won" the race), the process repeats from step 1 with the now-updated table.

This `compare_exchange_strong` loop is the core of the lock-free pattern. It guarantees that updates are always applied correctly and atomically, without ever needing to lock the data structure.

### Benefits

*   **High Concurrency:** Readers are never blocked. Writers only briefly retry if they conflict. This leads to excellent scalability on multi-core systems.
*   **No Deadlocks:** Since no locks are ever acquired, deadlocks are impossible.
*   **Simplified Logic:** Programmers don't need to reason about complex lock acquisition orders.

## Synergy with the Garbage Collector

This mutability model is the single most important enabler for Proto's fast garbage collector. Because all shared mutable state is centralized in the `mutableRoot` table, the GC's root scanning phase is incredibly fast. Instead of needing to scan the entire heap for pointers or track mutations, the GC only needs to consider the `mutableRoot` object itself as a single, comprehensive root. This is a primary reason why the "stop-the-world" pause is so short.

## Code Reference

The `setAttribute` function in `ProtoObject.cpp` perfectly illustrates the lock-free update pattern.

```cpp
// Illustrative snippet from core/ProtoObject.cpp
void ProtoObject::setAttribute(ProtoContext* context, const ProtoString* name, const ProtoObject* value) {
    // 1. Identify the shard
    int shard_idx = this->mutable_ref & 0xFF;
    auto& shard = context->space->mutableRoot[shard_idx];

    while (true) {
        // 2. Load the current shard snapshot
        ProtoSparseList* oldSnapshot = shard.load();

        // 3. Create a modified copy (structural sharing)
        ProtoSparseList* newSnapshot = oldSnapshot->setAt(context, this->mutable_ref, value);

        // 4. Attempt to swap the shard root
        if (shard.compare_exchange_strong(oldSnapshot, newSnapshot)) {
            // Success! The change is committed.
            break;
        }
    }
}
```
