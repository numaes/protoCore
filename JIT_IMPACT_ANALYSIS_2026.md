# ProtoCore: JIT & Architectural Strategy 2026

Following a critical re-evaluation of the "ProtoVM: Bytecode JIT" proposal, this document addresses the "Nucleus Paradox": whether a minimalist object system should own its execution policy (JIT) or merely provide best-in-class mechanisms for external compilers.

## 1. The Nucleus Paradox: Critical Analysis

As the core object model and memory manager, ProtoCore occupies a "nucleus" role. Attempting to implement a self-contained JIT within this layer introduces several fundamental risks:

*   **Policy vs. Mechanism**: A JIT requires deep knowledge of high-level language semantics (type coercion rules, specialized dunder-method dispatch, exception models). Implementing this in ProtoCore forces the library to "guess" the user's language policy, leading to a complex and likely incomplete implementation.
*   **The Invalidation Burden**: High-performance JITs spend a significant portion of their codebase on **guard checking** and **deoptimization**. In a nucleus library, tracking high-level type stability to invalidate machine code would exponentially increase maintenance complexity for a marginal gain that might be better handled by a specialized compiler (like ProtoJS).
*   **The Immutable Model Paradox**: While immutability simplifies some JIT optimizations (stable fields), it complicates others if the user-level interpreter allows dynamic prototype reassignment or re-interpretation of object identity.

## 2. Expected Impact & Risks

| Impact Category | Full JIT (Self-Contained) | Mechanism Focus (Hooks) |
| :--- | :--- | :--- |
| **Performance** | Extreme for narrow benchmarks | High for real-world integration |
| **Complexity** | Exponential (~14k+ LOC) | Linear (~1k LOC) |
| **Flexibility** | Rigid (ProtoCore's way) | High (Supports many languages) |
| **Maintenance** | High (Platform-specific asm) | Low (Pure C++) |

### Strategic Risks

*   **Scope Creep**: A JIT would move ProtoCore from a "Tiny & Precise" tool to a "Large & Opinionated" runtime, potentially alienating users who only need the concurrent object model.
*   **Hardware Portability**: JITs are notoriously non-portable. ProtoCore's current cross-platform C++20 standard is a major asset that a JIT would undermine.

## 3. Alternative Strategy: "Mechanism over Policy"

Instead of a full-blown JIT, ProtoCore should focus on providing **low-level primitives** that high-level compilers can use to generate their own optimized code:

### 3.1 Object Shapes (Hidden Classes)

Instead of a JIT, ProtoCore should expose the "Shape" of an object as a first-class optimization mechanism. A compiler can check if `obj->shape == constant_shape` and then use a precomputed offset to access a field, skipping the AVL lookup entirely.

### 3.2 Lowering-Friendly Memory Layout

Maintain the 64-byte Cell and Tagged Pointer stability. This allows external JITs (like a ProtoJS-specific JIT) to emit machine code that reads directly from common offsets, treating ProtoCore as a high-performance "backing store."

### 3.3 Optimized C++ Dispatch (Computed Goto)

If a VM is needed, implement a minimalist **Threaded Interpreter** in C++. It offers ~2-3x performance over a standard switch-loop without the multi-thousand-line burden of a machine-code JIT.

## 4. Final Recommendation: De-emphasize JIT

**Recommendation**: The proposal for a self-contained "ProtoVM JIT" should be deprioritized in favor of **Structural Optimization Hooks**.

ProtoCore's value is being the **fastest, safest nucleus**. It should remain a library that *enables* JITs at higher levels rather than being one itself. The focus for 2026 should remain on:

1.  **Vectorized Collection Primitives** (SIMD in C++).
2.  **Shape-based Attribute Caching** (Refining the existing mechanism).
3.  **FFI Parity** (Making it trivial for Rust/Python JITs to talk to ProtoCore).

## 5. Performance Grounding (2026 Audit)

The following metrics were captured on 2026-04-02 to provide concrete grounding for the "Mechanism over Policy" strategy.

### 5.1 Data Structure Primitives (Single-Threaded)

| Operation | Scale | Total Time | Latency (Avg) |
| :--- | :--- | :--- | :--- |
| **List Append** | 100,000 ops | 0.717 s | 7.17 µs |
| **SparseList Insert** | 100,000 ops | 0.371 s | 3.71 µs |
| **SparseList Access** | 100,000 ops | 0.016 s | 0.16 µs |
| **Object Attribute Access** | 10,000,000 ops | 35.14 s | 3.51 µs |
| **String Concatenation** | 10,000 ops | 7.433 s | 0.74 ms |

### 5.2 Concurrency & Sharing

| Scenario | Parameters | Result |
| :--- | :--- | :--- |
| **Immutable Versioning** | 10k base, 1k versions | 0.003 s (Versioning) |
| **Concurrent Append** | 4 threads, 4k total appends | 0.008 s |

### 5.3 Analysis of Results

*   **Sharing Efficiency**: The `0.003s` versioning time for 1,000 versions of a 10k-element list confirms the extreme efficiency of **Structural Sharing**. This validates the "Mechanism Focus" over a JIT that might struggle with immutable semantics.
*   **Attribute Bottleneck**: At `3.51 µs` per access, object attribute lookup is the primary candidate for the **Shape-based Attribute Caching** hook proposed in Section 3.1.
*   **Concatenation Scale**: String concatenation (`0.74 ms`) remains the most expensive operation due to AVL rebalancing. **Vectorized AVL Operations** (Section 3.2) are prioritized.

## 6. Conclusion

The raw data supports the transition to a **Mechanism-centric** architecture. ProtoCore's baseline performance is already competitive as a nucleus; the most significant gains will come from providing hooks for external compilers rather than increasing internal complexity with a self-contained JIT.
