# protoCore: A High-Performance, Embeddable Dynamic Object System for C++

[![Language](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://isocpp.org/)
[![Build System](https://img.shields.io/badge/Build-CMake-green.svg)](https://cmake.org/)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

> **Complexity is no longer a barrier. Precision is no longer a luxury. Speed is no longer a matter of headcount. Welcome to the era of the Swarm of One.**

**protoCore** is the official name of the library. It is a powerful, embeddable runtime written in modern C++ that brings the flexibility of dynamic, prototype-based object systems (like those in JavaScript or Python) into the world of high-performance, compiled applications. The library is built as a **shared library** for easy integration and distribution.

> [!WARNING]
> This project is officially **open for Community Review and Suggestions**. It is **not production ready**. We welcome architectural feedback, edge-case identification, and performance critiques. 

It is designed for developers who need to script complex application behavior, configure systems dynamically, or build domain-specific languages without sacrificing the speed and control of C++. With protoCore, you get an elegant API, automatic memory management, and a robust, immutable data model designed for elite concurrency.

## Project Status

**⚠️ Open for Review** - ProtoCore is in an advanced development stage and open for community review and suggestions. It is **not production ready**.

### Current Metrics

| Metric | Status |
|--------|--------|
| **API Completeness** | **100%** - All declared methods implemented ✅ |
| **Test Coverage** | **136/136 tests passing** (100% pass rate) ✅ |
| **Code Quality** | A+ - Excellent organization and documentation |
| **Architecture** | Exemplary - Hardware-aware design, GIL-free concurrency |
| **Production Ready** | **No** - Open for Community Review |

### Recent Improvements (2026)

- ✅ **Public Inline SmallInt Helpers** *(April 2026)* — Adds four `static inline` helpers in `protoCore.h` (`proto::isSmallInt`, `proto::asSmallInt`, `proto::smallIntInRange`, `proto::makeSmallInt`) so embedders can short-circuit the SmallInt+SmallInt fast path in their bytecode dispatcher without crossing the protoCore shared-library boundary.  The helpers expose **only the bit-pattern of the SmallInt tag** (signed 54-bit value at bits 10-63, fixed low-10-bit tag); no internal types, no internal symbols, no library ABI change.  protoPython's `OP_BINARY_ADD` / `OP_INPLACE_ADD` / `OP_BINARY_SUBTRACT` / `OP_INPLACE_SUBTRACT` / `OP_COMPARE_OP` handlers use them to skip ~10 cross-DSO function calls plus 6 redundant tag checks (the previous `binaryAdd → unwrapPrimitive → ProtoObject::add → Integer::add` chain) for the >90 % SmallInt+SmallInt case.  End-to-end protoPython benchmark `call_recursion` (fib(25), 242 K calls, every call doing one compare + three integer ops) drops 866 ms → 716 ms (**−17 %**); geomean ratio vs CPython 3.14 drops 5.53× → **5.34×** (best protoPython has hit).
- ✅ **Cooperative GC Safepoint Public API** *(April 2026)* — Adds `ProtoContext::safepoint()` to the public header so embedders (e.g. protoPython's bytecode dispatcher) can participate in stop-the-world from tight allocation-free hot loops.  Without it, a CPU-bound thread that never reaches `allocCell`'s 64-allocation poll starves the GC indefinitely and stalls every other thread waiting for STW.  Fast path is a single relaxed atomic load on `stwFlag`; slow path joins the same `parkedThreads`/`stopTheWorldCV` handshake `allocCell` already uses internally.  Also closes a thread-startup race in `ProtoThreadImplementation`: `runningThreads` is now incremented from `thread_main` (when the OS thread actually starts) rather than from the constructor (before `std::thread` was spawned), eliminating a window where GC waited for a phantom running thread that had no way to park.  protoPython multi-threaded benchmark wall-time dropped from 1217 ms to 165 ms (2.55× CPython, was 18.90×) after wiring this safepoint into the bytecode loop.  Per-thread allocation batches in `getFreeCells` are sized `min(blocksPerAllocation × runningThreads × 4, 65 536)` when more than one thread is running, so with 4 workers + GC each refill yields 65 536 cells; profiles confirm a typical worker calls `getFreeCells` exactly once per benchmark run and the global mutex is well-amortised — the remaining gap to CPython on `multithread_cpu` is per-bytecode interpreter cost in protoPython, not allocator contention in protoCore.
- ✅ **Attribute Cache Optimization: 6-bit Hash Shift & Benchmark Validation** *(April 2026)* — Adjusted the attribute cache hash function from `>> 4` to `>> 6` to account for 64-byte object alignment, recovering 75 % of previously "dead" cache slots and increasing effective capacity to 1024 entries per thread. Verified via `performance/cache_timing_benchmark.cpp` showing a **8.7 ns** base latency for cached lookups. The gap between a cache hit and a full AVL miss is only **~2.8 ns**, confirming the robustness of the pointer-indexed dictionary model. This optimization, combined with the 256-shard mutable root refactor (widening from 16 shards and adding per-thread snapshot caching), shaves another 5-10 % off attribute-bound workloads in protoPython/protoJS. End-to-end benchmarks show a geomean ratio vs CPython 3.14 of **7.30×** — the best performance achieved to date.
- ✅ **Mutable Hot Path: 256 Shards + Per-Thread Snapshot Cache (with negative caching)** *(April 2026)* — `mutableRoot` widens from 16 to 256 cache-line-padded shards (`mutable_ref % 256`), and every thread carries a 1024-entry `MutableValueCacheEntry` table that short-circuits the "atomic load + AVL `implGetAt`" sequence on the common own-thread mutable-read path. The cache stores **negative** results too (commit `75fee285`), so unmutated mutables — common for newly created functions / classes / dicts — pay zero AVL after the first read. Validation by shard-root pointer equality means any successful CAS by any thread invalidates stale entries on the next lookup — no broadcast, no signaling. Cache entries are GC roots, traced via `ProtoThreadExtension::processReferences`. Design: [`docs/MUTABLE_SHARDING_AND_CACHE_REFACTOR.md`](docs/MUTABLE_SHARDING_AND_CACHE_REFACTOR.md). Full bench report: [`protoPython/benchmarks/reports/2026-04-25-mutable-cache.md`](../protoPython/benchmarks/reports/2026-04-25-mutable-cache.md).
- ✅ **Three-Tier AVL String System** *(April 2026)* — Complete rewrite: embedded UTF-8 in tagged pointer (≤6 bytes, zero allocation), interned Symbols via 64-shard `SymbolTable`, and non-interned heap Strings — all sharing a uniform public API. 136/136 tests passing.
- ✅ **Complete API Implementation** - All 36 missing methods implemented, achieving 100% API completeness
- ✅ **Buffer API Completion** - Full ProtoByteBuffer public API with factory methods
- ✅ **GC Stress Testing** - Validated and optimized garbage collection behavior
- ✅ **Comprehensive Technical Audit** - Complete architecture and implementation review
- ✅ **Documentation** - Full API documentation and technical specifications
- ✅ **protoJS Runtime** - Complete JavaScript runtime built on protoCore (Phase 6 Complete)
- ✅ **protoPython Runtime** - GIL-free Python 3.14 environment built on protoCore (Phase 6 Complete)

### Community & Open Review

Beyond formal audits, this project is officially **open for Community Review and Suggestions**.

We welcome architectural feedback, edge-case identification, and performance critiques. While the core vision is firm, the path to perfection is a collective effort of the "Swarm."
---

## The protoCore Ecosystem: protoJS & protoPython

protoCore serves as the robust foundation for several high-performance runtimes, demonstrating its versatility as a universal object system for the AI-augmented engineering era.

### 1. protoJS: Elite JavaScript Performance
**protoJS** is a modern JavaScript runtime built entirely on protoCore. By replacing the standard QuickJS runtime with protoCore's immutability and concurrency primitives, it achieves industry-leading performance.

**Highlights:**
- ✅ **19.83x Faster** than Node.js (34x in specific array operations)
- ✅ **Phase 6 Complete** - Full ecosystem compatibility including npm and OpenSSL
- ✅ **Integrated Developer Tools** - Visual Profiler, Memory Analyzer, and Chrome DevTools Protocol debugger

### 2. protoPython: GIL-Free Python Parallelism
**protoPython** provides a highly parallel Python 3.14 environment. It leverages protoCore's GIL-free architecture to enable true multi-threaded execution, overcoming the primary bottleneck of traditional Python runtimes.

**Highlights:**
- ✅ **GIL-Free Execution** - Native parallelism without global locks
- ✅ **Zero-Copy Interop** - Massive data transfer via UMD and HPy without overhead
- ✅ **Phase 6 Complete** - Advanced collection support and smart object unwrapping

**Learn More:** See the [protoJS project](../protoJS/) and [protoPython project](../protoPython/) for complete documentation.


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
*   **Two GC-Bridge Mechanisms for Embedders**: Runtimes built on protoCore (e.g. protoJS, protoPython) can keep a `ProtoObject*` alive across an allocation boundary the GC cannot see — for example, an async callback receiver captured into a C++ lambda, or a language symbol that must outlive every individual context. **For perpetual objects** (attribute names, language keywords, type prototypes) allocate with `ProtoContext* = nullptr`: the Cell goes straight to `posix_memalign` and is never enrolled in any GC bookkeeping, surviving for the lifetime of the process at zero per-cycle cost. **For transient pinning** (async callbacks bounded in time) call `ProtoSpace::createRootSet` once at startup and use `add` / `remove` on the resulting `ProtoRootSet`: the registry is GC-traced and gives O(1) `add`/`remove` plus generational handles that prevent stale-handle resurrection. See `DESIGN.md` § "Keeping ProtoObjects alive across allocation boundaries the GC cannot see".
*   **Clean, `const`-Correct C++ API**: The entire system is exposed through a clear and minimal public API (`protoCore.h`) that has been refactored for `const`-correctness, improving safety and expressiveness. **100% API completeness** - all declared methods are fully implemented and tested.

## Architectural Highlights

protoCore's performance and safety stem from a set of deeply integrated architectural decisions:

1.  **Immutable-First Object Model**: At its core, protoCore is built around immutable data structures. Operations that "modify" collections like tuples or strings actually produce new versions, efficiently sharing the unchanged parts of the original (structural sharing). This design is the foundation of protoCore's concurrency story, making parallel programming fundamentally safer.

2.  **Hardware-Aware Memory Model**: protoCore's memory architecture is meticulously designed to leverage the features of modern multi-core CPUs, resulting in elite performance:
    *   **Hybrid Data Representation**: protoCore features a sophisticated system that maximizes efficiency by avoiding heap allocation for common values.
        *   **Tagged Pointers**: Small integers that fit within 56 bits (`SmallInteger`), booleans, and **Inline Strings** (up to 6 UTF-8 bytes) are stored directly inside the 64-bit `ProtoObject*` handle. This provides extreme cache locality and zero-allocation performance for the most common data types.
        *   **Transparent Promotion**: For data that exceeds these ranges (large integers, floats, or longer strings), protoCore automatically promotes them to heap-allocated objects (`LargeInteger`, `Double`, or `ProtoStringImplementation` backed by a persistent AVL tree). This hybrid approach offers both raw speed for small values and unlimited scale for complex data.
    *   **Eliminating False Sharing**: All heap objects reside in 64-byte `Cell`s, perfectly aligning with the 64-byte cache lines of modern CPUs. This ensures that when different cores access different objects, they are guaranteed not to contend for the same cache line—a common and severe performance bottleneck in multithreaded applications.
    *   **Concurrent Garbage Collector**: A dedicated GC thread works in parallel with the application, with extremely short "stop-the-world" pauses, making protoCore suitable for interactive and soft real-time applications.

## Performance Validation (2026 Audit)

To validate the theoretical performance of the `protoCore` object model and its per-thread attribute cache, we compared its latency against **Node.js (V8)** using a dedicated attribute access benchmark.

### Benchmark Results

| Scenario | protoCore Latency | Node.js (V8) Baseline | Multiplier |
| :--- | :--- | :--- | :--- |
| **Simple Attribute Access (Cached)** | **8.7 ns** | 1.45 ns | ~6.0x |
| **Inherited Attribute (Cached)** | **10.3 ns** | 1.45 ns | ~7.1x |
| **Cache Miss (Full AVL Search)** | **11.5 ns** | 1.45 ns | ~7.9x |
| **Mutable Object Resolution** | **11.1 ns** | 1.45 ns | ~7.6x |

### Conclusions

*   **Elite Interpreter Performance**: Even without a JIT compiler, `protoCore` achieves a latency of **~9-11 ns** per property access. This is within one order of magnitude of V8's highly optimized Inline Caches.
*   **Robustness to Cache Misses**: The difference between a cache hit and a full AVL tree search is only **~2.8 ns**. This confirms that the AVL-backed dictionary with pointer-indexed symbols is extremely efficient and remains performant even when the working set exceeds the per-thread cache capacity.
*   **Effective Sharding**: The resolution of mutable objects via the 256-shard system adds negligible overhead, validating the lock-free design for high-concurrency workloads.

---

## The Swarm of One

**The Swarm of One** represents the transition from "Individual Contributor" to "System Architect". It is the democratization of high-level engineering: a single human architect orchestrating a swarm of specialized AI agents to tackle high-density infrastructure—like lock-free atomics and 64-byte cell alignment—that previously required entire R&D departments. In protoCore, AI was our force multiplier to explore design spaces and validate invariants across thousands of lines of C++20 code. The vision is human; the execution is amplified.

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

protoCore supports standard installation and multi-platform packaging using **CMake** and **CPack**. Full step-by-step instructions for Linux (.deb, .rpm), macOS (.dmg, .tgz), and Windows (.exe, .zip) are in **[docs/INSTALLATION.md](docs/INSTALLATION.md)**.

### Prerequisites

- **C++20** compiler (GCC 10+, Clang 12+, or MSVC 2019+)
- **CMake** 3.16+

### Build

To compile the shared library:

```bash
cmake -B build -S .
cmake --build build --target protoCore
```

Output: `build/libprotoCore.so` (Linux), `build/libprotoCore.dylib` (macOS), or `build/protoCore.dll` (Windows).

### Install

**System install** (requires appropriate privileges):

```bash
sudo cmake --install build --component protoCore
```

**Staging install** (e.g. for packaging or local use):

```bash
cmake --install build --component protoCore --prefix ./dist
```

Installed files:

- **Library:** `lib/libprotoCore.so` (or `.dylib` / `.dll` on other platforms)
- **Header:** `include/protoCore.h`

### Packages (CPack)

To generate platform packages (e.g. .deb on Debian/Ubuntu, .rpm on Fedora when `rpmbuild` is installed; .dmg on macOS; .exe, .zip on Windows):

```bash
cd build
cpack
```

Only packages for the **current OS** are generated (e.g. on Debian, CPack builds TGZ and DEB only, not RPM), so `cpack` does not fail when tools for other formats are missing. Package files appear in the `build` directory. Install with your system package manager (e.g. `sudo dpkg -i …` or `sudo rpm -i …`) or run the generated installer. See [docs/INSTALLATION.md](docs/INSTALLATION.md) for per-platform details and troubleshooting.

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

**Don't just watch the shift. Lead it.** The tools are here, the barrier is gone, and the only limit is the clarity of your vision. Join the review, test the limits, and become part of the Swarm of One. Let's build the future of computing, one cell at a time. **Think Different, As All We.**
