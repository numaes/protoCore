# protoCore: An Embeddable Dynamic Object System for C++

[![Language](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://isocpp.org/)
[![Build System](https://img.shields.io/badge/Build-CMake-green.svg)](https://cmake.org/)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**protoCore** is a runtime library written in C++20 that brings prototype-based dynamic objects, of the kind found in JavaScript or Python, to compiled applications. It is built as a shared library and is the object model and runtime kernel of the projects listed under [The protoCore Ecosystem](#the-protocore-ecosystem).

> [!WARNING]
> protoCore is **not production ready**. It is open for community review: architectural feedback, edge-case reports and performance critiques are welcome.

protoCore is intended for developers who embed a scripting layer in a C++ application, configure systems dynamically, or build language runtimes and domain-specific languages. It provides automatic memory management, immutable collections with structural sharing, and native threads without a global interpreter lock.

## Project Status

| Item | Value |
|------|-------|
| Version | 1.2.0 |
| Status | Open for review; not production ready |
| Test suite | 235 CTest cases (GoogleTest), counted with `ctest -N` on 2026-09-15 |
| Change history | [CHANGELOG.md](CHANGELOG.md) |

### Recent kernel work (2026)

- **Concurrent mark** *(May 2026)*: mark, sweep and bulk unmark run outside the stop-the-world window. During the stop-the-world phase the collector copies the 256 mutable-shard roots into a per-cycle snapshot; the marker then traverses that snapshot while application threads keep running. See [docs/GarbageCollector.md](docs/GarbageCollector.md).
- **Inline small sparse lists** *(May 2026)*: `ProtoSparseListSmallImplementation` stores up to three (key, value) pairs in a single 64-byte cell; larger sparse lists use the AVL form. The public `ProtoSparseList` API is unchanged.
- **GC survivor re-chain and per-context submission** *(May 2026, on by default)*: cells that survive a sweep are examined again in later cycles, and a context hands its young cells to the collector once its allocation count crosses `ProtoSpace::maxAllocatedCellsPerContext` (default 10,000 cells, overridable with the `PROTOCORE_GC_CONTEXT_THRESHOLD` environment variable). The CMake option `PROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` disables it. Design: [2026-05-03-gc-survivor-rechain.md](docs/archive/design-specs/2026-05-03-gc-survivor-rechain.md).
- **`ProtoContext::CriticalSection`**: an RAII guard that defers stop-the-world parking while a thread holds newly allocated cells that are not yet attached to a GC root.
- **Collection paced by allocation** *(September 2026)*: without a heap limit, a GC cycle starts once the cells consumed since the previous cycle (a thread's refill batch is charged when it is exhausted or the thread exits) reach `max(PROTOCORE_GC_MIN_BUDGET_CELLS, retained cells × PROTOCORE_GC_GROWTH_PERCENT / 100)` and contexts have submitted garbage since then. The defaults are 1,048,576 cells (64 MiB) and 100%, and `PROTOCORE_GC_GROWTH_PERCENT=0` turns the trigger off. Before this change, the heap of a program with no limit grew without bound unless the embedder called `triggerGC()`. Long-running loops must call `ProtoContext::safepoint()` so that a requested cycle can stop the world. See [docs/GarbageCollector.md](docs/GarbageCollector.md) § "Memory Allocation".
- **Heap allocation limit** *(May 2026)*: `ProtoSpace::setHeapLimits(softCells, hardCells)` bounds the heap and enables out-of-memory detection based on reclamation. Design: [2026-05-22-allocation-limit-oom-design.md](docs/archive/design-specs/2026-05-22-allocation-limit-oom-design.md).
- **`getAttribute` returns `PROTO_NONE` for missing attributes** *(May 2026)*; `nullptr` signals invalid input. An attribute can also hold `PROTO_NONE`, so use `hasAttribute` or `hasOwnAttribute` to test for presence.
- **Public inline SmallInt helpers** *(April 2026)*: `proto::isSmallInt`, `proto::asSmallInt`, `proto::smallIntInRange` and `proto::makeSmallInt` in `protoCore.h` let embedders handle SmallInt arithmetic without calling into the shared library.
- **Cooperative safepoint** *(April 2026)*: `ProtoContext::safepoint()` lets CPU-bound loops that do not allocate take part in the stop-the-world handshake.
- **Mutable hot path** *(April 2026)*: `mutableRoot` has 256 cache-line-padded shards, and each thread has a 1024-entry cache of current mutable values. Design: [docs/MUTABLE_SHARDING_AND_CACHE_REFACTOR.md](docs/MUTABLE_SHARDING_AND_CACHE_REFACTOR.md).
- **Three-tier strings** *(April 2026)*: strings of up to 6 UTF-8 bytes are embedded in the pointer, identifiers are interned symbols in a 64-shard `SymbolTable`, and other strings are heap strings backed by a persistent AVL tree. Design: [2026-03-31-string-refactoring-design.md](docs/archive/design-specs/2026-03-31-string-refactoring-design.md).

---

## The protoCore Ecosystem

Four language runtimes (protoJS, protoPython, protoST, protoClojure) and protoCpp's C++ examples are built on protoCore.

| Project | Role | Repository |
|---|---|---|
| protoCore | C++20 object model and runtime kernel: immutable structures, concurrent GC, GIL-free threads | https://github.com/numaes/protoCore |
| protoJS | JavaScript runtime on protoCore | https://github.com/gamarino/protoJS |
| protoPython | Python 3 runtime (protopy) and ahead-of-time compiler (protopyc) on protoCore | https://github.com/gamarino/protoPython |
| protoST | Smalltalk-inspired actor language on protoCore | https://github.com/gamarino/protoST |
| protoClojure | Clojure dialect on protoCore (early stage) | https://github.com/gamarino/protoClojure |
| protoCpp | Examples and benchmarks using protoCore directly from C++ | https://github.com/gamarino/protoCpp |

---

## Quick Start: A "Hello, World" Example

This minimal example creates a list, appends some values, and prints the list size.

```cpp
#include <iostream>
#include "protoCore.h"

int main() {
    // 1. Create the runtime space. Its root context serves the main thread.
    proto::ProtoSpace space;
    proto::ProtoContext* context = space.rootContext;

    // 2. Create objects. Collections are immutable: appendLast returns a new list.
    const proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromUTF8String("Hello"));
    list = list->appendLast(context, context->fromLong(12345));      // SmallInt
    list = list->appendLast(context, context->fromDouble(987.65));   // Double
    list = list->appendLast(context, context->fromLong(1LL << 60));  // LargeInteger

    // 3. Use the objects.
    std::cout << "List size: " << list->getSize(context) << std::endl;
    // Expected output: List size: 4

    return 0;
}
```

When protoCore is part of your CMake build (for example through `add_subdirectory`), link the program with `target_link_libraries(hello PRIVATE protoCore)`; the `protoCore` target adds `headers/` to the include path. For an installed library, see [docs/INSTALLATION.md](docs/INSTALLATION.md).

## Core Features

*   **Dynamic typing in C++**: booleans, strings, lists, tuples, sparse lists, sets, multisets, arbitrary-precision integers (`LargeInteger`) and 64-bit floating-point numbers (`Double`).
*   **Prototype-based inheritance**: an object model based on Lieberman prototypes. Objects inherit directly from one or more parent objects (`newChild`, `addParent`, `setParents`).
*   **Immutable collections**: lists, tuples, strings and sparse lists are immutable. Operations such as `appendLast` or `setAt` return new versions that share unchanged parts with the original.
*   **Concurrency without a global lock**: each `ProtoThread` is a native OS thread, and the runtime has no global interpreter lock. Mutable objects are updated with compare-and-swap on a sharded table.
*   **Automatic memory management**: a concurrent mark-and-sweep garbage collector. A short stop-the-world phase covers only the thread handshake and root capture; marking and sweeping run concurrently with application threads.
*   **Two GC-bridge mechanisms for embedders**: runtimes built on protoCore sometimes need to keep a `ProtoObject*` alive where the GC cannot see it, for example an asynchronous callback receiver captured in a C++ lambda, or a language symbol that must outlive every context. **Perpetual objects** (attribute names, keywords, type prototypes) are allocated with a null `ProtoContext*`: the cell comes from `posix_memalign`, is never enrolled in GC bookkeeping, and lives for the lifetime of the process. **Transient pins** use a `ProtoRootSet` created once with `ProtoSpace::createRootSet`, whose `add` and `remove` calls are O(1) and return generational handles. See [DESIGN.md](DESIGN.md) § "Keeping ProtoObjects alive across allocation boundaries the GC cannot see".
*   **`const`-correct C++ API**: the public API is declared in `headers/protoCore.h`.

## Architectural Highlights

1.  **Immutable-first object model**: operations that "modify" collections produce new versions that share the unchanged parts of the original (structural sharing). Immutability is what lets the collector mark a stable graph while threads run.

2.  **Hardware-aware memory model**:
    *   **Tagged pointers**: small integers (signed 54-bit `SmallInt`), booleans, Unicode characters and inline strings of up to 6 UTF-8 bytes are stored directly in the 64-bit `ProtoObject*` handle, without heap allocation.
    *   **Transparent promotion**: values outside these ranges are promoted to heap objects (`LargeInteger`, `Double`, or strings backed by a persistent AVL tree).
    *   **64-byte cells**: heap objects live in 64-byte `Cell`s allocated with 64-byte alignment, the cache-line size of common CPUs, so each cell occupies exactly one cache line.
    *   **Per-thread caches**: each thread has a 1024-entry attribute cache, read without locks, and a 1024-entry mutable value cache whose hits are validated with a single atomic load of the shard root.

3.  **Concurrent garbage collector with a snapshot of mutable state**: a dedicated GC thread runs mark, sweep and bulk unmark concurrently with application threads. protoCore routes all mutable state through `MUTABLE_ROOT_SHARDS = 256` shards, so the stop-the-world phase captures the roots and a 256-pointer snapshot of the shard table, and no write barriers are needed. [docs/GarbageCollector.md](docs/GarbageCollector.md) breaks the pause into its components and estimates 30–250 μs for typical workloads; these are per-component estimates, not measured percentiles.

## Performance

This repository does not publish benchmark results. The `performance/` directory contains microbenchmarks (attribute access, cache timing, lists, sparse lists, string concatenation, structural sharing, concurrent appends) that are built with the project; see [Running the benchmarks](#running-the-benchmarks).

The protoCpp repository publishes a dated comparison (2026-06-15) that drives protoCore directly from C++. In whole-process wall time, protoCpp finishes before CPython on the five benchmarks with a same-size CPython measurement (1.04×–2.39× single-threaded, 14.33× on `multithread_cpu` with four threads). On three of those rows CPython's time is dominated by interpreter start-up, and the C++ and Python columns come from different harnesses and runs, so the figures do not isolate workload speed; see https://github.com/gamarino/protoCpp/blob/main/RESULTS.md for the full caveats. In the same measurement the inline SmallInt helpers cut protoCpp wall time by 58.9% on `call_recursion`. The CPython version and build used for that comparison are not recorded.

---

## The Swarm of One

protoCore is designed and maintained by a single architect, Gustavo Marino, working with AI coding agents that draft code, tests and documentation under human review. As of 2026-09-15 the kernel's `core/` and `headers/` directories hold 14,541 lines of C++, and five projects are built on it (see [The protoCore Ecosystem](#the-protocore-ecosystem)).

---

## Building the Project

### Prerequisites

*   **CMake** 3.16 or later
*   A C++ compiler with C++20 support (the build passes GCC/Clang options such as `-fno-delete-null-pointer-checks`)
*   Network access during the first configuration: the test suite downloads GoogleTest 1.14.0 with CMake `FetchContent`

### Compilation

```bash
git clone https://github.com/numaes/protoCore.git
cd protoCore
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

`cmake --build build` builds the shared library, the test executable (`build/test/proto_tests`) and the benchmark executables. To build only the library, add `--target protoCore`.

On Linux the library is `build/libprotoCore.so.1.2.0`, with the links `libprotoCore.so.1` (soname) and `libprotoCore.so`. On macOS CMake names it `libprotoCore.1.2.0.dylib`. Platform support for macOS and Windows is described in [docs/INSTALLATION.md](docs/INSTALLATION.md).

## Running Tests and Benchmarks

### Running the Test Suite

The project uses **GoogleTest** with **CTest**. Build everything (or at least the `protoCore` and `proto_tests` targets), then run all tests in parallel:

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```

On systems without `nproc`, use a number (for example `-j4`). You can also use the helper script:

```bash
./scripts/run_tests.sh
```

For a full configure, build and test run, use `./scripts/ci_run_tests.sh`. See [docs/TESTING.md](docs/TESTING.md) for re-running failed tests, filters and coverage, and the [Testing User Guide](docs/Structural%20description/guides/04_testing_user_guide.md) for a short copy-paste guide.

### Test Coverage

To build with coverage instrumentation and generate an HTML report:

```bash
cmake -B build -S . -DCOVERAGE=ON
cmake --build build --target protoCore proto_tests
cmake --build build --target coverage
```

Open `build/coverage/index.html` in a browser. Requires **lcov** and **genhtml**. See [docs/TESTING.md](docs/TESTING.md) for details.

### Running the Benchmarks

The benchmark executables are written to the build directory, for example:

```bash
./build/microbenchmark_final
./build/immutable_sharing_benchmark
./build/concurrent_append_benchmark
```

The other benchmark targets are `list_benchmark`, `object_access_benchmark`, `mutable_access_benchmark`, `sparse_list_benchmark`, `string_concat_benchmark`, `cache_timing_benchmark`, `cache_pressure_benchmark` and `hash_quality_benchmark`. Build with `-DCMAKE_BUILD_TYPE=Release` before measuring.

## Installation and Packaging

To install the library and its public header into a prefix:

```bash
cmake --install build --component protoCore --prefix ./dist
```

This installs `lib/libprotoCore.so*` and `include/protoCore.h`. Running `cpack` in the build directory generates packages with the generators configured for the current platform (on Linux: TGZ, plus DEB when `dpkg` is found and RPM when `rpmbuild` is found). See [docs/INSTALLATION.md](docs/INSTALLATION.md) for package names, installed files and platform notes.

## Building the Documentation

The API reference is generated from the source comments with **Doxygen**, using the `Doxyfile` in the repository root:

```bash
doxygen Doxyfile
```

See [docs/README.md](docs/README.md) for the output location and for generating HTML.

## Documentation

**Index:** [DOCUMENTATION.md](DOCUMENTATION.md) lists all protoCore documentation.

Main documents:
- **[DESIGN.md](DESIGN.md)**: architectural design and implementation rules
- **[docs/GarbageCollector.md](docs/GarbageCollector.md)**: garbage collector implementation
- **[docs/USER_GUIDE_UMD_MODULES.md](docs/USER_GUIDE_UMD_MODULES.md)**: generating a module for Unified Module Discovery
- **[docs/MODULE_DISCOVERY.md](docs/MODULE_DISCOVERY.md)**: module system (resolution chain, providers, `ProtoSpace::getImportModule`)
- **[docs/Structural description/](docs/Structural%20description/README.md)**: guides (testing, creating modules) and architecture overviews (garbage collector, mutability model, object model)
- **[docs/TESTING.md](docs/TESTING.md)**: testing (CTest, coverage, CI scripts)
- **[docs/archive/](docs/archive/README.md)**: historical analyses and design specifications (not maintained)

The runtimes in [The protoCore Ecosystem](#the-protocore-ecosystem) are complete examples of embedding protoCore.

## Contributing

Contributions are welcome, particularly from developers interested in memory management, concurrency and language runtimes. Read [DESIGN.md](DESIGN.md) for the architecture and implementation rules, and open an issue on GitHub to discuss a change before sending it.

## License

Copyright (c) 2023-2026 Gustavo Marino. Released under the MIT License; see [LICENSE](LICENSE).

---

Review the design, run the tests, and report what you find. **Think Different, As All We.**
