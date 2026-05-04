# Changelog

All notable changes to protoCore are documented in this file.

## [Unreleased] - 2026-05-04
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
- 150/150 protoCore tests pass.
- protoJS `tree_traversal`: 10/10 stable (was 0/5 crashing).

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
