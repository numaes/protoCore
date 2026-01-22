# Technical Analysis: ProtoCore

**Date:** 2026-01-22
**Project:** ProtoCore
**Type:** Embeddable Dynamic Object System / Runtime
**Language:** C++20

## 1. Executive Summary

ProtoCore is a high-performance, embeddable runtime library written in C++20. It bridges the gap between the flexibility of dynamic, prototype-based object systems (similar to JavaScript or Self) and the raw performance and concurrency of C++. Key features include a GIL-free architecture, true parallelism, immutable-by-default data structures, and a hybrid memory model designed for modern hardware cache locality.

## 2. Architecture & Design Philosophy

The project is built upon four pillars:
1.  **Immutability**: Core data structures are persistent and immutable using structural sharing.
2.  **Hardware-Aware Memory**: Tagged pointers, 64-byte aligned cells, and per-thread arenas.
3.  **Prototype Object Model**: Dynamic inheritance via delegation (prototypes) rather than classes.
4.  **Concurrency**: Designed for multi-core scaling without a Global Interpreter Lock (GIL).

### 2.1 Memory Model

ProtoCore employs a sophisticated memory management strategy:

*   **Tagged Pointers (`ProtoObject*`)**: A 64-bit handle that can represent either an immediate value (SmallInteger, Boolean) or a pointer to a heap-allocated `Cell`. This avoids allocation for common scalar values.
*   **Cell-Based Heap**: All heap objects are stored in fixed-size 64-byte `Cell`s. This alignment matches standard CPU cache lines, eliminating false sharing between threads.
*   **Per-Thread Arenas**: Each thread allocates from a local pool, requiring no locks for the vast majority of allocations.
*   **Garbage Collection**: A concurrent Mark-and-Sweep collector with a dedicated GC thread. It uses a "stop-the-world" pause only for scanning roots, followed by concurrent marking/sweeping. It also employs an implicit generational approach by tracking new allocations per `ProtoContext`.

### 2.2 Object Model

*   **Prototypes**: Objects inherit behavior via a prototype chain.
*   **ProtoObjectCell**: The internal representation of an object, containing a `ParentLink` (prototype) and a `ProtoSparseList` (attributes).
*   **Modifiability**: While defaults are immutable, controlled mutation is possible via a thread-safe `mutableRoot` sparse list and atomic compare-and-swap operations.

### 2.3 Data Structures

The library implements persistent data structures to support efficient copy-on-write semantics:

*   **Ropes**: Used for `ProtoString` and `ProtoTuple` to enable efficient concatenation and slicing without massive copying.
*   **Balanced Trees**: Used for `ProtoList` and `ProtoSparseList` to guarantee O(log n) operations.
*   **Interning**: Tuples are interned explicitly to allow pointer-equality checks.

## 3. Codebase Structure

The project follows a clean separation between public API and internal implementation.

### 3.1 Directory Layout

*   `core/`: Contains the implementation files (`.cpp`) for all logic.
*   `headers/`:
    *   `protoCore.h`: Public API. Defines opaque handles and trampoline methods.
    *   `proto_internal.h`: Internal implementation details (`*Implementation` classes).
*   `test/`: Unit tests using Google Test.
*   `performance/`: Benchmarks for threading and data structure performance.
*   `ext/` or `_deps/`: Dependencies managed via CMake.

### 3.2 Key Classes

*   **Runtime**:
    *   `ProtoSpace`: The global runtime environment (GC, interning, thread list).
    *   `ProtoContext`: Per-thread execution state (call stack, local memory arena).
    *   `ProtoThread`: New OS thread wrapper.
*   **Types**:
    *   `ProtoObject`: Base handle.
    *   `ProtoList`, `ProtoTuple`, `ProtoString`: Collection types.
    *   `SmallInteger`, `LargeInteger`, `Double`: Numeric hierarchy.

## 4. Build System

*   **System**: CMake (>= 3.16).
*   **Compiler Config**: strict C++20 requirement (`CMAKE_CXX_STANDARD 20`).
*   **Targets**:
    *   `proto` (Static Library): The core runtime.
    *   `immutable_sharing_benchmark`: Performance validation.
    *   `concurrent_append_benchmark`: Threading validation.
    *   `tests`: Unit test suite (enabled via `enable_testing()`).

## 5. Technology Stack

*   **Language**: C++20 (Concepts, modules support readiness, modern stdlib).
*   **Testing**: Google Test (gtest/gmock).
*   **Documentation**: Doxygen, Sphinx, Breathe.
*   **Build**: CMake, Make.

## 6. Conclusion and Recommendations

ProtoCore is a robustly architected system suitable for embedding scripting capabilities into performance-critical applications. Its design decisions (tagged pointers, immutability) align with high-frequency trading or real-time simulation requirements where latency and safe concurrency are paramount.

**Recommendations for Contributors:**
*   Adhere strictly to the `headers/protoCore.h` vs `headers/proto_internal.h` separation.
*   Use `toImpl` and `as...` helpers for safe type casting.
*   Respect the immutable nature of core types; use the defined mutation APIs only when necessary.
