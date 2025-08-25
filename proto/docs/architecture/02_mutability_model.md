# Architecture Deep Dive: Lock-Free Mutability

## Introduction

Managing shared mutable state is one of the most difficult challenges in concurrent programming. Traditional approaches rely on locks (like mutexes) to protect data from being corrupted by multiple threads accessing it simultaneously. However, locks introduce their own problems: performance bottlenecks, reduced scalability, and the potential for deadlocks. Proto avoids these issues by adopting a lock-free model for all state mutations.

## Proto's Solution: Identity vs. State

Proto's model is built on a fundamental separation between an object's **identity** and its **state**. An object's identity is a fixed, immutable reference. Its state, however, is not stored within the object itself but in a global, concurrent map called `mutableRoot`.

When you change an attribute of an object, you are not modifying the object's memory directly. Instead, you are atomically updating the `mutableRoot` table to map the object's identity to its new state.

### The `mutableRoot` Table

The `mutableRoot` is the heart of Proto's concurrency model. It is a `ProtoSparseList` (a highly efficient, sparse array) wrapped in a `std::atomic`. This atomic wrapper is the key that enables lock-free updates. All modifications to the shared state of the entire Proto system happen by atomically swapping the pointer to this table.

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

The `setAttribute` function in `Proto.cpp` perfectly illustrates the lock-free update pattern.

```cpp
// Illustrative snippet from Proto.cpp
void Proto::setAttribute(ProtoObject* target, ProtoObject* key, ProtoObject* value) {
    // Loop until the atomic update succeeds
    while (true) {
        // 1. Atomically load the current root
        ProtoObject* oldRoot = this->space->mutableRoot.load();

        // 2. Create a modified copy
        ProtoObject* newRoot = this->space->setAttribute(oldRoot, key, value);

        // 3. Attempt to swap the root pointer
        if (this->space->mutableRoot.compare_exchange_strong(oldRoot, newRoot)) {
            // Success! The change is committed.
            break;
        }
        // 4. If it failed, another thread made a change.
        // The loop will repeat with the new state.
    }
}
```
