# Proto: A High-Performance, Embeddable Dynamic Object System for C++

[![Language](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://isocpp.org/)
[![Build System](https://img.shields.io/badge/Build-CMake-green.svg)](https://cmake.org/)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Proto is a powerful, embeddable runtime written in modern C++ that brings the flexibility of dynamic, protoCoretype-based object systems (like those in JavaScript or Python) into the world of high-performance, compiled applications.**

It is designed for developers who need to script complex application behavior, configure systems dynamically, or build domain-specific languages without sacrificing the speed and control of C++. With Proto, you get an elegant API, automatic memory management, and a robust, immutable data model designed for elite concurrency.

---

## Quick Start: A "Hello, World" Example

See how easy it is to get started. This minimal example creates a list, appends some values, and prints the result.

```cpp
#include <iostream>
#include "headers/protoCore.h"

int main() {
    // 1. Create the runtime space.
    proto::ProtoSpace space;
    proto::ProtoContext* context = space.rootContext;

    // 2. Create and manipulate objects.
    // The API is const-correct and immutable by default.
    const proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromUTF8String("Hello"));
    list = list->appendLast(context, context->fromInteger(42));

    // 3. Use the objects.
    std::cout << "List size: " << list->getSize(context) << std::endl;
    // Expected output: List size: 2
    
    return 0;
}
```

## Core Features

*   **Dynamic Typing in C++**: Create and manipulate integers, floats, booleans, strings, and complex objects without compile-time type constraints.
*   **Prototypal Inheritance**: A flexible and powerful object model based on Lieberman prototypes. Objects inherit directly from other objects, allowing for dynamic structure and behavior sharing.
*   **Immutable-by-Default Data Structures**: Collections like lists, tuples, and dictionaries are immutable. Operations like `append` or `set` return new, modified versions, eliminating a whole class of bugs related to shared state and making concurrent programming safer and easier to reason about.
*   **True, GIL-Free Concurrency**: Each `ProtoThread` is a native OS thread. The runtime was designed from the ground up for parallelism and has no Global Interpreter Lock, allowing it to take full advantage of modern multi-core processors.
*   **Low-Latency Automatic Memory Management**: A concurrent, stop-the-world garbage collector manages the lifecycle of all objects, freeing you from manual `new` and `delete` with minimal application pauses.
*   **Clean, `const`-Correct C++ API**: The entire system is exposed through a clear and minimal public API (`protoCore.h`) that has been refactored for `const`-correctness, improving safety and expressiveness.

## Architectural Highlights

Proto's performance and safety stem from a set of deeply integrated architectural decisions:

1.  **Immutable-First Object Model**: At its core, Proto is built around immutable data structures. Operations that "modify" collections like tuples or strings actually produce new versions, efficiently sharing the unchanged parts of the original (structural sharing). This design is the foundation of Proto's concurrency story, making parallel programming fundamentally safer.

2.  **Hardware-Aware Memory Model**: Proto's memory architecture is meticulously designed to leverage the features of modern multi-core CPUs, resulting in elite performance:
    *   **Tagged Pointers**: Simple values like integers and booleans are stored directly inside the 64-bit pointer. This provides extreme cache locality and avoids heap allocation entirely for primitive types, dramatically reducing GC pressure.
    *   **Eliminating False Sharing**: All heap objects reside in 64-byte `Cell`s, perfectly aligning with the 64-byte cache lines of modern CPUs. This ensures that when different cores access different objects, they are guaranteed not to contend for the same cache line—a common and severe performance bottleneck in multithreaded applications.
    *   **Concurrent Garbage Collector**: A dedicated GC thread works in parallel with the application, with extremely short "stop-the-world" pauses, making Proto suitable for interactive and soft real-time applications.

---

## Building the Project

The project uses **CMake** for a modern, cross-platform build system.

### Prerequisites

*   **CMake** (version 3.16 or higher)
*   A modern C++ compiler that supports C++20 (e.g., **GCC 10+**, **Clang 12+**)

### Compilation

1.  **Clone the repository:**
    ```bash
    git clone <repository_url>
    cd protoCoreCore
    ```

2.  **Configure and Build:**
    This standard "out-of-source" build keeps your source directory clean.
    ```bash
    mkdir build
    cd build
    cmake ..
    make
    ```

This will produce the `libprotoCore.a` static library, along with all test and benchmark executables inside the `build` directory.

## Running Tests and Benchmarks

After a successful build, all executables are located in the `build` directory.

### Running the Test Suite

The project uses the **Google Test** framework. To run all tests:
```bash
cd build
ctest --verbose
```

### Running the Benchmarks

The benchmarks provide insight into Proto's performance characteristics.
```bash
./build/performance/immutable_sharing_benchmark
./build/performance/concurrent_append_benchmark
```

## Building the Documentation

The project documentation is generated using **Doxygen**, **Sphinx**, and **Breathe**.

1.  **Navigate to the docs directory:**
    ```bash
    cd docs
    ```

2.  **Build the HTML documentation:**
    A simple `Makefile` handles the entire process.
    ```bash
    make html
    ```

3.  **View the results:**
    Open `docs/_build/html/index.html` in your web browser.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

Copyright (c) 2024

## Contributing

We welcome contributions! This project is at a perfect stage for developers interested in compilers, memory management, and high-performance C++. Please check out `DESIGN.md` for architectural details and `next_steps.md` for our roadmap. Feel free to open an issue on GitHub to discuss your ideas.
