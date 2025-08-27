# Proto Roadmap & Next Steps

This document outlines the key decisions, critical fixes, and strategic roadmap for the evolution of the Proto project, based on a comprehensive architectural analysis.

## Strategic Vision

Proto is not a general-purpose competitor to runtimes like Python's CPython or V8. Its core strength and market position is as a **high-performance, low-latency C++ runtime foundation**.

*   **Identity**: An engine for next-generation systems.
*   **Target Audience**: Game engine developers, financial tech (Fintech) systems, real-time/embedded applications, and creators of Domain-Specific Languages (DSLs).
*   **Competitive Advantages**:
    1.  **Ultra-Low-Latency GC**: A concurrent garbage collector with minimal stop-the-world pauses, ideal for latency-sensitive applications.
    2.  **Lock-Free Mutability Model**: An innovative system separating identity from state, which simplifies GC root scanning and enables high-performance concurrency.
    3.  **Agnostic Foundation**: It is a library, not a language, allowing any execution strategy (interpreter, JIT/AOT compiler) to be built on top.
    4.  **Simplified FFI**: The GC does not move memory, making integration with existing C/C++ code inherently safer and simpler.

---

## 1. Immediate Priorities: Critical Fixes for Robustness

These fixes are essential to guarantee the runtime's stability and correctness.

### 1.1. (CRITICAL) GC Must Scan the Thread's Method Cache

**Problem**: The `attribute_cache` in `ProtoThreadImplementation` stores raw `ProtoObject*` pointers. If an object goes out of scope on the stack but remains in the cache, the GC will prematurely free it, creating a dangling pointer. This leads to a severe `use-after-free` bug when the memory is reallocated and a cache hit occurs on the stale entry.

**Solution**: The attribute cache **must** be treated as a root source for the garbage collector. The `processReferences` method of the thread must iterate over the cache and report the `object` pointers it contains. `attribute_name` does not need to be scanned, as `ProtoString`s are interned and effectively immortal.

### 1.2. (CRITICAL) Correct Hash Calculation in Method Cache

**Problem**: The hash calculation in `ProtoObject::call` used `reinterpret_cast<int>`, which truncates 64-bit pointers. This leads to excessive hash collisions and severely degrades cache performance.

**Solution**: Use `uintptr_t` for the pointer-to-integer conversion. This is a portable way to ensure the entire address is used for the hash, avoiding data loss.

---

## 2. Foundational Improvements: Codebase Modernization

These changes will improve the project's safety, readability, and maintainability, aligning it with modern C++ best practices.

1.  **RAII for Thread Management**: Replace raw `new std::thread` and `delete` in `ProtoThreadImplementation` with `std::unique_ptr<std::thread>`. This prevents resource leaks and simplifies lifetime management.
2.  **Idiomatic Array Allocation**: Replace `std::malloc`/`std::free` for the `attribute_cache` with `new[]` and `delete[]`.
3.  **Professional Build System**: **Migrate the `Makefile` to CMake.** This is the highest priority modernization task. It is essential for cross-platform compatibility (Linux, macOS, Windows) and for enabling outside contributors to build the project easily.

---

## 3. Strategic Roadmap

### Phase 1: MVP for Initial Showcase

**Goal**: To create a compelling demonstration of Proto's potential.

*   **[ ] Implement Critical Fixes**: Complete the robustness fixes outlined in Section 1.
*   **[ ] `proto_python` Transpiler PoC**: Develop a proof-of-concept transpiler that supports a functional subset of Python (e.g., variable assignment, arithmetic, function def/call, loops). The Python `ast` module should be used as the front-end.
*   **[ ] Interactive REPL**: Create a simple Read-Eval-Print Loop for the `proto_python` transpiler. This is the most effective tool for live demonstrations.
*   **[ ] High-Impact Benchmark**: Identify a key performance bottleneck in a real-world Python application (e.g., a data transformation task, a numerical simulation). Transpile only that single hot-spot function using `proto_python` and create a benchmark comparing the pure Python version to the hybrid version. The goal is to show a dramatic, order-of-magnitude speedup.

### Phase 2: Public Open-Source Release

**Goal**: To position Proto as a serious open-source project and attract contributors.

*   **[ ] Complete CMake Migration**: Finalize and polish the CMake build system.
*   **[ ] Establish Unit Test Suite**: Create a robust test suite for the core runtime to ensure stability and prevent regressions.
*   **[ ] Finalize World-Class Documentation (In Progress)**: Polish all documentation, including the architecture guides and the Doxygen API reference, ensuring it is comprehensive and professional.
*   **[ ] Create Community Guides**: Write a `CONTRIBUTING.md` file with clear guidelines for new contributors and label issues with `good first issue` on GitHub.

### Phase 3: Long-Term Ecosystem Development

*   **[ ] Build a Standard Library**: Develop foundational modules (e.g., file I/O, math, networking) using the Proto FFI.
*   **[ ] Develop Debugging & Introspection Tools**: Add hooks to the runtime to enable a step-by-step debugger and a visualizer for the heap and GC state.
*   **[ ] Design a Package Manager**: Create a simple system for the community to share and distribute Proto-based libraries.

---

## 4. Outreach & Community Plan

1.  **Technical Content Creation**: Write blog posts on platforms like Medium or a personal dev blog about the unique technical aspects of Proto. (e.g., *"Building a Low-Latency GC in C++"*, *"Transpiling Python to C++: A Case Study"*).
2.  **Academic Publication**: Publish a whitepaper on **arXiv.org** (in the `cs.PL` or `cs.DC` category) to lend academic credibility and visibility to the project.
3.  **Engage with Online Communities**: Share the blog posts, benchmarks, and demos on platforms like **Hacker News** (using `Show HN:`) and Reddit (in `/r/cpp`, `/r/compilers`, `/r/ProgrammingLanguages`).
4.  **Video Demonstrations**: Create a short, compelling video showing the benchmark running side-by-side. **Seeing the speed is more powerful than reading about it.**
