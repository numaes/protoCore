# Welcome to ProtoCore

For a unified index of all protoCore documentation, see [DOCUMENTATION.md](../../DOCUMENTATION.md) in the repository root.

ProtoCore is an ultra-high-performance, low-latency C++ runtime library engineered for systems where absolute, deterministic, near real-time performance is a strict requirement. 

Traditional virtual machines and runtime environments (like the JVM or V8) often sacrifice predictable latency for throughput, suffering from unpredictable Garbage Collection pauses and heavy synchronization overhead. ProtoCore rejects these compromises through a radical architectural design centered around extreme immutability, zero-overhead memory tracking, and mathematically lock-free state mutation. 

By providing highly deterministic, microsecond-scale execution characteristics, ProtoCore serves as an elite foundation for developing custom programming languages, high-frequency trading platforms, and real-time game engines.

## Architectural Pillars

ProtoCore achieves its performance guarantees through several interrelated architectural breakthroughs:

*   **Zero-Barrier Concurrent Garbage Collection:** Unlike traditional Java or JavaScript runtimes, ProtoCore requires absolutely no "write barriers" to track memory mutations. By leveraging an immutable-cell heap, the GC operates entirely in parallel with user execution, delivering extreme throughput without compiler-injected tracking overhead.
*   **Microsecond Stop-The-World (STW) Pauses:** The Garbage Collector's required pausing of application threads is strictly limited to an instantaneous root-scanning phase. By utilizing precise thread yielding at defined **Critical Sections**, STW pauses are mathematically deterministic and reliably execute in the microsecond range.
*   **Lock-Free Mutable Yarding:** Shared mutable state is entirely decoupled from object identity. State updates are executed via a 256-shard lock-free map (`mutableRoot`) backed by rapid localized allocators called **Mutable Yards**. This mathematically eliminates mutex deadlocks, prevents CPU cache-line bouncing, and allows true parallel scaling across massive multi-core hardware.
*   **Aggressive `protoContext` Caching:** Prototype-based attribute resolution is accelerated via an elite, thread-local attribute cache anchored to the `protoContext`. Deep polymorphic lookups execute in $O(1)$ time (~10 nanoseconds) without requiring cross-thread synchronization.
*   **Compaction-Free FFI:** Because the runtime never compacts or moves memory, integrating with external native C++ libraries is completely seamless. Native code can safely retain direct memory pointers to Proto objects without complex Handle indirections.

## Who is this for?

ProtoCore is engineered for developers operating at the vanguard of software architecture:

*   **Language Designers & Compiler Engineers:** An uncompromised, embeddable backend runtime for creating highly concurrent dynamic languages or JIT environments.
*   **High-Frequency / Fintech Engineers:** A predictable memory and execution model for systems where unpredictable GC spikes lead to direct financial loss.
*   **Game Engine Developers:** A robust foundation for orchestrating massive parallel entity logic without risking frame stutters or rendering drops.
*   **Advanced Systems Students:** A state-of-the-art case study analyzing alternatives to traditional tracing garbage collectors and mutual exclusion locks.

## Getting Started

Ready to integrate ProtoCore into your infrastructure?

*   **[Quick Start Guide](./guides/01_quick_start.md)** — Build the library and instantiate your first `protoContext`.
*   **[Creating Native Modules](./guides/05_creating_modules.md)** — Learn how to bind your C++ infrastructure to the Proto engine via the `ModuleProvider` API.

## Testing & Validation

ProtoCore maintains an extreme standard of reliability.

*   **[Testing Architecture Guide](../TESTING.md)** — Comprehensive overview of the parallelized CTest infrastructure, coverage reports, and CI pipelines.
*   **[User Testing Guide](./guides/04_testing_user_guide.md)** — Quick instructions for validating the runtime locally.

## Architectural Deep Dives

To truly leverage ProtoCore, one must understand the mechanics underlying its guarantees. These documents are written at a highly professional, engineering-level standard:

*   **[The Low-Latency Garbage Collector](./architecture/01_garbage_collector.md)** — Concurrency, write-barrier elimination, and Critical Sections.
*   **[The Mutability Model & Yarding](./architecture/02_mutability_model.md)** — Lock-free state management, the `mutableRoot`, and Mutable Yards.
*   **[The Object Model & protoContext](./architecture/03_object_model.md)** — Tagged pointers, prototype delegation, and context lifecycle.
*   **[FFI and Native Integration](./architecture/04_ffi_and_integration.md)** — Memory stability and managing GC critical sections across C++ boundaries.

## Community & Contribution

ProtoCore is open-source. We welcome rigorous peer review and architectural contributions.

*   **[GitHub Repository](https://github.com/your-repo/proto)**
*   **[Architectural Discussions](https://github.com/your-repo/proto/discussions)**
*   **[Contributor's Standard](./guides/03_contributing.md)**
