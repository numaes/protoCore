# Changelog

All notable changes to protoCore are documented in this file.

## [Unreleased]
### Added
- **Park-only safepoint** — `ProtoContext::parkIfStopRequested()` parks the
  calling thread while a stop-the-world phase is requested and, unlike
  `safepoint()`, never submits the context's young generation, so cells held
  only in C++ locals stay safe; `ProtoContext::isStopRequested()` is the
  inline flag poll to put in front of it. The four copies of the park
  handshake (allocation poll, `safepoint()`, the thread refill path and the
  return from an unmanaged region) now share one internal helper,
  `parkUntilWorldResumes`. The heap-limit reclaim wait, which runs inside the
  allocator, and thread start now park without submitting.
  `ProtoSpace::runningThreads` and `parkedThreads` are documented as internal
  quorum counters that embedders must not write.
- **Unmanaged-region API for blocking OS calls** — new pair of methods on
  `ProtoThread` and `ProtoContext`: `goUnmanaged()` /
  `returnFromUnmanaged()`. Bracket a blocking OS call (`read`, `write`,
  `poll`, `sleep`, network I/O, third-party C library calls) with this
  pair and the GC's stop-the-world quorum will NOT wait for that thread
  to reach a real safepoint — the thread is pre-counted as parked in
  `ProtoSpace::parkedThreads` for the duration of the region. Without
  this, a single thread blocked in a slow syscall could pause every
  other thread in the process behind the GC quorum until the syscall
  returned. Calls nest (depth counter per thread; outermost pair manages
  the quorum slot). `returnFromUnmanaged()` blocks if a STW phase is in
  progress at the moment of return, matching the behaviour of a normal
  safepoint park. A `ProtoContext::UnmanagedScope` RAII helper
  guarantees the matching return runs on every exit path including
  exceptions. The per-thread counter lives on `ProtoThreadExtension`
  (not on the 64-byte `ProtoThreadImplementation` Cell). See DESIGN.md
  §"Unmanaged regions: blocking OS calls without blocking the GC" for
  the full contract — particularly the rule that NO `ProtoObject*`
  access is permitted while unmanaged.
- **Caller-supplied element hash for `ProtoSet`** — `ProtoSet::addWithHash`,
  `hasHash` and `removeHash` take the hash explicitly, and
  `ProtoSetIterator::nextHash` returns the hash an element is stored under,
  so set operations (union, intersection, subset tests) can combine sets
  without hashing elements again. This lets a runtime whose equality differs
  from protoCore's use `ProtoSet`: in Python, `1`, `1.0` and `True` are one
  set element and a class's `__hash__` decides membership. A set must be
  accessed through one hash function only.
- **`PROTOCORE_TRUST_SYMBOLS` environment variable (experimental)** — when the
  variable is set (to any value), `getAttribute`, `hasAttribute` and
  `hasOwnAttribute` no longer resolve a non-interned string name through
  `SymbolTable::lookupByContent`; they report the attribute as absent. It is a
  measurement aid for embedders that always pass interned symbols: on the
  protoJS standard benchmark suite it reduced times by 4–22%, while call sites
  that pass non-interned names read back `PROTO_NONE` and take slower fallback
  paths. Write paths (`setAttribute`, `setAttributeIfEqual`,
  `deleteAttribute`) still intern string names. The default behaviour is
  unchanged.
- **Attribute-cache benchmarks and statistics** — new benchmark targets
  `hash_quality_benchmark`, `cache_pressure_benchmark` and
  `mutable_access_benchmark`, and optional instrumentation of the attribute
  cache (hits, collisions and cold misses, printed at exit) compiled in only
  when `PROTO_CACHE_STATS` is defined.

### Changed
- **Concurrent mark via a per-cycle mutable-shard snapshot** — the GC mark
  phase now runs outside the stop-the-world window, concurrently with mutator
  threads. During root collection the collector loads each of the 256
  mutable-shard roots into the new `ProtoSpace::gcMutableSnapshot[]` table,
  and the marker consults only that snapshot. Because every mutation goes
  through a shard-root compare-and-swap and cell fields are immutable after
  construction, the snapshot takes the place of a write barrier: mutators pay
  no per-write cost. The pause is bounded by root collection plus the 2 KB
  snapshot capture, independent of heap size and live-object count; floating
  garbage survives at most one extra cycle. Mark bits are now cleared by a
  bulk unmark over the marked list after sweep (Phase 6), replacing the
  pre-mark unmark pass introduced in 1.2.0, and Phase 7 clears the snapshot.
  Root collection also no longer iterates the unused `stringInternMap`, and
  interned tuples are traced by the concurrent mark instead of inside the
  pause. See `docs/GarbageCollector.md` § "Concurrent Mark Without Barriers".
- **`ProtoSpace` layout** — `ProtoSpace` gains the `gcMutableSnapshot[]` table,
  a `tupleInterner` pointer, and the collection-pacing members
  `gcGrowthPercent`, `gcMinBudgetCells`, `gcAllocationBudget` and
  `cellsSinceLastCycle`. Embedders built against 1.2.0, or against an earlier
  build of this release, must be rebuilt; no source change is required.
- **No GC trigger on free-list exhaustion without a heap limit** —
  `getFreeCells` refills from the OS without waking the collector when the
  freelist runs out. Previously every exhaustion woke the collector, which
  then waited in the stop-the-world quorum for threads that never reached a
  safepoint: in a protoST actor benchmark with 8 workers it waited 2.27 s of a
  2.49 s run and swept once, at the end. (This change left programs with no
  heap limit without any automatic collection; see the allocation-budget
  trigger under "Fixed".)
- **`toImpl` debug checks are compiled out of release builds** — the
  embedded-value tag, expected-tag and 64-byte alignment checks in `toImpl<>`
  now run only when `NDEBUG` is not defined, so release builds reduce the
  conversion to a null check and a mask. In a release build, a wrong-typed
  conversion that used to abort with a diagnostic is now undefined behaviour.
  The protoST `fib` benchmark went from about 500 ms to about 440 ms (median).
- **Install and packaging rules only in a top-level build** — when protoCore
  is included through `add_subdirectory()`, its install and CPack rules no
  longer run as part of the parent project's package. The RPM package license
  is declared as MIT.
- **Documentation restructure** — design specifications moved to
  `docs/archive/design-specs/` and the step-by-step implementation plans were
  deleted; five dated analyses moved to `docs/archive/` with a banner stating
  that their "production ready" assessment is superseded; `DOCUMENTATION.md`
  and `docs/README.md` are now indexes of the current documentation.
  `docs/GarbageCollector.md` documents the concurrent mark, the anatomy of the
  stop-the-world pause and a comparison with other production collectors, and
  `docs/STW_ELIMINATION_RESEARCH.md` records the research on bounding the
  pause. The README lists protoClojure and protoCpp in the ecosystem.
- **Repository hygiene** — build trees, in-source CMake and test outputs,
  static libraries and benchmark executables, generated Doxygen XML, a Python
  virtualenv, performance logs, IDE settings and unreferenced media files are
  no longer tracked and are ignored by `.gitignore`.

### Removed
- `ProtoThread::setManaged()` / `setUnmanaged()` — declared but never
  implemented and without callers; superseded by the unmanaged-region API.
- The unmaintained Sphinx documentation site (`docs/conf.py`, the `.rst`
  pages, `docs/Makefile`, `docs/make.bat` and the Doxyfiles under `docs/`).
  API reference generation uses the root `Doxyfile`. Guides and tutorials that
  described APIs and tools that do not exist were removed as well.

### Fixed
- **The collector no longer dereferences a null work-list entry** — the
  concurrent mark crashed with a segmentation fault at address 0x8 (reading
  `Cell::next_and_flags` of a null `Cell*`, `gcThreadLoop+0xdf8`), seen as
  0.2–0.3 % of protoPython runs with `PROTOCORE_GC_MIN_BUDGET_CELLS=4096`.
  `ProtoThreadExtension::processReferences` read every per-thread cache slot
  twice, once for `ProtoObject::isCellPointer` and once for `asCellPointer`,
  while the owning thread kept rewriting the slot; a slot that changed to
  `nullptr` or an embedded value between the two reads was reported as a null
  child and pushed on the work list. Every `processReferences` implementation
  now loads each reference field once (`if (const Cell* c =
  ProtoObject::asCellPointer(field)) method(ctx, self, c);`), which also stops
  a tagged null (a non-embedded tag with no pointer bits) from being reported.
  The mark loop skips null entries at its single pop site, which also covers
  roots holding a tagged null. In builds with `PROTOCORE_GC_INSTRUMENT` or
  without `NDEBUG`, a `processReferences` that reports `nullptr` aborts with a
  message naming the reporting cell's type.
- **Per-thread caches are stop-the-world roots** — the concurrent mark no
  longer reads the per-thread attribute cache and mutable-value cache, which
  their owning threads rewrite at any time. `gcThreadLoop` pushes the cells
  of every registered thread's caches during the Phase 2 root scan, while
  the owners are parked, and `ProtoThreadExtension::processReferences`
  reports nothing. The mark phase again reads only immutable fields, which
  restores the documented "Concurrent Mark Without Barriers" invariants.
  The stop-the-world pause gains a fixed scan of 5,120 field loads per
  thread. Embedder caches that hold cell pointers must be kept alive through
  a root captured under stop-the-world or be dropped when
  `getGCCycleCount()` changes (`docs/GarbageCollector.md`).
- **`ProtoList::has` with integers beyond `long long`** — both list forms
  compared integer elements with `asLong`, which throws
  `std::overflow_error` for a `LargeInteger` outside that range, so `has`
  threw on a list holding such an element (or when asked for one). Integers
  are now compared by value with `Integer::compare`, as `ProtoTuple::has`
  already did.
- **Destroying a space no longer waits for a full collection** — once
  `~ProtoSpace` marks the space ENDING, the GC thread abandons a cycle in
  progress (checked after resuming the world, every 4096 mark pops and every
  1024 sweep segments) instead of marking and sweeping a heap nothing will
  reuse; unswept segments are returned for the destructor to free. Before, a
  cycle that started near the end ran to completion and the process waited
  for it. In addition, when ENDING interrupted the stop-the-world handshake,
  the collector left `stwFlag` raised, so any mutator still running parked
  forever at its next safepoint; the flag is now lowered before the GC thread
  exits.
- **The allocation budget is charged for cells consumed, not batches handed
  out** — `getFreeCells` charged each refill batch to the GC allocation
  budget when a thread received it. With several threads running a batch is
  65,536 cells, so every thread that allocated a single cell was charged a
  whole batch: eight idle protoST workers cost 524,288 cells, half the
  default 1,048,576-cell budget, and sixteen protoClojure workers the whole
  budget, starting cycles in programs that produced almost no garbage (protoST
  `parallel_speedup` ran at about twice its wall time). A ProtoThread's batch
  is now charged when the thread exhausts it and refills, and at thread exit
  the used part is charged (`settleThreadCells`). Cycles per allocated cell no
  longer depend on the number of threads; the charge lags allocation by at
  most one batch per thread. Contexts without a thread are still charged at
  hand-out. **ABI:** `ProtoThreadExtension` gains `heldBatchCells` (its cell is
  now 64 of 64 bytes) and `ProtoSpace` gains the overload
  `getFreeCells(ProtoContext*, ProtoThreadExtension*)`; embedders must be
  rebuilt.
- **Exiting threads return their unused cells** — a `ProtoThread` allocates
  from a private freelist that is refilled in batches of up to 65,536 cells,
  and the unused part of its last batch was never returned when the thread
  exited: those cells were on no freelist and in no young generation, so no
  cycle could reclaim them. 64 sequential threads that allocated one object
  each grew the heap by 2,228,224 cells. `thread_main` now splices the
  remainder onto the global freelist (`settleThreadCells`) after the thread's
  last allocation.
- **An exiting `ProtoThread` stays in the stop-the-world quorum until it is
  unregistered** — `thread_main` decremented `runningThreads` before it
  rebuilt `space->threads`. That rebuild allocates, and in builds with
  `-DPROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` the allocation can park on a
  pending stop-the-world request, so the exiting thread counted as parked
  but not as running and the quorum could be met while another mutator was
  still running. The decrement now happens under `globalMutex` in the same
  critical section that publishes the new thread list, followed by the
  `gcCV` notification that previously ran without the mutex.
- **The GC cycle counter advances in every build configuration** — the only
  increment of `gcCycleCount` sat inside the `PROTOCORE_GC_REINCLUDE_SURVIVORS`
  block, so with `-DPROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` `getGCCycleCount()`
  stayed at zero and heap-limit reclamation waits, which wait for the counter
  to advance, never saw a cycle complete. The counter is now incremented for
  every cycle; the survivor re-chain still uses the same cycle number to decide
  fold cycles. The heap-growth trigger tests now run in both configurations.
- **GC cycles start from allocation when no heap limit is set** — without a
  hard heap limit (the default), nothing started a collection unless the
  embedder called `triggerGC()`, and no embedder in the ecosystem did, so the
  heap grew by every cell allocated. In a probe that allocated garbage with a
  constant live set of 1,000 strings, the heap reached 257 million cells
  (15.3 GiB) with zero cycles. `getFreeCells` now charges every batch it hands
  out against an allocation budget and requests a cycle through the existing
  GC-thread wake-up once the cells handed out since the previous cycle reach
  `max(PROTOCORE_GC_MIN_BUDGET_CELLS, retained × PROTOCORE_GC_GROWTH_PERCENT
  / 100)`. `retained` is `heapSize − freeCellsCount` at the end of the cycle,
  minus the cells handed out while it ran. A cycle is requested only if
  contexts have submitted garbage since the previous cycle. Cells still owned
  by a live context can never be reclaimed, and cycles started without
  submissions only re-scanned them under stop-the-world: in
  `immutable_sharing_benchmark` four such cycles reclaimed nothing, and their
  stop-the-world root collection grew to 439 ms. The defaults are 1,048,576
  cells (64 MiB) and 100%; `PROTOCORE_GC_GROWTH_PERCENT=0` restores the
  previous behaviour. The same probe now holds the heap at 3.9 million cells (240 MiB)
  across 102 cycles and finishes in 5.9 s instead of 14.5 s. The check runs
  only on the refill path. It is inactive while a hard heap limit is set, and
  the per-thread allocation fast path is unchanged. A requested cycle still
  needs every running thread to park, so loops whose allocations all run
  inside critical sections must call `ProtoContext::safepoint()`.
- **String hashes depend on content, not rope shape** — `getHash` on a heap
  string returned a cached hash that mixed its children's hashes, so equal
  content split differently by concatenation hashed differently (a 47-byte
  literal and the same text built as 37 + 10 bytes). Every structure keyed by
  `getHash` missed equal strings, including sparse lists and tuple hashes.
  Heap strings now hash with the structure-independent FNV-1a content hash
  that string interning already uses, and inline strings use the same FNV-1a
  over their bytes, so a string hashes the same whether it is inline or
  heap-backed.
- **`LargeInteger` hashing covers every digit** — the hash used only the
  lowest digit, so every multiple of 2^64 collided and hash-keyed structures
  kept a single entry for 2^64, 2^65 and 2^70.
- **`ProtoObject::asDouble` on large integers** — no longer throws for a
  `LargeInteger` beyond the range of `long long`; the value is converted
  through its decimal digits, and out-of-range values become ±infinity.
- **Exact integer/double comparison** — `ProtoObject::compare` converted the
  integer to `double`, which threw beyond `long long` and rounded above 2^53
  (`2**70 + 1` compared equal to `2.0**70`). The integer is now compared with
  `floor(d)` as integers. NaN compares as before; ±infinity is handled
  explicitly.
- **`objectPrototype` is mutable** — `ProtoSpace::objectPrototype` was created
  immutable, so the first `setAttribute` on it returned a new object and left
  every previously stored reference stale (in protoJS,
  `Object.prototype.foo = 1` was silently lost). `getPrototype` on
  `objectPrototype` itself now returns `nullptr` instead of looping back to
  itself, and `ProtoObject::clone(ctx, true)` now returns a mutable clone (the
  flag was ignored). Across ten protoJS test262 families, passing tests went
  from 8397 to 8451 of 9823.

### Performance
- **Tuple interning uses a sharded hash table** — the interner was a binary
  search tree under `ProtoSpace::globalMutex` that never rebalanced and, keyed
  by allocation-ordered pointers, degenerated into a linked list walked twice
  per tuple creation. `TupleInterner` uses 64 shards, each with its own hash
  index and a mutex that is never held across an allocation; lookup runs
  before allocation. Interned tuples remain perennial. Interning 100,000
  distinct tuples timed out after 120 s before and now takes 0.03 s, as does
  the 4-thread concurrent test (previously 82 s).
- **`getOwnAttributeDirect` consults the attribute cache** — it resolved the
  mutable snapshot and then walked the attribute tree on every call; it now
  checks the per-thread `(snapshot, name)` attribute cache first, like
  `getAttribute`. No behavioural change.
- **Better attribute-cache slot distribution** — cell pointers are 64-byte
  aligned, so their low 6 bits are always zero; the cache index now shifts
  them out. `object_access_benchmark` used 6.6% fewer cycles and
  `hash_quality_benchmark` 16% fewer.
- **Attribute-cache entries padded to 32 bytes** — two entries now fit in one
  64-byte cache line and the table is allocated 64-byte aligned, at a cost of
  8 KB per thread. `object_access_benchmark` used 3.6% fewer cycles with 27%
  fewer L1 data-cache misses.

### Tests
- `ConcurrentMarkSafety.ThreadCacheSlotFlipsDuringMark` flips the main
  thread's attribute-cache and mutable-value-cache slots between a cell and a
  non-cell from a helper thread while cycles run every few thousand cells; it
  crashed within about 25 ms before the null work-list fix.
  `GCMark.NullReferenceIsSkipped` pins a tagged null in a root set (it crashed
  before the fix), and
  `GCMarkDeathTest.NullReferenceFromProcessReferencesIsReported` checks that
  instrumented and debug builds name a cell type whose `processReferences`
  reports `nullptr`.
- `ListTest.HasComparesLargeIntegersByValue` checks `has` on inline and AVL
  lists holding 2^70 against an equal distinct object, neighbours and small
  integers (it threw `std::overflow_error` before the fix).
- `ParkOnly` (three cases): `isStopRequested` follows the flag; 256 young
  lists held only in C++ locals survive four cycles that stop the world while
  the thread parks through `parkIfStopRequested` with a 16-cell submission
  threshold (the young chain is not submitted); `safepoint()` submits where
  `parkIfStopRequested()` does not.
- `GCShutdown.CycleStartedWhileEndingSkipsMarkAndSweep` ends the space during
  a cycle's root collection and checks that 20,000 garbage external pointers
  are not finalized (all were before the fix);
  `GCShutdown.EndingDuringStopTheWorldHandshakeLowersTheFlag` ends the space
  while the handshake waits and checks that `stwFlag` is lowered and a
  following safepoint returns (it blocked forever before).
- `GCHeapGrowthTriggerTest.IdleThreadBatchesDoNotSpendTheBudget` (eight
  threads take a batch each and go idle; no cycle may start; it failed before
  the charge-on-consumption fix), `ManyThreadGarbageStillBoundsHeap` (eight
  threads, 8 million cells of garbage, at least three cycles and growth below
  a quarter of the allocation) and `ShortLivedThreadsAreCharged` (128 threads
  that each allocate less than one batch start cycles and keep the heap below
  half the allocation). The multi-thread tests wait while a cycle is pending,
  so they test pacing rather than collector throughput.
- `ThreadLifecycle.ShortLivedThreadsDoNotGrowTheHeapByABatchEach` runs 64
  sequential threads that allocate one object each and bounds the heap
  growth to four batches (2,228,224 cells before the fix).
- `ThreadLifecycle.ExitingThreadStaysCountedUntilUnregistered` raises the
  stop-the-world flag while a thread exits and checks that the quorum is
  never met while the test thread runs; it failed before the fix in the
  survivor re-chain OFF build.
- `ConcurrentMarkSafety.AttributeCacheChurnWithSmallBudget` writes and reads
  attributes on fresh objects through the public API for three seconds with
  a 4096-cell budget, then checks a pinned live set.
- `GCHeapGrowthTriggerTest` (five cases): with no heap limit, garbage
  allocation with a constant live set starts cycles and keeps the heap bounded
  without collecting live objects. The main case failed before the fix with 0
  cycles and 15,990,784 cells of growth. `PROTOCORE_GC_GROWTH_PERCENT=0`
  disables automatic cycles, malformed values fall back to the defaults, the
  budget scales with the retained cells, and a spent budget waits until
  garbage has been submitted.
- New suites and cases: `ConcurrentMarkSafety` (mutation during mark, no lost
  mutable references), `UnmanagedRegionTest` (six cases), three `TupleTest`
  interning cases, `StringTest.EqualContentHashesEqualWhateverTheRepresentation`,
  `SetTest.ExplicitHashKeysElementsByTheCallersHash` and three `NumericTest`
  cases for large-integer hashing, conversion and comparison.
- `SwarmTest.OneMillionConcats` and `SwarmTest.LargeRopeIndexAccess` are
  enabled again (the rope GC failures they were disabled for no longer
  reproduce), and `SwarmTest.LargeRopeIndexAccessUnderHeapPressure` builds a
  large rope under a tight heap limit so reclamation runs during
  construction.

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
    and `docs/archive/design-specs/2026-05-22-allocation-limit-oom-design.md`.

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
  (`ProtoString::createSymbol` is unchanged). See `core/SymbolTable.cpp` and
  DESIGN.md § "Mechanism A — Perpetual allocation via `ProtoContext* = nullptr`".

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
  clean tricolor invariant at the start of every cycle. This pass was later
  replaced by the bulk unmark of the concurrent mark (see the concurrent mark
  entry above); its `docs/GarbageCollector.md` section no longer exists.

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
- **String Handling Refactor**: Introduced **Inline Strings** (up to 6 UTF-8 bytes) stored directly in `ProtoObject*` tagged pointers.
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
