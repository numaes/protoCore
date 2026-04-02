# ProtoCore: Runtime Strategy & Roadmap 2026

This document provides a comparative analysis of ProtoCore against modern concurrent runtimes, recommends target application domains, and outlines the strategic "next steps" following the successful April 2026 String Refactor.

## 1. Comparative Analysis

ProtoCore occupies a unique niche: **Embeddable, Immutable-First, and GIL-Free.**

| Feature | ProtoCore | BEAM | Pony | Clojure | Lua / QJS |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Memory Model** | Shared Memory (Cells) | Process Isolated | Actor Isolated | Shared (JVM Heap) | Shared (Single VM) |
| **Concurrency** | GIL-Free Multicore | Preemptive Processes | Actors / Capabilities | Software Transactional Memory | Usually Single-Threaded |
| **Immutability** | Mandatory (Deep) | Mandatory | Capabilities-based | Mandatory | Optional/None |
| **String Model** | 3-Tier AVL (Interned) | Binaries / Refc | Reference Counting | JVM Interning | Interned (JS) |
| **Footprint** | Tiny (~8k LOC) | Large (Full VM) | Moderate | Large (JVM) | Tiny |
| **Embeddability**| Exceptional (C++ API) | Difficult | Moderate | Difficult | Exceptional |

### Key ProtoCore Differentiators

*   **Zero-Allocation Short Strings**: The new "Embedded" tier (≤6 bytes) beats almost all competitors for identifier-heavy workloads (attribute maps, symbols).
*   **Synchronous Immutability**: Unlike STM (Clojure) or Actor passing (Pony), ProtoCore's immutable collections are instantly shareable across threads with NO synchronization overhead after creation.
*   **Hardware-Awareness**: 64-byte Cell alignment is specifically tuned for modern L1 cache lines, a level of optimization rarely found in higher-level VMs like BEAM or JVM.

## 2. Recommended Application Domains

Based on its architecture, ProtoCore is natively suited for:

### 2.1 High-Performance Game Logic

*   **Why**: Modern engines (Unreal, Unity) struggle with multi-threaded scripting due to mutability/race conditions. ProtoCore's "Immutable-First" approach allows game systems (AI, pathfinding, ECS data) to be processed in parallel across all cores without locks.
*   **Role**: A "Parallel Scripting Layer" that replaces Lua or C# for heavy logic.

### 2.2 Edge Computing & Serverless Runtimes

*   **Why**: Fast startup time and low memory footprint (64-byte Cells) make it ideal for cold-starting isolated functions.
*   **Role**: Alternative to WASM for dynamic, data-centric workloads that require high-performance string and collection handling.

### 2.3 Real-Time Network Middleware

*   **Why**: The AVL-based string system (O(log N) concat/split) is perfect for high-throughput packet parsing and message transformation where data is frequently sliced and recombined.
*   **Role**: Orchestration layer for high-concurrency message brokers (akin to a lightweight Erlang).

## 3. Recommended Next Steps (2026 Q3-Q4)

With the string refactor complete and 136/136 tests passing, the roadmap should focus on **utilization** and **further hardware optimization**:

### 3.1 Tiered Symbol Optimization

*   **Goal**: Further reduce the overhead of Symbol lookups.
*   **Action**: Implement "Symbol ID" caching in attribute maps (`setAttribute` / `getAttribute`). Instead of hashing strings, store/retrieve by the unique pointer of the interned Symbol directly (O(1) lookups).

### 3.2 Vectorized AVL Operations (SIMD)

*   **Goal**: Make bulk collection operations (List/String) even faster.
*   **Action**: Use SIMD (AVX2/NEON) to process multiple `StringLeafNode` buffers at once during concatenation and search.

### 3.3 ProtoVM: Bytecode JIT

*   **Goal**: Close the gap with V8 performance for hot loops.
*   **Action**: Currently, ProtoCore is a C++ object system. Adding a lightweight VM with a minimalist JIT compiler targetting the ProtoObject handles would move it from "High-Performance Library" to "High-Performance Language Runtime."

### 3.4 Python/Rust Bindings (FFI Expansion)

*   **Goal**: Increase adoption.
*   **Action**: Wrap the `protoCore.h` API in Crate (Rust) and C-Extension (Python) to allow these ecosystems to leverage ProtoCore's unique parallel-sharing capabilities.
