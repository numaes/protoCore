# Changelog

All notable changes to protoCore are documented in this file.

## [1.2.0] - 2026-05-22
### Added
- **`ProtoObject::setAttributeIfEqual` — public attribute compare-and-swap** —
  `setAttributeIfEqual(ctx, name, expected, newValue)` writes `newValue` only
  if the receiver's current OWN value for `name` is still pointer-identical to
  `expected`, returning whether the swap happened. It exposes the shard-root
  CAS loop that `setAttribute` already runs internally, so an embedder can
  build a lock-free read-modify-write (e.g. appending to a list held under an
  attribute) with a CAS-retry loop instead of an external mutex. `expected ==
  nullptr` means "the attribute is currently absent". Requires a mutable
  receiver; an immutable receiver is a no-op returning `false`. This is the
  protoCore primitive that lets protoST drop its per-actor mailbox mutex.
- **Configurable heap allocation limit with reliable OOM detection** — a new
  `ProtoSpace::setHeapLimits(softCells, hardCells)` lets an embedder cap the
  `Cell` heap instead of growing it unbounded until the OS is exhausted. Both
  limits are in `Cell`s; `0` (the default) disables them and the allocator is
  bit-for-bit the previous unbounded path — the entire feature is gated on
  `maxHeapSize > 0`.
  - `getFreeCells` clamps every OS request to the remaining headroom, so
    `heapSize` never crosses the hard ceiling for ordinary allocations.
  - A thread that must wait for the GC leaves the running set first
    (`ProtoSpace::waitForHeapHeadroom` → `reclaimWaitLocked`), so it never
    stalls the Stop-The-World quorum.
  - The blocking check runs at outermost critical-section entry
    (`ProtoContext::heapLimitCheckpoint`, called from the `CriticalSection`
    constructor): a thread mid tree-build holds un-anchored cells and must stay
    in the running set, so it cannot wait there. An allocation that drains the
    cell pool *inside* a critical section may overshoot by at most one OS batch.
  - OOM is detected from per-cycle *reclamation*: the GC publishes
    `reclaimedLastCycle`; two consecutive completed cycles that each reclaim
    zero cells while the heap is at its ceiling are genuine OOM. A mark-phase
    live count would miss a live context's un-submitted young generation, which
    fills the heap without entering `markedList`. On confirmed OOM,
    `outOfMemoryCallback` is invoked once, then protoCore performs a controlled
    `std::abort()` with a diagnostic.
  - `getFreeCells(const ProtoThread*)` → `getFreeCells(ProtoContext*)`; new
    `ProtoSpace` members `softHeapLimit`, `reclaimedLastCycle`,
    `liveCellsLastCycle`, `memoryReclaimedCV`; new `ProtoContext` method
    `heapLimitCheckpoint`.
  - See `DESIGN.md` § "The Heap Allocation Limit and Out-of-Memory Detection"
    and `docs/superpowers/specs/2026-05-22-allocation-limit-oom-design.md`.

### Changed
- **Interned strings are always perennial** — `SymbolTable::intern` now builds
  every symbol with a null `ProtoContext` unconditionally; the `is_strong`
  parameter and the never-called `removeWeak()` weak-eviction path are removed.
  Previously a "weak" symbol could be allocated against a `ProtoContext` and
  collected by the GC — but every caller already interned strong, so the weak
  path was dead code, and it was unsound to keep: a perennial symbol whose
  bucket got evicted would lose its canonical pointer and break pointer-identity
  symbol comparison. Now, when a symbol is created — or a non-interned string is
  converted to one — the interned string is allocated outside the GC and lives
  for the lifetime of the process. No public API change
  (`ProtoString::createSymbol` is unchanged). See `SymbolTable.cpp` and
  DESIGN.md § "Perpetual allocations via NULL ProtoContext".

### Fixed
- **GC stale-mark bug** — Mark phase set the per-cell mark bit on every reachable
  cell, but Sweep only cleared that bit on cells inside the captured
  `segmentsToProcess` snapshot. Cells reachable from a root that lived outside
  that snapshot (young cells whose owning context never submitted, perpetual
  prototypes, tuple/string interner entries) carried `mark=1` over from the
  previous cycle. The next cycle's mark drain skipped them via the
  `if (!isMarked())` guard and never traced their children, so any candidate
  reachable exclusively through that path was freed by Sweep while still live.
  Reproduced 100% with `PROTOCORE_GC_REINCLUDE_SURVIVORS` enabled and a
  16k-node mutable-object workload (protoJS `tree_traversal` benchmark crashed 0/5).
- **Fix** — Added a pre-mark unmark pass that walks the live graph from all
  roots and clears the mark bit on every reachable cell before Phase 4 begins.
  Cost is `O(reachable cells)`, comparable to Mark itself, in exchange for a
  clean tricolor invariant at the start of every cycle. See
  `docs/GarbageCollector.md` § "Phase 4a: Pre-mark Unmark Pass".

### Tests
- 196/196 protoCore tests pass — including the `AllocationLimit*` suite
  (hard-limit reclamation, soft limit, `setHeapLimits` validation and an
  unrecoverable-OOM death test) and the `AttributeCas*` suite (compare-and-swap
  match / mismatch / absent / immutable, plus an 8-thread no-lost-update
  concurrency proof).

## [1.1.0] - 2026-04-02
### Added
- **Performance Benchmarking Suite**: Integrated micro-benchmarks for List, SparseList, Object Access, and String Concatenation into the CMake build system.
- **JIT Impact Analysis**: Comprehensive performance grounding for the "Mechanism over Policy" strategy, validating structural sharing efficiency and identifying attribute lookup bottlenecks.
- **String Handling Refactor**: Introduced **Inline Strings** (up to 7 characters) stored directly in `ProtoObject*` tagged pointers.
- Optimized hybrid data model for `ProtoString` (Inline + Rope).
- Enhanced `RopeCharacterIterator` and `ProtoStringIteratorImplementation` for performance.
- Direct bitwise comparison and hashing for inline strings.

### Fixed
- **API contract: PROTO_NONE for missing keys** — Align `getAttribute` and `ProtoSparseList::getAt` with the documented API: return `PROTO_NONE` instead of `nullptr` when an attribute or key is not found.
- **ProtoObject::getAttribute** — Returns `PROTO_NONE` instead of `nullptr` when the attribute does not exist (ObjectTest.GetMissingAttribute).
- **ProtoObject::hasAttribute** — Correctly treats `PROTO_NONE` as "not found" when checking attribute existence.
- **ProtoSparseList::getAt** — Returns `PROTO_NONE` instead of `nullptr` when the key is not in the map (ContextTest.LocalVariableAllocation).
- **ProtoMultiset** — `add`, `count`, and `remove` now treat `PROTO_NONE` as a missing entry, fixing MultisetTest.AddAndCount, MultisetTest.Remove, and MultisetTest.RemoveNonExistent.

### Tests
- All 136 tests pass (100% pass rate) following the performance infrastructure integration.

## [1.0.0] - 2024-01-22
- Initial release of protoCore shared library.
