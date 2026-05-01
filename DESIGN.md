# Proto: Architectural Design

This document outlines the core architectural principles and design decisions behind the Proto library. For a full list of documentation, see [DOCUMENTATION.md](DOCUMENTATION.md). It is intended for developers who wish to understand the "why" behind the code, contribute to the project, or learn from its design.

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
| - Owns SymbolTable (64-shard string interning)  |
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

*   **If the tag indicates a pointer**, the remaining bits are the memory address of a `Cell` on the heap (e.g., Objects, Lists, SparseLists).
*   **If the tag indicates an embedded value**, the remaining bits store the value directly (e.g., a 56-bit integer, a boolean, or an **Inline String**).

#### Inline Strings (Embedded UTF-8 Pointer)

Strings up to **6 UTF-8 bytes** are stored directly within the 64-bit `ProtoObject*` handle using `POINTER_TAG_EMBEDDED_VALUE` (1) with `EMBEDDED_TYPE_INLINE_STRING` (4). The `inlineString` bitfield packs `inline_byte_count` (3 bits) and up to 48 bits of raw UTF-8 bytes (LSB = first byte), supporting any Unicode content whose encoded form fits in 6 bytes (e.g., up to 6 ASCII characters, 3 two-byte codepoints, or 2 three-byte CJK characters).

Pointer equality implies content equality for inline strings, eliminating the need for hash table lookups on the hot path. This is the single most important optimization for scalar operations: it makes common string comparisons as fast as integer equality and avoids triggering the garbage collector.

### The Memory Arena: Lock-Free, Per-Thread Allocation

*   **Fixed-Size Cells**: All heap-allocated objects reside in 64-byte memory blocks called `Cell`s. This strategy eliminates memory fragmentation and simplifies the allocator. The 64-byte size is chosen to align perfectly with the cache lines of modern CPUs, preventing "false sharing" in concurrent applications.
*   **Per-Thread Arenas**: Each `ProtoThread` maintains its own local pool of free `Cell`s. When an object needs to be allocated, the thread takes a cell from its local pool. This operation is the key to high-speed allocation: it is extremely fast and **requires no global lock**, allowing threads to allocate memory in parallel at full speed with zero contention.
*   **Global Space**: Only when a thread's local pool is exhausted does it request a new, large batch of cells from the global `ProtoSpace`. This amortization strategy minimizes global synchronization.

### The Garbage Collector (GC): Concurrent and Low-Latency

The GC is designed to minimize application pauses.

*   **Dedicated GC Thread**: The GC runs in its own background thread.
*   **Brief Stop-The-World Phase**: The GC only requires a very short "stop-the-world" pause. During this phase, all application threads are temporarily halted so the GC can safely and quickly scan the root set (thread stacks, global objects). This is the only moment of significant synchronization.
*   **Concurrent Mark and Sweep**: Once the roots are identified, the marking and sweeping phases can run concurrently while the application threads resume execution. The immutable nature of the data structures is critical here, as it guarantees that the object graph will not be modified during the concurrent marking phase.
*   **Cooperative GC Safepoints**: To ensure the GC can always reach a Stop-The-World (STW) state, long-running loops in embedders (like a bytecode interpreter) should periodically call `ProtoContext::safepoint()`. This is a low-overhead check (relaxed atomic load) that allows the thread to park if a GC cycle is pending. This prevents "GC starvation" where a single CPU-bound thread stalls the entire system.
*   **Implicit Generational Collection**: The `ProtoContext` object, which represents a function's scope, tracks all new cells allocated within it. When a context is destroyed (i.e., a function returns), it provides its list of newly-allocated cells to the `ProtoSpace`. This acts as a highly efficient, implicit form of generational GC, as most objects are short-lived and can be identified for collection very quickly.

### Concurrency Primitives: Recursive Locking

To prevent deadlocks during complex operations (e.g., allocation triggering GC, which then needs to access global metadata), ProtoCore utilizes a **Global Reentrant Mutex** (`globalMutex`).
*   **Reentrancy**: The `std::recursive_mutex` allows a single thread to acquire the same lock multiple times without deadlocking. This is essential for the hybrid execution model where high-level operations (like interning or thread creation) may trigger low-level memory management tasks that also require the global lock.
*   **Granularity**: While the system avoids a GIL, the `globalMutex` protects specific global registries (Tuple Dictionary, Thread List) and orchestrates the Stop-The-World phase. String interning uses a separate `SymbolTable` with fine-grained per-shard mutexes (one per 64 shards), avoiding contention on the global lock for string operations.

### Keeping ProtoObjects alive across allocation boundaries the GC cannot see

A runtime built on top of protoCore (a JS interpreter, a Python interpreter, a foreign function bridge) often needs to keep a `ProtoObject*` alive across an allocation boundary that the GC cannot see. The canonical examples are an asynchronous callback whose receiver is captured into a C++ lambda registered with an external event loop, and a language symbol (an attribute name, a keyword, a cached literal) that has to outlive every individual context that ever interacts with it.

protoCore offers two complementary mechanisms for this. They cover **different lifetimes** and choosing the right one is part of the embedder's design contract:

#### Mechanism A — Perpetual allocation via `ProtoContext* = nullptr`

When an object is conceptually a permanent fixture of the runtime — a Symbol that will be referenced for the entire lifetime of the process, a per-type prototype, a one-shot canonical constant — allocate it through a **null `ProtoContext`**. The unified `ProtoContext::allocCell()` path falls through to `posix_memalign`: the resulting Cell is *never* enrolled in any thread freelist and *never* added to a context's young-generation chain, so the GC's free-and-recycle machinery cannot see it. It survives forever.

```cpp
auto* impl = ProtoStringImplementation::fromUTF8Bytes(/*ctx=*/nullptr, bytes, len);
// `impl` and every Cell its AVL tree allocated are perpetual.
```

The key property is that *the entire reachable subgraph must be allocated null-context too*: a perpetual root that points to a normal GC-managed Cell is a ticking bomb because the GC sees no path to that child and will reclaim it. In practice this is enforced by passing a single `nullptr` through the construction call chain (`fromUTF8Bytes` → `buildAVL` → `new(ctx) ...`), since every allocation site in protoCore takes its `ProtoContext*` as a parameter and forwards it.

The canonical user is `SymbolTable::intern(strObj, is_strong=true)`: every strong symbol — including the implicit auto-intern that `ProtoObject::setAttribute` performs on a non-interned heap String — is allocated null-context. There is no per-cycle GC scan to mark them, and there is no SymbolTable bucket pointing to a Cell that might be reclaimed.

**When to use it**:
* The object is a singleton or a member of a closed, source-bounded set (attribute names, language keywords, type prototypes).
* You can guarantee its entire reachable graph is also null-context.
* You will never need to free it.

**When *not* to use it**:
* The object's lifetime is bounded (for example a few milliseconds for an async callback receiver).
* The object holds references into the regular GC-managed graph (transitive perpetual allocation would explode).
* You need the option to release it explicitly.

#### Mechanism B — `ProtoRootSet` (transient pinning)

For everything that doesn't fit the perpetual case, protoCore exposes `ProtoRootSet`. An embedder calls `ProtoSpace::createRootSet(name)` once at startup, then pins each transient `ProtoObject*` with `add(obj)` and releases it with `remove(handle)`. The set is GC-traced: during the STW root-collection phase the GC iterates every registered set via `ProtoSpace::forEachRootSet` and adds the pinned objects to the same worklist as thread stacks and global registries.

* **Per-embedder isolation.** Each embedder calls `createRootSet` once. Two embedders sharing a `ProtoSpace` never see each other's pins.
* **O(1) pin/unpin.** A flat slot array with a free list of recyclable indices. A short `std::mutex` guards the slot vector; under realistic embedder rates (a few thousand pins/sec) the lock is uncontended.
* **Generational handles.** Each handle carries a 32-bit generation in addition to the slot index, so a stale handle from a freed slot cannot accidentally resolve to a different object after the slot is reused.
* **Lifetime.** The embedder may call `destroyRootSet` explicitly. If it doesn't, `~ProtoSpace` frees any orphans after the GC thread has joined.

```cpp
auto* rs = space->createRootSet("my-runtime-async");
auto handle = rs->add(callback);
event_loop.enqueue([handle, rs]() {
    auto* cb = rs->resolve(handle);
    rs->remove(handle);
    invoke(cb);
});
```

**When to use it**:
* The object's reachability is asynchronous: it must survive a window where no JS-/Python-/embedder-side reference can reach it but a C++ continuation will.
* The transitive reachable graph is GC-managed (so tracing a single root pins the rest).
* You need explicit `add` / `remove`.

#### Decision matrix

| Lifetime | Reachable graph all perpetual? | Mechanism |
|---|---|---|
| Process-perpetual | Yes (or trivially extendable) | NULL `ProtoContext` allocation |
| Bounded async (microseconds to seconds) | Doesn't matter | `ProtoRootSet` |
| Anything in between | — | `ProtoRootSet` (safer; explicit release) |

The two mechanisms are **complementary, not interchangeable**: don't try to release a NULL-context allocation, and don't lean on `ProtoRootSet` for objects that are conceptually language vocabulary.

The full `ProtoRootSet` contract — `add`, `resolve`, `remove`, `forEachRoot`, plus `ProtoSpace::createRootSet` / `destroyRootSet` / `forEachRootSet` — lives in `headers/protoCore.h`.

---

## 2. The Data Model: Immutable and Efficient

All core collection types in Proto are implemented as persistent, immutable data structures, backed by self-balancing AVL trees. This provides efficient structural sharing and guarantees O(log n) performance for most operations.

* **`ProtoString`** — Three-tier architecture, all tiers sharing a uniform public API:

  * **Tier 1 — Embedded pointer** (`POINTER_TAG_EMBEDDED_VALUE`): strings ≤6 UTF-8 bytes encoded directly in the tagged pointer. Zero heap allocation; pointer equality implies content equality.

  * **Tier 2 — Symbol** (`POINTER_TAG_SYMBOL` = 22): interned strings used as identifiers and attribute keys. Backed by a `ProtoStringImplementation` wrapping a persistent AVL tree. A **64-shard `SymbolTable`** (one `std::mutex` per shard, double-checked locking) guarantees that equal content always returns the same pointer. Strong symbols (created by `createSymbol()` or ProtoSpace literals) are GC roots; weak symbols (auto-interned keys) are evictable via `removeWeak()`. Read-only lookups use the non-inserting `lookupByContent()` path with no allocation or lock side effects.

  * **Tier 3 — String** (`POINTER_TAG_STRING` = 6): non-interned heap strings for general text. Same AVL tree cells as Symbol but not deduplicated.

  * **Persistent AVL tree** — two 64-byte Cell types:
    * `StringLeafNode`: stores up to 32 UTF-8 bytes, FNV-1a `content_hash`, `char_count`, `is_partial` flag. `processReferences()` is a no-op.
    * `StringInternalNode`: left/right children, `total_chars`, `left_chars` (for O(log N) `charAt` navigation), `subtree_hash`, AVL `height`. `processReferences()` visits both children for GC tracing.

  * **Two primitives** compose all string operations:
    * `strConcat(ctx, a, b)` — O(log |h(a)−h(b)| + 1)
    * `strSplit(ctx, node, char_index)` — O(log N)

  * **Iterators**: `RopeCharacterIterator` (internal, byte-offset based, O(1) amortized per codepoint) and `ProtoStringIteratorImplementation` (public API, leaf-caching, fits in a 64-byte Cell, `hasNext()` is O(1)).

  * **Auto-interning**: `setAttribute` automatically interns non-interned String keys (tag 6 → Symbol). Attribute lookup methods (`getAttribute`, `hasAttribute`) use `SymbolTable::lookupByContent()` with no insertion side effects.

* **`ProtoTuple`**: Implemented as a rope of small, fixed-size leaf arrays forming a persistent AVL tree. `ProtoString` no longer uses tuples internally; tuple interning is managed by a separate global `tupleRoot` dictionary (BST).

* **Interning summary**:
  * **Tuples**: global `tupleRoot` dictionary (BST).
  * **Symbols**: `SymbolTable` (64-shard hash table, per-shard mutex).
  * **Inline Strings**: pointer equality is content equality — no interning table needed.

*   **Sets & Multisets**:
    *   **`ProtoSet`**: Implemented using a `ProtoSparseList` where keys are the hash of elements and values are the elements themselves.
    *   **`ProtoMultiset`**: Implemented as a "Bag of Counts". It uses a `ProtoSparseList` where keys are element hashes and values are `SmallInteger` counts, allowing for efficient allocation-free tracking of duplicates.

---

## 3. The Object Model: Prototypes and Controlled Mutability

Proto implements a flexible and dynamic object model inspired by the Self programming language and JavaScript.

*   **Controlled Mutability (The 256-Shard System)**: While the default is immutability, Proto provides a high-performance mechanism for controlled mutation.
    *   **Mutable Identity**: A mutable object (`ProtoObjectCell`) holds a unique 64-bit `mutable_ref` ID.
    *   **Global Side Table**: The ID refers to an entry in `ProtoSpace::mutableRoot`, which is organized into **256 independent shards** (indexed by `mutable_ref & 0xFF`). Each shard is a `std::atomic<ProtoSparseList*>`.
    *   **Lock-Free Mutation**: A "mutation" is a lock-free `compare-and-swap` (CAS) operation on the shard root. This replaces the old immutable snapshot with a new one (structural sharing via AVL). 256 shards eliminate contention even on high-core-count machines.
    *   **Validation via Pointer Equality**: Because AVL nodes and shard roots are immutable and newly allocated on change, pointer equality on a shard root guarantees content equality (no ABA problem at the snapshot level).

---

## 4. The Two-Tier Cache Architecture

To eliminate the $O(\log N)$ cost of AVL lookups and prototype chain walks on hot paths, ProtoCore implements a sophisticated two-tier per-thread caching system.

### Tier 1: Mutable Value Cache (Snapshot Resolution)
Every `ProtoThread` carries a 1024-entry `MutableValueCacheEntry` table. This short-circuits the resolution of a mutable ID to its current snapshot.
*   **Key**: `mutable_ref`.
*   **Validation**: The cache stores the `shard_root` pointer at the time of caching. A lookup reloads the current atomic `shard_root`; if it still matches the cached pointer, the `current_value` snapshot is returned in $O(1)$.
*   **Self-Healing**: If another thread CASes the shard root, the pointer equality check fails, forcing an authoritative AVL lookup and a cache refresh.

### Tier 2: Attribute Cache (Resolution & Inheritance)
A 1024-entry `AttributeCacheEntry` table accelerates `getAttribute` lookups, short-circuiting both the local AVL search and the entire prototype chain traversal.
*   **Hash Function**: `(reinterpret_cast<uintptr_t>(obj) ^ (reinterpret_cast<uintptr_t>(name) >> 6)) % 1024`.
*   **6-bit Shift Optimization**: Because objects are 64-byte aligned (bits 0-5 are zero), we right-shift the attribute pointer by 6 bits to recover high-entropy bits for the index, maximizing cache utilization.
*   **Consistency**: `setAttribute` uses the same hash to invalidate cache entries, ensuring that any write to an object is immediately visible to subsequent cached reads on the same thread.

### Performance Model
| Access Type | Latency | Complexity |
| :--- | :--- | :--- |
| **Cached Hit** | **~8-10 ns** | $O(1)$ |
| **AVL Search (Miss)** | **~11-14 ns** | $O(\log N)$ |
| **Mutable Snapshot** | **+2 ns overhead** | $O(1)$ amortized |

The small gap between a cache hit and a miss (~3 ns) is a result of the extreme efficiency of pointer-indexed AVL trees. This makes the system exceptionally robust to "cache thrashing" compared to systems using complex hidden classes or hash-based dictionaries.

---

## 5. Execution Model and Method Invocation

The execution in Proto is centered around the `ProtoContext` and the concept of "trampoline" calls.

### Method Invocation Lifecycle

1.  **Lookup**: When a method is called on a `ProtoObject`, the system first looks for the attribute in the object's own `ProtoSparseList`.
2.  **Delegation**: If not found locally, it recursively searches through the `ParentLink` chain.
3.  **Binding**: If the found value is a function (a `ProtoMethod`), it is wrapped in a `ProtoMethodCell` along with the receiver (`self`).
4.  **Invocation**: The `ProtoMethodCell::implInvoke` is called. This sets up the execution environment within the current `ProtoContext`.

### The `ReturnReference` Mechanism

Internally, Proto uses a special `ReturnReference` cell. This is not exposed to the user but serves as a way for methods to return values through the `ProtoContext` while still being managed by the garbage collector. It ensures that the return value is tracked as a root if a GC cycle occurs exactly during a return operation.

---

### Public Inline Helpers for Embedders
To maximize performance in hot paths (like bytecode dispatchers), `protoCore.h` provides `static inline` helpers that allow embedders to handle the most common cases without cross-DSO function calls:
*   `isSmallInt(obj)` / `asSmallInt(obj)`: Fast path for 54-bit signed integers.
*   `isObjectFast(obj)`: Quick tag check for object handles.
*   `ProtoContext::safepoint()`: Cooperative GC check.

---

## 6. Conclusion: A Synergistic Design

No single feature of Proto stands alone. The `const`-correct, immutable API is what makes the concurrent GC safe. The tagged-pointer system is what makes the use of `ProtoObject*` as a universal handle performant. The two-tier caching and 256-shard mutable architecture are what enable true, GIL-free concurrency with $O(1)$ amortized access. Together, these elements create a runtime that is uniquely positioned to offer both the flexibility of a dynamic language and the raw performance of modern C++.
