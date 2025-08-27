# Proto Runtime Library

The `proto` library is a high-performance runtime environment designed for programming languages, emphasizing immutable data structures, a flexible prototype-based object model, and an advanced concurrent garbage collection system.

## Key Features

### Immutable Data Structures

`proto` provides core data structures that are inherently immutable, enhancing system safety and concurrency:

*   **Lists (`ProtoList`):** Implemented as balanced trees, operations on lists return new instances rather than modifying the original, ensuring data persistence. Optimized for concurrent environments.
*   **Tuples (`ProtoTuple`):** Immutable collections similar to Python tuples. They are interned to optimize memory usage and comparisons, and are implemented using search trees for efficient access and slicing.
*   **Strings (`ProtoString`):** Immutable and built upon `ProtoTuple`, benefiting from the same immutability and deduplication optimizations. String operations also yield new string instances.

### Prototype-Based Object Model

Inspired by languages like JavaScript and Self, `proto` features a dynamic and flexible object model:

*   **Objects (`ProtoObjectCell`):** Objects link to a `parent` (prototype) and manage their `attributes` using a sparse list.
*   **Prototype-Based Inheritance:** Objects inherit properties and methods from their `parent` objects, with attribute lookups traversing the prototype chain.
*   **Cloning and Child Creation:** Objects can be cloned or new objects can be created that directly inherit from existing prototypes.
*   **Dynamic Attributes:** Attributes can be added or modified dynamically.
*   **Method Calling:** A robust mechanism for invoking object-associated functions through the prototype chain.
*   **Mutable Objects:** While core data structures are immutable, the system supports mutable objects, with their visibility and collection safely managed by the concurrent GC.

### Concurrent Memory Management and Garbage Collection (GC)

The `proto` runtime boasts a sophisticated memory management system designed for minimal overhead and high concurrency:

*   **Cell-Based Allocation:** All runtime objects are managed in fixed-size (64-byte) cells, eliminating fragmentation and enabling a fast, simple allocator.
*   **Per-Thread Memory Pools:** Each thread maintains its own pool of free cells, allowing for near-instantaneous, lock-free memory allocation.
*   **Dedicated Concurrent GC:** A dedicated GC thread (`gcThreadLoop`) operates in parallel with application threads, minimizing pauses.
*   **Hybrid GC (Partial Stop-the-World):** The GC employs a brief "stop-the-world" phase to safely collect system roots (thread stacks, global references). The subsequent mark and sweep phases run concurrently with application execution, leveraging data immutability to ensure consistency.
*   **Optimized Short-Lived Object Cleanup:** An asynchronous analysis pool efficiently reclaims memory from short-lived objects, acting as a generational collection mechanism.

## Building the Project

The project uses `make` for its build system.

### Prerequisites

*   `g++` (GCC C++ Compiler)
*   `make`

### Compilation Instructions

1.  **Clone the repository:**
    ```bash
    git clone <repository_url>
    cd proto
    ```
2.  **Build the release version of the library:**
    ```bash
    make all
    ```
    This will compile the `proto` library and create `lib/libproto.a`.

3.  **Build the debug version of the library and the test executable:**
    ```bash
    make debug
    ```
    This will compile the debug version of the library (`lib/libproto-debug.a`) and the `test_proto` executable located in `bin/`.

4.  **Run the tests:**
    ```bash
    make test
    ```
    This will execute the `bin/test_proto` program.

5.  **Clean the build artifacts:**
    ```bash
    make clean
    ```
    This will remove all compiled objects, debug files, and generated libraries.
