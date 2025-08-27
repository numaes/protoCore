# Proto Architecture & Design

This document outlines the core architectural principles and design decisions behind the Proto library. Proto is a high-performance, embeddable runtime for a dynamic, prototype-based object system, engineered from the ground up in modern C++.

## Core Design Philosophy

Proto is built on a set of synergistic principles designed to unify performance, flexibility, and concurrent safety:

1.  **Immutability by Default**: Core data structures are immutable to eliminate entire classes of concurrency bugs. Modifications produce new versions via structural sharing (copy-on-write), making parallel code safer and easier to reason about.
2.  **Performance through Optimized Memory Management**: The memory model is designed for extreme speed and low latency, using techniques like per-thread allocation arenas, tagged pointers, and a concurrent garbage collector.
3.  **Flexible Prototype-Based Object Model**: Instead of rigid class hierarchies, Proto uses a powerful object model based on Lieberman-style prototypes, allowing for dynamic and flexible object composition and inheritance.
4.  **Concurrency as a First-Class Citizen**: The entire system is architected for true multi-core parallelism, with no Global Interpreter Lock (GIL). Safety is derived from the immutable data model, not from complex and error-prone locking.

---

## 1. High-Performance Memory Management

The memory management system is a cornerstone of Proto's performance.

### Tagged Pointers for Primitives

To avoid the overhead of heap allocation and garbage collection for common, simple values, Proto uses **tagged pointers**. A 64-bit `ProtoObject*` can represent either a true pointer to a heap object or an immediate value. The lowest bits of the pointer are used as a tag to distinguish the type:

*   **If the tag indicates a pointer**, the remaining bits are the memory address of a `Cell`.
*   **If the tag indicates an embedded value**, the remaining bits store the value directly (e.g., a 56-bit integer, a 32-bit float, a boolean, a date).

This optimization dramatically reduces memory pressure and improves performance for scalar operations.

### Cell-Based Allocation & Per-Thread Arenas

*   **Fixed-Size Cells**: All heap-allocated objects reside in fixed-size memory blocks called `Cell`s (specifically, `BigCell`, which is 64 bytes). This strategy eliminates memory fragmentation and simplifies the allocator.
*   **Per-Thread Arenas**: Each OS thread (`ProtoThread`) maintains its own local pool of free `Cell`s. When an object needs to be allocated, the thread takes a cell from its local pool. This operation is extremely fast and **requires no global lock**, minimizing contention and allowing threads to allocate memory in parallel at full speed.
*   **Global Space**: If a thread's local pool is exhausted, it requests a new batch of cells from the global `ProtoSpace`, which manages the main heap.

### Concurrent, Low-Latency Garbage Collector (GC)

The GC is designed to minimize application pauses and operate concurrently with the application threads.

*   **Dedicated GC Thread**: The GC runs in its own background thread (`gcThread`).
*   **Brief Stop-The-World Phase**: The GC only requires a very short "stop-the-world" pause. During this phase, all application threads are temporarily halted (`THREAD_STATE_STOPPED`) so the GC can safely and quickly scan the root set (thread stacks, global mutable objects).
*   **Concurrent Mark and Sweep**: Once the roots are identified, the marking and sweeping phases run concurrently while the application threads resume execution. The immutable nature of the data structures is critical here, as it guarantees that the object graph will not be modified during the concurrent marking phase.
*   **Implicit Generational Collection**: The `ProtoContext` object, which represents the call stack, plays a key role. Objects allocated within a function call are tied to its context. When the function returns, any objects that are not part of the return value become immediate candidates for collection. This acts as a highly efficient, implicit form of generational GC, as most objects are short-lived.

---

## 2. Immutable Data Structures

All core collection types in Proto are implemented as persistent, immutable data structures, backed by self-balancing AVL trees. This provides efficient structural sharing and guarantees O(log n) performance for most operations.

*   **`ProtoTuple` & `ProtoString`**: These are implemented as **ropes**, which are a specific type of tree structure. This makes operations like concatenation, slicing, and insertion/deletion extremely efficient, even for very large strings and tuples, as it avoids massive data copies.
*   **`ProtoList` & `ProtoSparseList`**: These are also backed by balanced trees, ensuring that operations at any point in the collection (beginning, middle, or end) have consistent, logarithmic time complexity.
*   **Interning for Tuples and Strings**: To conserve memory, all tuples and strings with identical content are **interned**. This means they are guaranteed to be the *exact same object in memory*. This is managed by a global `TupleDictionary` in the `ProtoSpace` and makes equality checks (which become simple pointer comparisons) incredibly fast.

---

## 3. Prototype-Based Object Model

Proto implements a flexible and dynamic object model inspired by the Self programming language and JavaScript.

*   **`ProtoObjectCell`**: A standard object is represented internally by a `ProtoObjectCellImplementation`. This cell contains:
    *   A `ParentLink` pointing to its prototype(s).
    *   A `ProtoSparseList` to hold its own local attributes.
*   **Prototype Chains & Multiple Inheritance**: Objects inherit behavior and data from their prototypes. The `ParentLink` forms a chain (or more accurately, a directed acyclic graph), allowing for multiple inheritance. Attribute lookup traverses this graph.
*   **Dynamic Modification**: Objects can be cloned (`clone`) or used as prototypes for new objects (`newChild`). Their attributes can be modified at any time.
*   **Controlled Mutability**: While the default is immutability, Proto provides a safe mechanism for controlled mutation. A mutable object holds a reference to an entry in a global, thread-safe sparse list (`mutableRoot` in `ProtoSpace`). A "mutation" is an atomic operation that replaces the immutable value associated with that reference, preserving the safety of the overall system.
