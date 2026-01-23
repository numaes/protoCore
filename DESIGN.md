# Proto: Architectural Design

This document outlines the core architectural principles and design decisions behind the Proto library. It is intended for developers who wish to understand the "why" behind the code, contribute to the project, or learn from its design.

## Core Philosophy

Proto is built on a set of synergistic principles designed to unify performance, flexibility, and concurrent safety:

1.  **Immutability by Default**: Core data structures are immutable to eliminate entire classes of concurrency bugs. Modifications produce new versions via structural sharing (copy-on-write), making parallel code safer and easier to reason about.
2.  **Performance through a Hardware-Aware Memory Model**: The memory model is designed for extreme speed and low latency, using techniques like tagged pointers, per-thread allocation, and cache-line alignment to work *with* the hardware, not against it.
3.  **Flexible Prototype-Based Object Model**: Instead of rigid class hierarchies, Proto uses a powerful object model based on Lieberman-style prototypes, allowing for dynamic and flexible object composition and inheritance.
4.  **Concurrency as a First-Class Citizen**: The entire system is architected for true, GIL-free parallelism. Safety is derived from the immutable data model, not from complex and error-prone locking.

---

## Gemini Implementation Guidelines

To ensure consistent and correct code generation, the following architectural rules must be strictly followed in all sessions:

1.  **Public API vs. Internal Implementation**:
    *   The public-facing API is defined in `headers/protoCore.h`. These classes (e.g., `ProtoObject`, `ProtoList`) are opaque "handles" for users of the library.
    *   The internal implementation classes are defined in `headers/proto_internal.h` and always have the `Implementation` suffix (e.g., `ProtoListImplementation`). These classes inherit from `Cell` and contain the actual data and logic.
    *   Public API methods are **never** virtual and should not be implemented in the header. They are "trampoline" functions that delegate the call to the corresponding internal implementation class.

2.  **Casting and Type Conversion**:
    *   To get from a public API object to its internal implementation, always use the `toImpl<...>()` template function. For example: `toImpl<const ProtoListImplementation>(myProtoList)`.
    *   To get from an internal implementation object back to its public API handle, use the `as...()` method (e.g., `myProtoListImpl->asProtoList(context)`).

3.  **Method Naming Convention**:
    *   Public API methods use standard camelCase (e.g., `myList->getSize(context)`).
    *   Internal implementation methods that are part of the public API's implementation are prefixed with `impl` (e.g., `myListImpl->implGetSize(context)`).

4.  **File Structure**:
    *   The implementation for a public class `ProtoFoo` and its internal counterpart `ProtoFooImplementation` should reside in `core/ProtoFoo.cpp`.
    *   All internal class definitions **must** be in `headers/proto_internal.h`.
    *   All public API class declarations **must** be in `headers/protoCore.h`.

---

## High-Level Architecture

At a high level, the system is composed of a few key entities:

```
+-------------------------------------------------+
|                  ProtoSpace                     |
| (The Global Runtime Environment)                |
|                                                 |
| - Manages the Heap & Garbage Collector (GC)     |
| - Owns the Object Prototypes (List, String...)  |
| - Manages the list of all active threads        |
| - Holds the global interned tuple dictionary    |
|                                                 |
| +--------------------+   +--------------------+ |
| |   ProtoThread 1    |   |   ProtoThread 2    | |
| | (Native OS Thread) |   | (Native OS Thread) | |
| | - Owns a Context   |   | - Owns a Context   | |
| +--------------------+   +--------------------+ |
+-------------------------------------------------+
         |
         | Owns
         v
+-------------------------------------------------+
|                  ProtoContext                   |
| (A Thread's Call Stack & Execution State)       |
|                                                 |
| - Points to the current ProtoSpace              |
| - Tracks the call stack (previous context)      |
| - Holds local variables for the current scope   |
| - Manages a per-thread memory arena             |
+-------------------------------------------------+
```

---

## 1. The Memory Model: Performance and Safety

The memory management system is a cornerstone of Proto's performance.

### The `ProtoObject*` Handle: Pointer or Immediate Value?

To avoid the overhead of heap allocation for simple values, Proto uses **tagged pointers**. A 64-bit `ProtoObject*` is not just a pointer; it's a "handle" that can represent either a heap object or an immediate value. The lowest bits of the address are used as a tag:

*   **If the tag indicates a pointer**, the remaining bits are the memory address of a `Cell` on the heap.
*   **If the tag indicates an embedded value**, the remaining bits store the value directly (e.g., a 56-bit integer, a boolean).

This is the single most important optimization for scalar operations, as it makes them as fast as primitive C++ types and avoids triggering the garbage collector.

### The Memory Arena: Lock-Free, Per-Thread Allocation

*   **Fixed-Size Cells**: All heap-allocated objects reside in 64-byte memory blocks called `Cell`s. This strategy eliminates memory fragmentation and simplifies the allocator. The 64-byte size is chosen to align perfectly with the cache lines of modern CPUs, preventing "false sharing" in concurrent applications.
*   **Per-Thread Arenas**: Each `ProtoThread` maintains its own local pool of free `Cell`s. When an object needs to be allocated, the thread takes a cell from its local pool. This operation is the key to high-speed allocation: it is extremely fast and **requires no global lock**, allowing threads to allocate memory in parallel at full speed with zero contention.
*   **Global Space**: Only when a thread's local pool is exhausted does it request a new, large batch of cells from the global `ProtoSpace`. This amortization strategy minimizes global synchronization.

### The Garbage Collector (GC): Concurrent and Low-Latency

The GC is designed to minimize application pauses.

*   **Dedicated GC Thread**: The GC runs in its own background thread.
*   **Brief Stop-The-World Phase**: The GC only requires a very short "stop-the-world" pause. During this phase, all application threads are temporarily halted so the GC can safely and quickly scan the root set (thread stacks, global objects). This is the only moment of significant synchronization.
*   **Concurrent Mark and Sweep**: Once the roots are identified, the marking and sweeping phases can run concurrently while the application threads resume execution. The immutable nature of the data structures is critical here, as it guarantees that the object graph will not be modified during the concurrent marking phase.
*   **Implicit Generational Collection**: The `ProtoContext` object, which represents a function's scope, tracks all new cells allocated within it. When a context is destroyed (i.e., a function returns), it provides its list of newly-allocated cells to the `ProtoSpace`. This acts as a highly efficient, implicit form of generational GC, as most objects are short-lived and can be identified for collection very quickly.

---

## 2. The Data Model: Immutable and Efficient

All core collection types in Proto are implemented as persistent, immutable data structures, backed by self-balancing AVL trees. This provides efficient structural sharing and guarantees O(log n) performance for most operations.

*   **`ProtoTuple` & `ProtoString`**: These are implemented as **ropes**, a tree structure where leaves are small, fixed-size arrays of data. This makes operations like concatenation, slicing, and insertion extremely efficient, even for very large strings and tuples, as it avoids massive data copies by simply creating new tree nodes that point to existing, shared data.
*   **`ProtoList` & `ProtoSparseList`**: These are also backed by balanced trees, ensuring that operations at any point in the collection (beginning, middle, or end) have consistent, logarithmic time complexity.
*   **Interning for Tuples & Strings**: To conserve memory and speed up equality checks, all **Tuples** and **Strings** are interned. This means distinct objects with identical content share the same memory address.
    *   **Tuples**: Managed by a global `tupleRoot` dictionary (BST).
    *   **Strings**: Managed by a global `stringInternMap` (Hash Set). Since Strings are implemented as wrappers around Tuples, checking string equality often reduces to checking pointer equality of the underlying interned tuple.

*   **Sets & Multisets**:
    *   **`ProtoSet`**: Implemented using a `ProtoSparseList` where keys are the hash of elements and values are the elements themselves.
    *   **`ProtoMultiset`**: Implemented as a "Bag of Counts". It uses a `ProtoSparseList` where keys are element hashes and values are `SmallInteger` counts, allowing for efficient allocation-free tracking of duplicates.

---

## 3. The Object Model: Prototypes and Controlled Mutability

Proto implements a flexible and dynamic object model inspired by the Self programming language and JavaScript.

*   **`ProtoObjectCell`**: A standard object is represented internally by a `ProtoObjectCell`. This cell contains:
    *   A `ParentLink` pointing to its prototype(s). The `ParentLinkImplementation` allows for a linked list of parents, effectively enabling multiple inheritance through delegation.
    *   A `ProtoSparseList` to hold its own local attributes.
*   **Prototype Chains**: Objects inherit behavior and data from their prototypes. The `ParentLink` forms a chain (or more accurately, a directed acyclic graph), allowing for multiple inheritance. Attribute lookup traverses this graph depth-first until a match is found or all paths end.
*   **Methods and Bindings**: When a function is retrieved from an object, it is often wrapped in a `ProtoMethodCell`. This cell binds the function to the `self` object, ensuring that when the method is invoked, it has access to the correct receiver.
*   **Controlled Mutability**: While the default is immutability, Proto provides a safe mechanism for controlled mutation. A mutable object holds a unique ID that refers to an entry in a global, thread-safe sparse list (`mutableRoot` in `ProtoSpace`). A "mutation" is an atomic `compare-and-swap` operation that replaces the immutable value associated with that ID, preserving the safety of the overall system while providing the convenience of mutable state.

---

## 4. Execution Model and Method Invocation

The execution in Proto is centered around the `ProtoContext` and the concept of "trampoline" calls.

### Method Invocation Lifecycle

1.  **Lookup**: When a method is called on a `ProtoObject`, the system first looks for the attribute in the object's own `ProtoSparseList`.
2.  **Delegation**: If not found locally, it recursively searches through the `ParentLink` chain.
3.  **Binding**: If the found value is a function (a `ProtoMethod`), it is wrapped in a `ProtoMethodCell` along with the receiver (`self`).
4.  **Invocation**: The `ProtoMethodCell::implInvoke` is called. This sets up the execution environment within the current `ProtoContext`.

### The `ReturnReference` Mechanism

Internally, Proto uses a special `ReturnReference` cell. This is not exposed to the user but serves as a way for methods to return values through the `ProtoContext` while still being managed by the garbage collector. It ensures that the return value is tracked as a root if a GC cycle occurs exactly during a return operation.

---

## Conclusion: A Synergistic Design

No single feature of Proto stands alone. The `const`-correct, immutable API is what makes the concurrent GC safe. The tagged-pointer system is what makes the use of `ProtoObject*` as a universal handle performant. The per-thread memory arenas are what enable true, GIL-free concurrency. Together, these elements create a runtime that is uniquely positioned to offer both the flexibility of a dynamic language and the raw performance of modern C++.
