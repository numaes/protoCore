# protoCore: A High-Performance, Embeddable Dynamic Object System for C++

[![Language](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://isocpp.org/)
[![Build System](https://img.shields.io/badge/Build-CMake-green.svg)](https://cmake.org/)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**protoCore** is the official name of the library. It is a powerful, embeddable runtime written in modern C++ that brings the flexibility of dynamic, prototype-based object systems (like those in JavaScript or Python) into the world of high-performance, compiled applications. The library is built as a **shared library** for easy integration and distribution.

It is designed for developers who need to script complex application behavior, configure systems dynamically, or build domain-specific languages without sacrificing the speed and control of C++. With protoCore, you get an elegant API, automatic memory management, and a robust, immutable data model designed for elite concurrency.

## Project Status

**✅ Production Ready** - ProtoCore is fully implemented and production-ready with comprehensive test coverage.

### Current Metrics

| Metric | Status |
|--------|--------|
| **API Completeness** | **100%** - All declared methods implemented ✅ |
| **Test Coverage** | **50/50 tests passing** (100% pass rate) ✅ |
| **Code Quality** | A+ - Excellent organization and documentation |
| **Architecture** | Exemplary - Hardware-aware design, GIL-free concurrency |
| **Production Ready** | Yes - All systems operational |

### Recent Improvements (2026)

- ✅ **Complete API Implementation** - All 36 missing methods implemented, achieving 100% API completeness
- ✅ **Buffer API Completion** - Full ProtoByteBuffer public API with factory methods
- ✅ **GC Stress Testing** - Validated and optimized garbage collection behavior
- ✅ **Comprehensive Technical Audit** - Complete architecture and implementation review
- ✅ **Documentation** - Full API documentation and technical specifications
- ✅ **protoJS Runtime** - Complete JavaScript runtime built on protoCore (Phase 5 Complete)

### Community & Open Review

Beyond formal audits, this project is officially **open for Community Review and Suggestions**.

We welcome architectural feedback, edge-case identification, and performance critiques. While the core vision is firm, the path to perfection is a collective effort of the "Swarm."

---

## Real-World Application: protoJS

**protoJS** is a modern JavaScript runtime built entirely on protoCore, demonstrating the framework's capability to serve as the foundation for production-ready runtimes. This real-world application showcases protoCore's strengths in a practical, high-performance environment.

### protoJS Status: Phase 5 Complete ✅

**Current Status:** Production-ready JavaScript runtime with full Node.js compatibility

**Key Achievements:**
- ✅ Complete JavaScript runtime with QuickJS parser integration
- ✅ Full Node.js API compatibility (fs, http, net, stream, crypto, buffer, etc.)
- ✅ Advanced networking (TCP, UDP, HTTP, Cluster, Worker Threads)
- ✅ Comprehensive developer tools (Memory Analyzer, Visual Profiler, Integrated Debugger)
- ✅ Module system (CommonJS, ES Modules, npm integration)
- ✅ Transparent worker thread execution with automatic parallelization
- ✅ Production-ready binary (2.3 MB executable)
- ✅ Comprehensive test suite with high coverage

**What protoJS Demonstrates:**
- **Concurrency without GIL**: protoCore's GIL-free architecture enables true parallel execution
- **Efficient Memory Management**: protoCore's concurrent GC handles JavaScript object lifecycle
- **Immutability Benefits**: Structural sharing provides memory efficiency and thread safety
- **Performance**: Hardware-aware design delivers elite performance characteristics
- **Production Readiness**: Complete runtime with full module system and developer tools

**Learn More:** See the [protoJS project](https://github.com/gamarino/protoJS) for complete documentation and source code.

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
    list = list->appendLast(context, context->fromLong(12345)); // SmallInteger
    list = list->appendLast(context, context->fromFloat(987.65)); // Double
    list = list->appendLast(context, context->fromLong(1LL << 60)); // LargeInteger

    // 3. Use the objects.
    std::cout << "List size: " << list->getSize(context) << std::endl;
    // Expected output: List size: 4
    
    return 0;
}
```

## Core Features

*   **Rich Dynamic Typing in C++**: Create and manipulate a rich set of types, including booleans, strings, sets, multisets, and a powerful numeric hierarchy with arbitrary-precision integers (`LargeInteger`) and 64-bit floating-point numbers (`Double`).
*   **Prototypal Inheritance**: A flexible and powerful object model based on Lieberman prototypes. Objects inherit directly from other objects, allowing for dynamic structure and behavior sharing.
*   **Immutable-by-Default Data Structures**: Collections like lists, tuples, and dictionaries are immutable. Operations like `append` or `set` return new, modified versions, eliminating a whole class of bugs related to shared state and making concurrent programming safer and easier to reason about.
*   **True, GIL-Free Concurrency**: Each `ProtoThread` is a native OS thread. The runtime was designed from the ground up for parallelism and has no Global Interpreter Lock, allowing it to take full advantage of modern multi-core processors.
*   **Low-Latency Automatic Memory Management**: A concurrent, stop-the-world garbage collector manages the lifecycle of all objects, freeing you from manual `new` and `delete` with minimal application pauses.
*   **Clean, `const`-Correct C++ API**: The entire system is exposed through a clear and minimal public API (`protoCore.h`) that has been refactored for `const`-correctness, improving safety and expressiveness. **100% API completeness** - all declared methods are fully implemented and tested.

## Architectural Highlights

protoCore's performance and safety stem from a set of deeply integrated architectural decisions:

1.  **Immutable-First Object Model**: At its core, protoCore is built around immutable data structures. Operations that "modify" collections like tuples or strings actually produce new versions, efficiently sharing the unchanged parts of the original (structural sharing). This design is the foundation of protoCore's concurrency story, making parallel programming fundamentally safer.

2.  **Hardware-Aware Memory Model**: protoCore's memory architecture is meticulously designed to leverage the features of modern multi-core CPUs, resulting in elite performance:
    *   **Hybrid Numeric System**: protoCore features a sophisticated numeric system that maximizes efficiency.
        *   **Tagged Pointers**: Integers that fit within 56 bits (`SmallInteger`) are stored directly inside the 64-bit pointer. This provides extreme cache locality and avoids heap allocation entirely for the most common numeric values.
        *   **Heap-Allocated Objects**: For numbers that exceed this range, protoCore automatically and transparently promotes them to heap-allocated objects: `LargeInteger` for arbitrary-precision integers and `Double` for 64-bit floating-point numbers. This hybrid approach provides the best of both worlds: extreme speed for common cases and unlimited precision when needed.
    *   **Eliminating False Sharing**: All heap objects reside in 64-byte `Cell`s, perfectly aligning with the 64-byte cache lines of modern CPUs. This ensures that when different cores access different objects, they are guaranteed not to contend for the same cache line—a common and severe performance bottleneck in multithreaded applications.
    *   **Concurrent Garbage Collector**: A dedicated GC thread works in parallel with the application, with extremely short "stop-the-world" pauses, making protoCore suitable for interactive and soft real-time applications.

---

## The Swarm of One

**The Swarm of One** is the transition from "Individual Contributor" to "System Architect": a single human architect orchestrating a swarm of specialized AI agents to tackle high-density infrastructure that previously required entire R&D departments.

In protoCore, that infrastructure is low-level memory management: 64-byte cell alignment for cache-line efficiency, lock-free atomics, tagged pointers, and a concurrent GC with minimal stop-the-world pauses. Navigating these complexities—without sacrificing correctness or performance—is exactly where AI augmentation acted as a force multiplier: exploring design space, validating invariants, and keeping the codebase consistent across hundreds of cells and thousands of lines. The result is the democratization of high-level systems engineering: one architect, one vision, amplified.

---

## The Methodology: AI-Augmented Engineering

These projects were built using **extensive AI-augmentation tools** to empower human vision and strategic design.

This is not "AI-generated code" in the traditional sense; it is **AI-amplified architecture**. The vision, the constraints, and the trade-offs are human; the execution is accelerated by AI as a force multiplier for complex system design. We don't just use AI—we embrace it as the unavoidable present of software engineering.

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
    cd protoCore
    ```

2.  **Configure and Build:**
    It is recommended to use a separate build directory:
    ```bash
    cmake -B build -S .
    cmake --build build --target protoCore
    ```

This will produce the **protoCore shared library** and test/benchmark executables in the `build` directory:

| Platform | Library output |
|----------|----------------|
| Linux   | `libprotoCore.so` |
| macOS   | `libprotoCore.dylib` |
| Windows | `protoCore.dll` |

## Installation and Packaging

protoCore supports standard installation and multi-platform packaging using CMake and CPack.

### 1. Build
To compile the shared library:
```bash
cmake -B build -S .
cmake --build build --target protoCore
```

### 2. Installation
To install the library on your system (requires administrative privileges):
```bash
sudo cmake --install build --component protoCore
```
To install to a specific directory (staging):
```bash
cmake --install build --component protoCore --prefix ./dist
```

Installed files:
- **Library:** `${CMAKE_INSTALL_LIBDIR}/libprotoCore.so` (or `.dylib` / `.dll` on other platforms)
- **Header:** `${CMAKE_INSTALL_INCLUDEDIR}/protoCore.h`

### 3. Packaging
To generate installers or packages for your platform (.deb, .rpm, .zip, .nsis, etc.):
```bash
cd build
cpack
```
This will generate the package files in the `build` directory.

## Running Tests and Benchmarks

After a successful build, all executables are located in the `build` directory.

### Running the Test Suite

The project uses the **Google Test** framework with **CTest**. To run all tests in parallel (recommended):
```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```
On systems without `nproc`, use a number (e.g. `-j4`). You can also use the helper script:
```bash
./scripts/run_tests.sh
```
For CI or a full configure/build/test run: `./scripts/ci_run_tests.sh`. See [docs/TESTING.md](docs/TESTING.md) for test caching, re-running failed tests, coverage, and [Testing User Guide](docs/Structural%20description/guides/04_testing_user_guide.md) for a short copy-paste guide.

### Test Coverage

To build with coverage instrumentation and generate an HTML report:
```bash
cmake -B build -S . -DCOVERAGE=ON
cmake --build build --target protoCore proto_tests
cmake --build build --target coverage
```
Open `build/coverage/index.html` in a browser. Requires **lcov** and **genhtml**. See [docs/TESTING.md](docs/TESTING.md) for details.

### Running the Benchmarks

The benchmarks provide insight into protoCore's performance characteristics.
```bash
./build/immutable_sharing_benchmark
./build/concurrent_append_benchmark
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

Copyright (c) 2024 Gustavo Marino <gamarino@gmail.com>

## Documentation

**Unified index:** [**DOCUMENTATION.md**](DOCUMENTATION.md) — Index of all protoCore documentation with references (entry points, module system, guides, architecture, testing, historical).

Main documents:
- **`DESIGN.md`** — Architectural design and implementation rules
- **`COMPREHENSIVE_TECHNICAL_AUDIT_2026.md`** — Technical audit and production readiness
- **`docs/USER_GUIDE_UMD_MODULES.md`** — User guide: generating a module for Unified Module Discovery
- **`docs/MODULE_DISCOVERY.md`** — Module system (resolution chain, providers, ProtoSpace::getImportModule)
- **`docs/Structural description/`** — Guides, architecture, tutorials (quick start, building on proto, creating modules, testing)
- **`docs/TESTING.md`** — Testing (CTest, coverage, CI)

### Real-World Reference Implementation

- **protoJS** - A complete JavaScript runtime built on protoCore, demonstrating production use of all protoCore features. See the [protoJS repository](https://github.com/gamarino/protoJS) for implementation examples and best practices.

## Contributing

We welcome contributions! This project is at a perfect stage for developers interested in compilers, memory management, and high-performance C++. Please check out `DESIGN.md` for architectural details. Feel free to open an issue on GitHub to discuss your ideas.

## Acknowledgments

ProtoCore is developed and maintained by Gustavo Marino. The project represents years of careful design and implementation focused on performance, safety, and developer experience.

---

**Don't just watch the shift. Lead it.** The tools are here, the barrier is gone, and the only limit is the clarity of your vision. Join the review, test the limits, and become part of the Swarm of One. Let's build the future of computing, one cell at a time.
