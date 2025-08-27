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

## Getting Started & Building the Project

The project now uses **CMake** for a modern, cross-platform build system. This allows you to build Proto easily on Linux, macOS, and Windows with a variety of compilers and IDEs.

### Prerequisites

*   **CMake** (version 3.16 or higher)
*   A modern C++ compiler that supports C++20 (e.g., **GCC 10+**, **Clang 12+**, **MSVC v142 - VS 2019+**)

### Compilation Instructions

1.  **Clone the repository:**
    ```bash
    git clone <repository_url>
    cd proto
    ```

2.  **Configure the project with CMake:**
    This is the standard "out-of-source" build process. It keeps your source directory clean.
    ```bash
    mkdir build
    cd build
    cmake ..
    ```

3.  **Compile the project:**
    After CMake has generated the build files, you can compile everything.
    ```bash
    make
    ```
    (Or, on Windows with Visual Studio, you would open the generated `.sln` file and build from there).

4.  **Run the benchmarks:**
    The compiled executables will be located in the `build` directory.
    ```bash
    ./list_benchmark
    ./immutable_sharing_benchmark
    # ... and so on for the other benchmarks
    ```

### Cleaning the Build

To clean the build artifacts, simply remove the `build` directory:
```bash
cd ..
rm -rf build
```
