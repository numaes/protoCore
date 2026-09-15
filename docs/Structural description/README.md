# protoCore: Structural Description

For the index of all protoCore documentation, see [DOCUMENTATION.md](../../DOCUMENTATION.md) in the repository root.

protoCore is a C++20 runtime library that provides a prototype-based object model, immutable collections with structural sharing, a concurrent garbage collector, and native threads without a global interpreter lock. It is the kernel of several language runtimes. It is not production ready.

This directory holds short architecture overviews and guides. The detailed references are [DESIGN.md](../../DESIGN.md) and [GarbageCollector.md](../GarbageCollector.md).

## Architecture Overviews

*   **[Garbage collector](./architecture/01_garbage_collector.md)**: concurrent mark and sweep without write barriers, the stop-the-world phase, critical sections, and external buffers.
*   **[Mutability model](./architecture/02_mutability_model.md)**: separation of identity and state, the sharded `mutableRoot`, compare-and-swap updates, and how the collector captures mutable state.
*   **[Object model](./architecture/03_object_model.md)**: contexts, tagged pointers, prototype-based inheritance, and the per-thread attribute cache.

## Guides

*   **[Installation Guide](../INSTALLATION.md)**: build and install the library.
*   **[Creating Modules](./guides/05_creating_modules.md)**: implement and register a `ModuleProvider` for Unified Module Discovery.
*   **[Testing User Guide](./guides/04_testing_user_guide.md)**: run the tests and generate a coverage report.
*   **[Testing Guide](../TESTING.md)**: the CTest setup, re-running failures, coverage, and CI scripts in detail.

## Repository

*   **[protoCore on GitHub](https://github.com/numaes/protoCore)**
