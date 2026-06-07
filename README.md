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
| **Test Coverage** | **148/148 tests passing** in both flag states (100%) ✅ |
| **Code Quality** | A+ - Excellent organization and documentation |
| **Architecture** | Exemplary - Hardware-aware design, GIL-free concurrency |
| **Production Ready** | **No** - Open for Community Review |

### Recent Improvements (2026)

- ✅ **GC: Concurrent Mark via Per-Cycle Mutable-Shard Snapshot — STW pause now decoupled from heap size** *(May 2026)* — The mark phase now runs **OUTSIDE** the Stop-The-World window.  At STW Phase 2, the GC atomically copies the 256-entry `mutableRoot[]` shard table into a per-cycle `gcMutableSnapshot[]` (~2 KB, 32 cache lines); workers resume and may CAS-swap shards freely while the marker traverses a frozen view rooted at the snapshot.  Sweep was already concurrent; this completes the picture.  In a follow-up trim, the tuple-interner AVL walk also moved out of STW (Phase 2 now pushes only `tupleRoot`; mark traces `key/previous/next` via `TupleDictionary::processReferences`), and the dead `stringInternMap` scan was removed (canonical interned strings live in `SymbolTable` which is perennial and never scanned).  Net effect: **no component of the STW window scales with the live heap or the mutation rate** — the residual STW cost is bounded by thread count, per-thread stack depth, and a fixed 256-pointer snapshot.  Typical STW pause: **30–250 μs** (sub-millisecond with comfort) on realistic workloads.  Places protoCore in the same latency category as ZGC / Shenandoah / Go — but achieved without load barriers, Brooks pointers, hybrid write barriers, or multi-mapping; the architectural pivot is "concentrated mutability in N shards", so a tiny snapshot replaces every per-write barrier.  Soft-real-time grade for UI / gaming / web SLA / general server workloads.  No per-mutation overhead.  Floating garbage bounded by one cycle.  See [`docs/GarbageCollector.md`](docs/GarbageCollector.md) § "Concurrent Mark Without Barriers" + "STW Pause Anatomy and Latency Profile" for the full design, cost breakdown, and comparison; the conclusion in [`docs/STW_ELIMINATION_RESEARCH.md`](docs/STW_ELIMINATION_RESEARCH.md) §§ 5-6 that "concurrent mark requires Dijkstra/SATB write barriers" is explicitly corrected in § 13 of that note — that conclusion was implicitly conditioned on dispersed mutability, which protoCore's design rejects.  Tests: 205/207 pass plus a new `ConcurrentMarkSafetyTests` suite (4 worker threads × 20K mutations during continuous GC pressure; mutable reference chain integrity across cycles).
- ✅ **SmallSparseList: single-cell inline (key, value) form for ≤ 3 entries** *(May 2026)* — New `ProtoSparseListSmallImplementation` cell type that holds up to 3 (unsigned long key, ProtoObject* value) pairs inline in a single 64-byte Cell (vtable 8 + next_and_flags 8 + keys[3] 24 + values[3] 24 = 64 B exact).  Mirrors the existing `ProtoListSmallImplementation` pattern: tag-dispatched trampolines on the public `ProtoSparseList::*` API route reads and writes by `POINTER_TAG_SPARSE_LIST_SMALL` (= 26) vs `POINTER_TAG_SPARSE_LIST`, so callers see no API change.  Aggregate operations (`setAt`) **promote** to the AVL form when the result would exceed `MAX_INLINE = 3` or when the caller stores key 0 (reserved as the empty-slot sentinel); `removeAt` deliberately does NOT degrade the AVL form back to Small — the asymmetry keeps the hot path simple.  `ProtoContext::newSparseList()` now returns an empty Small as the fresh-allocation default.  `ProtoObjectCell::attributes` is retyped to the public `ProtoSparseList*` so attribute tables can transparently sit in either form, and a new internal `sparseListGetRaw` inline helper preserves the *nullptr-for-absent* vs *PROTO_NONE-for-explicit-None* distinction across the GC roots / mutable-shard / `getOwnAttributeDirect` paths (without it `attr = None` would test as missing — broke `from posixpath import altsep` until fixed).  Closure cells in protoJS — always exactly 1 attribute (`__cv__`) — drop from 3-4 cell allocations per write down to 1; small Python instance dicts benefit equivalently.  16 new unit tests; 181/181 protoCore tests pass; 33/33 protoJS, 178/178 protoPython downstream sit unchanged.  End-to-end protoJS-vs-Node geomean: **41.05× → 40.17×** (12 outer × 5 inner = 60-sample median); function_calls.js (the canary that motivated the work, profile-led to identify SparseList rebuild as the dominant cost in closure-cell writes): **457 ms → 389 ms (−15 %)** on the same hardware.
- ✅ **protoJS embedder-side P-JS-7 (dispatch_table hoisted)** *(May 2026)* — Final embedder-side win of the May 2026 cycle.  The 256-entry computed-goto table was re-initialised at every entry to `runBytecode`: 256 default-fills + ~210 per-opcode overrides = ~470 stores per call.  For tree_traversal (65 K recursive calls × 5 iters) that was ~150 M wasted stores per bench run, and crucially each frame held 2 KB of duplicate dispatch table on the C++ stack — with recursion depth 14, ~28 KB of cache-line traffic competing with L1d (32 KB).  Fix: function-scope `static const void* dispatch_table[256]` + DCLP (`std::atomic<bool>` flag + `std::mutex`); the static is initialised on the first `runBytecode` call across the process and reused thereafter.  Address-of-label values are stable per binary load (labels live at fixed code-segment offsets), so single-process initialisation is correct.  Steady-state cost on every subsequent `runBytecode` entry: 1 acquire-load + a predicted-not-taken branch (~2 cycles).  This is the largest single win of the cycle; flat profiles missed it because the cost was attributed to the `runBytecode` self-time symbol (no individual hot store stood out).  Bonus discovery: the bench runner had been silently measuring a stale `build/protojs` binary for the entire P-JS-{0..6} + SmallSparseList period — every "+1-2 % per step" reported earlier was noise on a 2-day-old binary.  The cumulative TRUE impact of the cycle, measured against the actually optimised binary: end-to-end **75.13× → 28.94×** Node-vs-protoJS geomean (−62 %); vs vanilla QuickJS the gap drops to **7.05×** (was estimated 9.67× on the stale baseline, but that comparison was invalid).  Per-bench: tree_traversal 1004→316 ms (−69 %), function_calls 448→189 ms (−58 %), object_write_only 1430→823 ms (−42 %), object_property 598→359 ms (−40 %).  No protoCore code change.

- ✅ **protoJS embedder-side P-JS-6 (DISPATCH hot-path trim)** *(May 2026)* — Embedder-only follow-up to SmallSparseList.  protoJS's computed-goto bytecode dispatcher used to re-fetch `globalObj` from `*pGlobalRoot` on every opcode dispatch and to null-check the dispatch_table slot, even though `globalObj` is read by only ~6 specific opcodes and the table is pre-populated for every implemented opcode.  Pre-filling the unimplemented slots with `&&L_default` and moving the `globalObj` refresh to a `REFRESH_GLOBAL_OBJ()` macro invoked at the consumer sites drops per-dispatch overhead from ~19 cycles to ~12 cycles (−37%).  No protoCore change.  End-to-end protoJS-vs-Node geomean: **40.17× → 39.40×** (60-sample median); per-bench tree_traversal 885→847 ms (−4%), control_flow 259→255 (−2%); function_calls dispatch density was already saturated by the SmallSparseList commit, so its single-bench delta sits within noise.

- ✅ **protoJS embedder-side P-JS-{0..5} cycle** *(May 2026)* — Embedder-only follow-up to the May polish, mirroring the protoPython strategy: minimise protoCore traffic on the JS property-access hot path. **P-JS-0** removed the vestigial JSValue ↔ ProtoObject mapping update on every write (QuickJS is parser/compiler only; objects live in protoCore exclusively at run time, so the mapping was unread).  **P-JS-1** caches `__get_<name>__` / `__set_<name>__` accessor sidecar symbols per name in a thread-local map — the hot OP_get_field2 miss probe used to construct a fresh ProtoString rope every call.  **P-JS-2** dedup'd a redundant `getAttribute(callbacks=true)` in OP_get_field2.  **P-JS-3** replaced six pointer-compares per write in `updateSpacePrototypeIfMatching` with a cache-line-resident 8-slot prototype-identity set.  **P-JS-4** short-circuits `JSObjectBehavior` virtual dispatch for plain objects (the dominant case): when the resolved behavior is the registry's default, the interpreter calls `obj->getAttribute` / `obj->setAttribute` directly and skips the v-table indirection.  **P-JS-5** profile-guided fix discovered via `perf record` on `function_calls.js` (44× behind QuickJS): the May 5 commit `47ea3e1a` had only converted `OP_call_method` to the single-allocation argsList builder, leaving every free function call (`f(x)`) and `new X(...)` on the legacy `newList() + N×appendLast` path — 1+N cell allocations per call instead of 1.  Per-bench impact (12-round 60-sample medians): tree_traversal 1004→890 ms (−11%), object_property 598→531 ms (−11%), string_concat 176→157 ms (−11%), control_flow 307→280 ms (−9%), object_write_only 1430→1332 ms (−7%).  End-to-end Node-vs-protoJS geomean: **54.4× → 41.05×**; vs QuickJS the gap holds at **9.86×** (was 9.68×, within noise) with parallel_cpu widening to 14.1× protoJS-favored.
- ✅ **String / GC late-May polish** *(May 2026)* — Stripped two latent UAF surfaces in protoJS-shaped workloads and one redundant hot-path branch in `getAttribute`.
  1. **Runtime string intern removed.** `ProtoStringImplementation::wrapRoot` and `ProtoString::create` no longer feed every freshly built rope through the global `stringInternMap`.  The map's content-keyed hash (`computeContentHash`) walked the entire rope, so a tight `s += 'x'` loop became O(N²) and timed out at 50 000 concats; after the change the same loop completes in 129 ms.  Symbols (attribute keys, identifiers) keep their dedicated perpetual table via `ProtoString::createSymbol`; ad-hoc ropes now skip dedup, and equality / sort / hash-table-key sites compare via `ProtoObject::compare` (which already walks content) and `getHash` (which returns the cached `subtree_hash` in O(1)).
  2. **`createSymbol` allocates the working impl with NULL `ProtoContext`.** Strong symbols are eternal by contract — a name once interned must never be reclaimed — so routing the build through `ctx` gained nothing and exposed a window where a concurrent collector could see the in-flight rope as a candidate, miss it because it was held only via a C++ local, and free it before `SymbolTable::intern` could record the canonical pointer.  With NULL context every cell here lives for the lifetime of the process and the race is structurally impossible.
  3. **`ProtoObject::getAttribute` no longer pays a per-call atomic load + branch for GC-cycle cache invalidation.** `ProtoThreadExtension::processReferences` already pins every `(object, result, name)` entry as a GC root, so the cache pointers cannot dangle: the cells they reference stay alive and the arena cannot recycle their addresses while the cache holds them.  Stripping the guard saved 5–11 % on `getAttribute`-heavy protoJS benches (`numeric_loop`, `array_literal`, `function_calls`, `control_flow`); end-to-end Node-vs-protoJS geomean dropped from 68.15× to 54.4×.
- ✅ **GC Survivor Re-chain & Per-Context Threshold Submission — now default ON** *(May 2026)* — Closes the long-standing survivor-tenuring leak in the concurrent mark-and-sweep collector and adds a per-context allocation threshold that bounds RSS in tight loops with small working sets. Cells that survive a sweep are now re-pushed to `dirtySegments` for the next cycle (instead of being dropped from the analysis set forever); when a context's `allocatedCellsCount` crosses `ProtoSpace::maxAllocatedCellsPerContext` the context hands its young chain over to `dirtySegments` at the next `safepoint()` call, freeing whatever has become unreachable. A new `ProtoContext::CriticalSection` RAII guard bars cooperative STW polling around every protoCore site that allocates ≥1 cell and only attaches them to a GC root via a final atomic store/CAS — `setAttribute` (mutable + immutable), `addParent`, `newChild`, `getAttributes`, `ProtoMultiset::add/remove`, `ProtoSet::add/remove`, every `ProtoList` mutator, every `ProtoSparseList` mutator, every `ProtoString` rope builder, `ProtoTuple::tupleFromList/Vector/getSlice`, `Integer::fromString/toString`, `LargeInteger::fromTempBignum`, `ProtoContext::newSet/newMultiset/newObject/fromUTF8String`, both `ProtoThreadImplementation` constructors, and the module-loader code paths.  Compile-time flag `PROTOCORE_GC_REINCLUDE_SURVIVORS` defaults to ON since the May 2026 audit; configure with `-DPROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` to bisect against the previous behaviour.  End-to-end protoPython benchmark `memory_pressure` (heavy alloc + drop, small working set): RSS drops from **1347 MB → 358 MB** and wall time from **190.9× → 43.4×** vs CPython 3.14; geomean across the suite improves from **3.69× → 3.06× (17% faster)**.  Threshold tunable at runtime via env var `PROTOCORE_GC_CONTEXT_THRESHOLD` (default 10 000 cells). Embedders driving their own bytecode loop must size `co_automatic_count` so the operand stack lives inside `automaticLocals` (a single `compiler.getMaxStack() + 32` pass over module / `eval` / `exec` / `py_compile` code-object construction is sufficient — see the protoPython tree for the canonical pattern). Design: [`docs/superpowers/specs/2026-05-03-gc-survivor-rechain.md`](docs/superpowers/specs/2026-05-03-gc-survivor-rechain.md). Implementation walk-through: [`docs/superpowers/plans/2026-05-03-gc-survivor-rechain.md`](docs/superpowers/plans/2026-05-03-gc-survivor-rechain.md).
- ✅ **`getAttribute` returns `PROTO_NONE` for missing keys** *(May 2026)* — Restores the documented API convention across two paths in `ProtoObject::getAttribute` that were silently returning `nullptr` (the symbol-table early-out and the prototype-chain exhaustion path). Existing protoPython callers were already handling both cases via `if (!x || x == PROTO_NONE)`, so this is a no-op for them; the change closes `ObjectTest.GetMissingAttribute` and aligns every internal caller on the same sentinel. `nullptr` now means *invalid input* (null `this` / `name` / `context`) and `PROTO_NONE` means *not found*.
- ✅ **Public Inline SmallInt Helpers** *(April 2026)* — Adds four `static inline` helpers in `protoCore.h` (`proto::isSmallInt`, `proto::asSmallInt`, `proto::smallIntInRange`, `proto::makeSmallInt`) so embedders can short-circuit the SmallInt+SmallInt fast path in their bytecode dispatcher without crossing the protoCore shared-library boundary.  The helpers expose **only the bit-pattern of the SmallInt tag** (signed 54-bit value at bits 10-63, fixed low-10-bit tag); no internal types, no internal symbols, no library ABI change.  protoPython's `OP_BINARY_ADD` / `OP_INPLACE_ADD` / `OP_BINARY_SUBTRACT` / `OP_INPLACE_SUBTRACT` / `OP_COMPARE_OP` handlers use them to skip ~10 cross-DSO function calls plus 6 redundant tag checks (the previous `binaryAdd → unwrapPrimitive → ProtoObject::add → Integer::add` chain) for the >90 % SmallInt+SmallInt case.  End-to-end protoPython benchmark `call_recursion` (fib(25), 242 K calls, every call doing one compare + three integer ops) drops 866 ms → 716 ms (**−17 %**); geomean ratio vs CPython 3.14 drops 5.53× → **5.34×** (best protoPython has hit).
- ✅ **Cooperative GC Safepoint Public API** *(April 2026)* — Adds `ProtoContext::safepoint()` to the public header so embedders (e.g. protoPython's bytecode dispatcher) can participate in stop-the-world from tight allocation-free hot loops.  Without it, a CPU-bound thread that never reaches `allocCell`'s 64-allocation poll starves the GC indefinitely and stalls every other thread waiting for STW.  Fast path is a single relaxed atomic load on `stwFlag`; slow path joins the same `parkedThreads`/`stopTheWorldCV` handshake `allocCell` already uses internally.  Also closes a thread-startup race in `ProtoThreadImplementation`: `runningThreads` is now incremented from `thread_main` (when the OS thread actually starts) rather than from the constructor (before `std::thread` was spawned), eliminating a window where GC waited for a phantom running thread that had no way to park.  protoPython multi-threaded benchmark wall-time dropped from 1217 ms to 165 ms (2.55× CPython, was 18.90×) after wiring this safepoint into the bytecode loop.  Per-thread allocation batches in `getFreeCells` are sized `min(blocksPerAllocation × runningThreads × 4, 65 536)` when more than one thread is running, so with 4 workers + GC each refill yields 65 536 cells; profiles confirm a typical worker calls `getFreeCells` exactly once per benchmark run and the global mutex is well-amortised — the remaining gap to CPython on `multithread_cpu` is per-bytecode interpreter cost in protoPython, not allocator contention in protoCore.
- ✅ **Attribute Cache Optimization: 2026 Final Overhaul** *(May 2026)* — Complete restoration of the high-performance attribute resolution engine. Achieved **~8.2 ns** latency for hot-cache lookups via a non-lossy, thread-local inline cache (TL-IC). The engine now distinguishes between missing attributes and `PROTO_NONE` values without performance penalty, and implements O(1) jump resolution for deep inheritance chains. Added `CACHE_FLAG_OWN` tagging to safely enable caching for `getOwnAttributeDirect` and `hasOwnAttribute`, resulting in a ~10% gain for local property checks. Validation via `performance/microbenchmark_final.cpp`.
- ✅ **Mutable Hot Path: 256 Shards + Per-Thread Snapshot Cache (with negative caching)** *(April 2026)* — `mutableRoot` widens from 16 to 256 cache-line-padded shards (`mutable_ref % 256`), and every thread carries a 1024-entry `MutableValueCacheEntry` table that short-circuits the "atomic load + AVL `implGetAt`" sequence on the common own-thread mutable-read path. The cache stores **negative** results too (commit `75fee285`), so unmutated mutables — common for newly created functions / classes / dicts — pay zero AVL after the first read. Validation by shard-root pointer equality means any successful CAS by any thread invalidates stale entries on the next lookup — no broadcast, no signaling. Cache entries are GC roots, traced via `ProtoThreadExtension::processReferences`. Design: [`docs/MUTABLE_SHARDING_AND_CACHE_REFACTOR.md`](docs/MUTABLE_SHARDING_AND_CACHE_REFACTOR.md). Full bench report: [`protoPython/benchmarks/reports/2026-04-25-mutable-cache.md`](../protoPython/benchmarks/reports/2026-04-25-mutable-cache.md).
- ✅ **Three-Tier AVL String System** *(April 2026)* — Complete rewrite: embedded UTF-8 in tagged pointer (≤6 bytes, zero allocation), interned Symbols via 64-shard `SymbolTable`, and non-interned heap Strings — all sharing a uniform public API. 136/136 tests passing.
- ✅ **Complete API Implementation** - All 36 missing methods implemented, achieving 100% API completeness
- ✅ **Buffer API Completion** - Full ProtoByteBuffer public API with factory methods
- ✅ **GC Stress Testing** - Validated and optimized garbage collection behavior
- ✅ **Comprehensive Technical Audit** - Complete architecture and implementation review
- ✅ **Documentation** - Full API documentation and technical specifications
- ✅ **protoJS Runtime** - Complete JavaScript runtime built on protoCore (Phase 6 Complete)
- ✅ **protoPython Runtime** - GIL-free Python 3.14 environment built on protoCore (Phase 6 Complete)
- ✅ **protoST Runtime** - Actor-native Smalltalk-80 runtime built on protoCore (cooperative yield, DAP debugger)

### Community & Open Review

Beyond formal audits, this project is officially **open for Community Review and Suggestions**.

We welcome architectural feedback, edge-case identification, and performance critiques. While the core vision is firm, the path to perfection is a collective effort of the "Swarm."
---

## The protoCore Ecosystem: protoJS, protoPython & protoST

protoCore serves as the robust foundation for several high-performance runtimes, demonstrating its versatility as a universal object system for the AI-augmented engineering era. Three complete runtimes are built on it today — a JavaScript engine, a GIL-free Python environment, and an actor-native Smalltalk — so whichever one you arrive through, the same core powers all three.

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

### 3. protoST: Actor-Native Smalltalk for Digital Twins
**protoST** is a Smalltalk-80 runtime built on protoCore, designed as a platform for digital twins — any object can become an actor whose messages return futures, modelling physical components as independently scheduled state machines.

**Highlights:**
- ✅ **Actor Model** - `obj asActor` yields a transparent proxy; messages return Futures combinable with and/or
- ✅ **Cooperative Yield** - actors awaiting a future release their worker thread; thousands of interdependent actors share a handful of OS threads without thread starvation
- ✅ **Real Parallelism** - managed `ProtoThread` worker pool over protoCore's GIL-free concurrency
- ✅ **Tooling From Day One** - interactive REPL and a Debug Adapter Protocol (DAP) debugger for VS Code

**Learn More:** See the [protoJS project](../protoJS/), [protoPython project](../protoPython/), and [protoST project](../protoST/) for complete documentation.


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
    *   **Concurrent Garbage Collector with Snapshot-at-STW**: A dedicated GC thread runs **mark, sweep, and bulk-unmark concurrent with user threads**.  The STW window contains only the cooperative thread handshake and a fixed-cost root capture (a 256-pointer snapshot of the mutable-shard table — protoCore concentrates all mutability in `MUTABLE_ROOT_SHARDS=256` shards, so a 2 KB snapshot is a complete snapshot of every mutable in the system).  **No component of the STW window scales with the live heap or the mutation rate.**  Typical pause: **30–250 μs**, comparable to ZGC, Shenandoah, and Go — but with **zero write barriers, zero load barriers, no Brooks pointers, no multi-mapping**.  The architectural pivot is concentrated mutability: a tiny snapshot replaces every per-write barrier.  Soft-real-time grade for UI, gaming, web SLA, and general server workloads.  See [`docs/GarbageCollector.md`](docs/GarbageCollector.md) for the full design and latency profile.

## Performance Validation (2026 Audit)

To validate the theoretical performance of the `protoCore` object model and its per-thread attribute cache, we conducted high-precision microbenchmarks and compared the results against industry standards for hash-based lookups in other major runtimes.

### Benchmark Results (Low-Nanosecond Attribute Access)

Refreshed on 2026-05-06 — median of 5 runs of `performance/microbenchmark_final.cpp` (10 M iterations per scenario, Release build with `-O3 -DNDEBUG`, single-threaded).  May 2026 perf-investigation cycle landed paths #2/#3/#4/#6, task #28 (CAS removal), task #34 (destructor reorder UAF fix), and **task #36 (chunked freelist via GC pre-chunking)** — every microbench metric is at or below the pre-revert May 2026 targets.

| Scenario | Latency (ns/op) | Note |
| :--- | :--- | :--- |
| **getAttribute (Hot Cache)** | **~7.7 ns** | Single-probing TL-IC hit for pre-interned symbols. |
| **hasAttribute (Hot Cache)** | **~10 ns** | Non-lossy existence check (distinguishes PROTO_NONE). |
| **getOwnAttributeDirect** | **~5.8 ns** | Direct property access, bypasses inheritance walk. |
| **Inherited Attribute (10-level)** | **~36.8 ns** | Prototype-chain walk; ~3.7 ns per inheritance level. |

**Allocator path (task #36, chunked freelist):** `ProtoSpace::getFreeCells` dropped from 7.91 % to **0.52 %** of bench CPU on `bench_binary_trees(10)` after sweep started pre-chunking dead cells into 8192-cell `FreeChunk` units.  `getFreeCells` becomes O(1) (one chunk pop) instead of O(N) (linked-list walk to find a cut point).

**Attribute-protocol fast paths (tasks #37 and #39 — protoPython side, May 2026):** the two protoPython-level mirrors of CPython's descriptor / slots / class-shape protocol used to perform 3-5 protoCore probes per `LOAD_ATTR` / `STORE_ATTR` (each one a `hasOwnAttribute` or `getOwnAttributeDirect`).  A new per-class `__pyflags__` SmallInt cache (computed once on first read, stored as an own-attribute on the class) collapses those probes into a single cache hit covering `IS_PYTHON_CLASS`, `HAS_SLOTS`, `HAS_DATA_DESCR` and `HAS_GET_DESCR`.  `OP_LOAD_ATTR`'s descriptor check, `OP_STORE_ATTR`'s slots/descriptor probe, `tryFastGetAttribute`'s descriptor check and `PythonEnvironment::setAttribute`'s MRO walk all now route through the same cached invariants — eliminating between 25 % and 80 % of the protoCore calls per attribute access depending on path.

**`isString` cascade elimination (task #42 / P8, May 2026):** ProtoSparseList nodes computed a hash by propagating `v->getHash(ctx)` up the tree at construction.  Inside `getHash`, the wrapper-object protocol called `isString(ctx)` which itself called `getAttribute(literalData)` — a chain walk per SparseList node built.  The hash field was retained for ABI but never read externally; setting it to 0 broke the entire cascade.  Profile delta on `bench_binary_trees(10)`:

  - `isString`:  3.78 % → 0 % (eliminated as hotspot)
  - `getAttribute`:  14.03 % → 5.90 % (cascade gone)
  - `protoCore::ProtoObject::isStringTagFast` static inline (P6): replaces the function call at 15+ interpreter sites where the receiver is always a co_names entry (interned).

bench_binary_trees(10) wall-clock (60-run baseline solid measurement):
  - **Baseline: min=2241  median=2547  q3=2996 ms**
  - **Post-cycle: min=1700  median=2080  q3=2557 ms** (~18 % median, ~24 % min)

attr_lookup benchmark: **108 → 81 ms (~26 % faster)**.
list_append_loop: **265 → 234 ms (~12 % faster)**.

**TL-IC entry padding (digression, 2026-06-06):** `AttributeCacheEntry`
went from 24 to 32 bytes.  Two motivations, both verified via
`objdump` before/after on `libprotoCore.so`:

  1. **Indexing collapses to one shift.**  A 24-byte stride forces the
     compiler to emit a LEA chain — `lea (%rax,%rax,2)` then
     `lea (%r15,%rax,8)` — to compute `&cache[idx]`.  At 32 bytes it
     becomes a single `shl $0x5, %rbx` + `add`.
  2. **No split-line loads.**  64-byte cache lines hold exactly two
     32-byte entries; no entry crosses a line boundary.  Allocation
     moved from `std::malloc` (16-byte guarantee) to
     `std::aligned_alloc(64, ...)`.

`perf stat -r 10` on `object_access_benchmark` (1000 objects × 10 K
attribute reads = 10 M `getAttribute` hits, single CPU pinned):

| Metric              | Before (24B)        | After (32B)         | Δ        |
| :---                | :---                | :---                | :---     |
| **Wall-time**       | 7.384 ± 0.216 s     | **6.673 ± 0.032 s** | **-9.6 %**  |
| **Cycles**          | 25.16 B (σ 1.36 %)  | **24.26 B** (σ 0.23 %) | **-3.6 %**  |
| Instructions        | 59.46 B             | 59.46 B             | ≈ 0      |
| **IPC**             | 2.36                | **2.45**            | **+3.8 %**  |
| **L1 dcache misses**| 3.98 M (σ 7.68 %)   | **2.90 M** (σ 2.10 %) | **-27.1 %** |

Instructions are essentially identical (the change is pure
addressing-mode), so the cycles improvement is microarchitectural:
fewer split-line loads + a shorter address-of-entry dependency
chain.  Memory cost: 8 KB extra per thread.

Two related proposals (devirtualize `Cell::getType()`, speculative
prefetch in the AVL walk) were **rejected by the baseline data**
before any code change: branch-miss rate sat at 0.13 % (the BTB
already calls the right target) and L1-miss rate at 0.02 % (the
working set fits in L1).  Either change would need a different
workload to justify.

**TL-IC cache-key hash improvement (same digression):** the per-thread
attribute cache hash used to be `(currentValue ^ (name >> 4)) % 1024`.
Since `ProtoObject` cells are 64-byte aligned, the low 6 bits of
`currentValue` are always zero — they cancelled 6 of the 10 mask bits,
leaving only ~4 useful bits of `currentValue` selecting the slot.  A
standalone distribution probe on a 250-object × 4-attr workload
confirmed the loss: only **64 unique slots out of 1024 used** for
1000 entries.  Shifting `currentValue` by 6 first
(`((currentValue >> 6) ^ (name >> 4)) % 1024`) lifts that to **256
unique slots** at zero extra cost (just an extra `shr` per lookup).
A Knuth-multiplier variant (`(name >> 6) * 0x9E3779B1`) reached **756
unique slots** in the model but the runtime `IMUL` cost outweighed
the better distribution on real pointer patterns.

`perf stat -r 10`, pinned to one CPU:

| Bench / Hash form | OLD `obj ^ (n>>4)` | **v1 `(obj>>6) ^ (n>>4)`** | v3 Knuth multiplier |
| :--- | ---: | ---: | ---: |
| **hash_quality (500 obj × 20 K, fits cache)** | 586.9 M cyc (σ 4.23 %), IPC 2.82 | **493.3 M cyc (σ 0.58 %), IPC 3.39** | 495.7 M cyc (σ 1.81 %), IPC 3.41 |
| **object_access (1000 obj × 10 K, 99.99 % hit)** | 25.75 B cyc (σ 1.39 %), IPC 2.31 | **24.05 B cyc (σ 0.30 %), IPC 2.47** | 24.44 B cyc (σ 0.50 %), IPC 2.43 |

v1 wins on every metric, run-to-run variance drops 4–7×, and zero
extra hardware cost.  The `MutableValueCache` hash is *not* modified
— that cache is indexed by the integer `mutable_ref` directly, so the
optimisation does not apply there.

**Negative result documented:** padding `MutableValueCacheEntry`
from 24 → 32 bytes (the symmetrical move) **increased** cycles by
5.3 % and L1 misses by 6.6 % on a dedicated mutable-heavy bench.
Why: that cache is indexed by `mutable_ref % 1024`, accessed nearly
sequentially in this workload — the hardware prefetcher already
streams it, so there was no split-line problem to solve, and the
extra 8 KB merely displaced useful AttributeCache lines.  Change
reverted.  Lesson: symmetric struct layout does not imply
symmetric impact — each cache must be evaluated against its own
access pattern.  Commit `41dd2448` keeps the benchmark and the
measurement so future maintainers don't re-explore the same dead
end.

Auxiliary protoCore microbenchmarks (median of single Release run, see `performance/`):

| Benchmark | Workload | Result |
| :--- | :--- | :--- |
| **list_benchmark** | 100 K appends | 112 ms |
| **sparse_list_benchmark** | 100 K inserts (size 1 K), then access | 150 ms insert / 18 ms access |
| **string_concat_benchmark** | 10 K rope concats | 13 ms |
| **immutable_sharing_benchmark** | 1 000 versions × 10 K-element list (structural sharing) | 0.5 ms versioning |
| **concurrent_append_benchmark** | 4 threads × 10 K appends | 43 ms |
| **cache_timing_benchmark (simple hit / mutable hit / AVL-1 / AVL-50)** | 10 M ops each | 5.7 / 7.5 / 9.2 / 8.6 ns |

### Comparative Latency: protoCore vs. Industry Standards

When compared to standard hash-based lookups in high-level languages and standard libraries, `protoCore`'s object model demonstrates a significant performance advantage:

| System / Operation | Average Latency | Comparison |
| :--- | :--- | :--- |
| **protoCore (TL-IC)** | **~7.7 ns** | **Reference baseline** |
| **Python `getattr`** | ~20ns - 70ns | 2.6x - 9.1x slower |
| **Java `HashMap.get()`** | ~30ns - 100ns | 3.9x - 13x slower |
| **C++ `std::unordered_map`** | ~30ns - 80ns | 3.9x - 10.4x slower |
| **Main Memory (L3 Miss)** | ~100ns | ~13x slower |

### Architectural Advantages

The performance gap is a result of fundamental architectural choices designed for modern hardware:
- **Thread-Locality**: Thread-local caches eliminate atomic operations and synchronization overhead in the hot path.
- **Symbol Interning**: Attribute keys are pointer-identities, reducing comparison to a single CPU register check instead of string hashing or `strcmp`.
- **Hardware-Aligned Cells**: 64-byte object alignment prevents false sharing and ensures all attribute metadata fits within a single CPU cache line.
- **Non-Lossy Design**: The cache distinguishes between "not found" and "found with value None" without additional branches, using a dedicated bit-tagging system.

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

### Real-World Reference Implementations

Three complete language runtimes are built on protoCore — each a reference for a different style of embedding:

- **protoJS** - A complete JavaScript runtime built on protoCore, demonstrating production use of all protoCore features. See the [protoJS repository](https://github.com/gamarino/protoJS) for implementation examples and best practices.
- **protoPython** - A GIL-free Python 3.14 environment, showing protoCore's concurrency model and the HPy / UMD interop paths. See the [protoPython repository](https://github.com/gamarino/protoPython).
- **protoST** - An actor-native Smalltalk-80 runtime, showing protoCore's threading, per-method contexts, GC root discipline, and embedder-driven cooperative scheduling. See the [protoST repository](https://github.com/gamarino/protoST).

## Contributing

We welcome contributions! This project is at a perfect stage for developers interested in compilers, memory management, and high-performance C++. Please check out `DESIGN.md` for architectural details. Feel free to open an issue on GitHub to discuss your ideas.

## Acknowledgments

ProtoCore is developed and maintained by Gustavo Marino. The project represents years of careful design and implementation focused on performance, safety, and developer experience.

---

**Don't just watch the shift. Lead it.** The tools are here, the barrier is gone, and the only limit is the clarity of your vision. Join the review, test the limits, and become part of the Swarm of One. Let's build the future of computing, one cell at a time. **Think Different, As All We.**
