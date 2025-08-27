# Proto: A High-Performance, Embeddable Dynamic Object System for C++

[![Language](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://isocpp.org/)
[![Build System](https://img.shields.io/badge/Build-Makefile-green.svg)](https://www.gnu.org/software/make/)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

**Proto is a powerful, embeddable runtime written in modern C++ that brings the flexibility of dynamic, prototype-based object systems (like those in JavaScript or Python) into the world of high-performance, compiled applications.**

It is designed for developers who need to script complex application behavior, configure systems dynamically, or build domain-specific languages without sacrificing the speed and control of C++. With Proto, you get an elegant API, automatic memory management, and a robust, immutable data model designed for elite concurrency.

---

## Core Features

*   **Dynamic Typing in C++**: Create and manipulate integers, floats, booleans, strings, and complex objects without compile-time type constraints.
*   **Prototypal Inheritance**: A flexible and powerful object model based on Lieberman prototypes. Objects inherit directly from other objects, allowing for dynamic structure and behavior sharing without the rigidity of classical inheritance.
*   **Immutable-by-Default Data Structures**: Collections like lists, tuples, and dictionaries are immutable. Operations like `append` or `set` return new, modified versions, eliminating a whole class of bugs related to shared state and making concurrent programming safer and easier to reason about.
*   **Elite Concurrency Model**: By leveraging immutability, Proto provides a foundation for true multi-core scalability, free from the limitations of mechanisms like Python's GIL.
*   **Low-Latency Automatic Memory Management**: A concurrent, stop-the-world garbage collector manages the lifecycle of all objects, freeing you from manual `new` and `delete` with minimal application pauses.
*   **Clean, Embeddable C++ API**: The entire system is exposed through a clear and minimal public API (`proto.h`), making it easy to integrate into existing C++ applications.

## The Proto Philosophy: A Unified Vision

Proto is more than a collection of features; it's a cohesive ecosystem built on a powerful, unified philosophy. It was designed to solve fundamental problems in software development that often force a choice between performance, safety, and flexibility.

*   **Performance and Flexibility, Reunited**: We believe developers shouldn't have to choose between the raw power of C++ and the dynamic expressiveness of languages like Python. Proto is architected to provide both, offering a runtime that can outperform traditional JIT-based systems like Node.js in real-world, CPU-bound workloads.
*   **Concurrency by Design, Not by Effort**: Instead of burdening the developer with locks, mutexes, and semaphores, Proto's immutable data model makes concurrent programming safe and simple by design.
*   **Solving the "Impedance Mismatch"**: The same data model and principles apply from the lowest level (in-memory objects) to the highest (on-disk persistence and cloud storage). This eliminates the costly and complex translation layers that plague modern applications.
*   **A Foundation for the Future**: With a clear path towards a high-performance `proto_python` compiler and trivial FFI integration, Proto aims to be the definitive runtime for the next generation of data-intensive applications, from high-frequency trading to large-scale AI.

Here's a new `README.md` for the Proto project, incorporating all the advanced concepts and architectural details discussed in our conversation:

---

# Proto: A Revolutionary High-Performance Immutable Object Runtime & Data Platform

Proto is not just a runtime; it's a **holistic, high-performance ecosystem** for dynamic, object-oriented programming. Designed from first principles in C++, it unifies in-memory object management, concurrent execution, and durable storage, challenging traditional software development paradigms. Proto is inspired by the dynamism of Self and JavaScript, the functional purity of Clojure, and the raw performance of C++, forging a path toward a new generation of scalable, robust, and expressive applications.

## Core Architectural Pillars & Innovations

Proto's unique power stems from the synergistic interplay of its deeply integrated architectural decisions:

### 1. Immutable-First, Prototype-Based Object Model
* **Pure Concurrency Safety:** All core data structures (`ProtoTuple`, `ProtoSparseList`, `Strings`, `Collections`) are inherently immutable. Operations that "modify" them actually produce new versions, sharing unchanged parts (`structural sharing`). This design inherently eliminates common concurrency pitfalls like race conditions and simplifies parallel programming.
* **Lieberman Prototypes & Multiple Inheritance:** Instead of rigid classes, objects are cloned from existing prototypes and can be modified at runtime. This offers unparalleled flexibility for rapid development and evolving systems. 
* **`PROTO_NONE` for Robustness:** Accessing non-existent attributes gracefully returns a `PROTO_NONE` value, preventing exceptions and enhancing resilience for evolving data schemas and migrations.

### 2. Sophisticated Memory Management & Concurrent GC
* **Tagged Pointers for Primitives:** Simple values (integers, floats, UTF-8 chars, dates, timestamps) are directly embedded within pointers using "tags," eliminating object allocation and GC overhead for these frequent types.
* **Per-Thread Fixed-Block Allocators (Arenas):** Each thread manages its own memory pools with fixed-size blocks. This ensures extremely fast, lock-free allocation and minimizes memory fragmentation, perfectly complementing the immutable model's object creation rate. 
* **Concurrent, Low-Latency Garbage Collector:** The custom GC (`triggerGC`, `processReferences`) is designed for concurrency (`gcThread`, `stopTheWorldCV`) and low-latency. It only briefly halts the world for root scanning, performing the bulk of its work in parallel, making Proto suitable for real-time and interactive applications.
* **Implicit Generational GC via Call Context:** Blocks created during a method call are primarily scoped to that call's context (`ProtoContext`). Only the return value "survives" and is promoted to the caller's context, acting like an efficient, implicit generational GC without complex write barriers. 

### 3. Native Multithreading & Isolated Spaces
* **True Concurrency:** Proto is built from scratch for multithreading, allowing it to fully leverage multi-core CPUs without a Global Interpreter Lock (GIL).
* **`ProtoSpace` for Isolation:** Multiple independent runtimes (`ProtoSpace`) can coexist within the same process, each with its own threads and resources. This provides robust sandboxing for plugins and efficient multi-tenant server architectures.
* **Unified String & Tuple Interning:** Strings and tuples with identical content are guaranteed to be the *exact same object in memory* across all `ProtoSpace` instances. This drastically reduces memory footprint and enables lightning-fast equality comparisons (pointer comparison). 

### 4. High-Performance Persistent Data Structures
* **AVL-Tree Backed Collections:** All collections (lists, sets, dictionaries, strings, tuples) are implemented as self-balancing AVL trees. 
* **Logarithmic Performance Everywhere (O(log n)):** This ensures consistent, efficient performance for access, insertion, and deletion at *any* point in the collection (beginning, middle, end), removing the "bad use case" problem of traditional linear collections. 
* **Efficient Iteration:** Optimized non-recursive iteration over collections enhances cache locality and practical performance.
* **Controlled Mutability:** Proto introduces a safe mechanism for mutable values, where a "mutable object" holds an ID pointing to a `SparseListGlobal` — a shared, global mapping. "Mutation" is an atomic re-mapping to a new immutable value, preserving concurrent safety and simplifying GC root scanning.

## The Proto Vision: From Runtime to Universal Data Platform

Proto's most ambitious aspect is its seamless extension from an in-memory runtime to a **unified, multi-platform object database and cloud persistence layer**. 

* **Unified Memory & Disk Format:** The same immutable, tree-based structures used in memory are inherently serializable, making persistence to disk a natural extension rather than a costly translation. 
* **ACID Transactions by Design:** Proto's copy-on-write philosophy naturally supports transactional behavior. Committing a transaction involves atomically updating a root pointer to a new immutable state, providing isolation and atomicity almost for free. 
* **Seamless Integration:** The embedded nature of Proto and its trivial FFI (Foreign Function Interface) allow Python code running on Proto to easily interact with external C/C++ libraries (e.g., NumPy, AI frameworks) without complex data copying or serialization. This opens the door to **Python's expressiveness with C++-level performance** for data science and AI workloads. 

## Maturity & Future

Proto is in an **active, deep development phase** (Alpha/Experimental). It's not yet a production-ready product, but a highly promising system for:
* **Game Engines:** Concurrent logic, real-time performance.
* **Professional Application Scripting/Plugins:** High-performance, secure embeddable engine.
* **High-Frequency Trading (HFT) & Finance:** Ultra-low latency, extreme numerical performance, high concurrency.
* **Next-Gen AI/ML & Scientific Computing:** Scalable data pipelines, native parallelism for complex computations.
* **Distributed Systems & Cloud-Native Data:** The unified memory-disk-cloud model is ideal for building highly scalable, versioned, and resilient data platforms. 

**Proto represents a truly unique synthesis of cutting-edge computer science concepts.** It aims to provide the concurrent efficiency of Erlang/Elixir, the dynamic flexibility of JavaScript/Self, the ergonomics of Python, the robust immutable data structures of Clojure/Haskell, and a performance profile that challenges hand-tuned C++. This project is poised to attract a passionate community interested in shaping the future of high-performance software.

---