# Proto: A High-Performance, Embeddable Dynamic Object System for C++

[![Language](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://isocpp.org/)
[![Build System](https://img.shields.io/badge/Build-CMake-green.svg)](https://cmake.org/)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

**Proto is a powerful, embeddable runtime written in modern C++ that brings the flexibility of dynamic, prototype-based object systems (like those in JavaScript or Python) into the world of high-performance, compiled applications.**

It is designed for developers who need to script complex application behavior, configure systems dynamically, or build domain-specific languages without sacrificing the speed and control of C++. With Proto, you get an elegant API, automatic memory management, and a robust, immutable data model designed for elite concurrency.

---

## Project Status: Alpha & Actively Developed

Proto is a feature-complete but experimental runtime. The core architecture is stable and well-tested, making it an ideal moment for new contributors to get involved in performance optimization, API refinement, and the development of the broader ecosystem.

## Core Features

*   **Dynamic Typing in C++**: Create and manipulate integers, floats, booleans, strings, and complex objects without compile-time type constraints.
*   **Prototypal Inheritance**: A flexible and powerful object model based on Lieberman prototypes. Objects inherit directly from other objects, allowing for dynamic structure and behavior sharing.
*   **Immutable-by-Default Data Structures**: Collections like lists, tuples, and dictionaries are immutable. Operations like `append` or `set` return new, modified versions, eliminating a whole class of bugs related to shared state and making concurrent programming safer and easier to reason about.
*   **Elite Concurrency Model**: By leveraging immutability and a GIL-free architecture, Proto provides a foundation for true multi-core scalability.
*   **Low-Latency Automatic Memory Management**: A concurrent, stop-the-world garbage collector manages the lifecycle of all objects, freeing you from manual `new` and `delete` with minimal application pauses.
*   **Clean, `const`-Correct C++ API**: The entire system is exposed through a clear and minimal public API (`proto.h`) that has been refactored for `const`-correctness, improving safety and expressiveness.

## Architectural Highlights

Proto's performance and safety stem from a set of deeply integrated architectural decisions:

1.  **Immutable-First Object Model**: At its core, Proto is built around immutable data structures. Operations that "modify" collections like tuples or strings actually produce new versions, efficiently sharing the unchanged parts of the original (structural sharing). This design is the foundation of Proto's concurrency story, making parallel programming fundamentally safer.

2.  **Hardware-Aware Memory Model**: Proto's memory architecture is meticulously designed to leverage the features of modern multi-core CPUs, resulting in elite performance:
    *   **Tagged Pointers**: Simple values like integers and booleans are stored directly inside the 64-bit pointer. This provides extreme cache locality and avoids heap allocation entirely for primitive types, dramatically reducing GC pressure.
    *   **Cache-Line-Aligned Cells**: All heap objects reside in 64-byte `Cell`s, perfectly aligning with the 64-byte cache lines of modern CPUs. This ensures that an entire object is fetched in a single memory operation and, crucially, **eliminates false sharing**. When different cores access different objects, they are guaranteed not to contend for the same cache line, a common and severe performance bottleneck in multithreaded applications.
    *   **Concurrent Garbage Collector**: A dedicated GC thread works in parallel with the application, with extremely short "stop-the-world" pauses, making Proto suitable for interactive and soft real-time applications.

3.  **True, GIL-Free Concurrency**: Each `ProtoThread` is a native OS thread. The runtime was designed from the ground up for parallelism and has no Global Interpreter Lock, allowing it to take full advantage of modern multi-core processors.

---

## Getting Started & Building the Project

The project now uses **CMake** for a modern, cross-platform build system. This allows you to build Proto easily on Linux, macOS, and Windows with a variety of compilers and IDEs like CLion or Visual Studio Code.

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

## Testing

The project includes a comprehensive test suite using the **Google Test** framework, which provides a safety net against regressions and validates the core logic. The tests are automatically configured by CMake.

### Running the Tests

After compiling the project, you can run the entire test suite from the `build` directory:

```bash
ctest --verbose
```

This command will discover and run all tests, providing detailed output.

### Test Coverage

The test suite provides coverage for the most critical components of the runtime, including:

*   **Primitives:** Correct handling of integers, booleans, and strings (validating the tagged pointer system).
*   **Objects:** Creation, attribute access, and prototype-based inheritance.
*   **Lists (`ProtoList`):** Appending, accessing, removing, and slicing.
*   **Sparse Lists (`ProtoSparseList`):** Use with both integer keys and string hashes (for dictionaries).
*   **Tuples (`ProtoTuple`):** Creation, access, slicing, and validation of the interning mechanism.

## Contributing

We welcome contributions! This project is at a perfect stage for developers interested in compilers, memory management, and high-performance C++. Please check out the `next_steps.md` file for our roadmap and feel free to open an issue on GitHub to discuss your ideas.

## Benchmarks

The compiled benchmark executables are located in the `build` directory. You can run them individually to see Proto's performance characteristics in action:

```bash
./performance/immutable_sharing_benchmark
./performance/concurrent_append_benchmark
./performance/string_concat_benchmark
```
