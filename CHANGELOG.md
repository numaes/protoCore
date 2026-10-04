# Changelog

All notable changes to protoCore are documented in this file.

## [Unreleased]

## [2.14.2] - 2026-10-04

No public API change; ABI compatible (SOVERSION 3).  The four collector
adjustments approved after the [final re-measurement](docs/reports/2026-10-04-final-remeasurement.md)
(its section 6); evidence and data in
[docs/reports/2026-10-04-collector-adjustments.md](docs/reports/2026-10-04-collector-adjustments.md).
Synthetic workloads, medians of 3, one notebook-class CPU (Ryzen 5 5500U).

### Changed

- **The sweep walks one chain for a single allocating thread that has its
  runway** (A1).  On 2.14.1 the 8-chain walk made protoClojure
  `coll_alloc` with one task 29 % slower than on 2.10.2 (11.25 -> 14.52 s
  in this session): the sweep ran far ahead of the lone mutator, whose
  allocations then wrote cells that had gone cold or sat modified in the
  collector's cache (11 G more mutator cycles and 33 M more DRAM fills;
  +26 to +54 % on every placement of the two threads).  The walk is now
  wide while a thread waits for headroom, when more than one thread
  allocated since the last cycle (`getFreeCells` notes the thread of each
  refill), when the cells left before the ceiling are below pacing's
  runway or two free chunks per running thread, or with
  `PROTOCORE_GC_SWEEP_ENGAGE=always`; one chain otherwise
  (`sweep::cursorsFor`, `sweep::mutatorsShort`).  `coll_alloc` with one
  task: 14.52 -> 11.99 s (2.10.2: 11.25).  Multi-threaded workloads walk
  wide as before.  The measured engagement of the helpers compares against
  a wide sweep without them, and a cycle whose sweep walked one chain
  leaves the adaptive controller the throughput of the last wide sweep.
  The instrumented build prints `wide_segments` and `narrow_segments`.
- **Adaptive controller: the live-set floor** (A3).  After a cycle that
  reclaimed fewer cells than its live set -- the first cycle included,
  which the law otherwise skips -- the soft limit is raised to at least
  `min(H, 2 L + runway)`, by at least a doubling (`adaptive::liveSetFloor`).
  A program whose start-up live set sits just below S0 (protoST: 1.7 M
  cells against 2 M) ran a futile first cycle, kept S at S0, and ran the
  next one after a thin slice of allocation.  protoST `fib.st` under the
  controller: see the report (it remains slower than with protoST's fixed
  limit: the start-up cycle and a cycle waited for at exit).

### Fixed

- `softHeapLimit` is stored with a relaxed atomic store (`setHeapLimits`,
  the controller): the sweep now reads it without the lock.

### Not changed, with evidence

- **Pacing** (A2).  The re-measurement attributed +1-6 % at six threads
  and 40 M cells to pacing's early starts.  In this session 2.14.1 was not
  slower than 2.10.2 there (`doctree` 9.85 -> 9.69 s, `wordfreq` 7.60 ->
  7.29 s), removing the early starts made those runs 2-12 % slower, and a
  shorter runway (until the sweep's first cells are back) ran 25-40 % fewer
  cycles but was up to 13 % slower and let waits back in on a CI runner.
  The runway is 2.14.1's.
- **`immutable_sharing_benchmark` +8.5 % cycles** (A4) is code layout: the
  same 4.574 G instructions, the hot functions identical in size and moved
  across 64-byte boundaries, 41 times more switches between the op cache
  and the decoder; with every function aligned to 64 bytes 2.14.1 runs in
  2.996 G cycles against 2.10.2's 3.023 G.  No code change.

## [2.14.1] - 2026-10-03

No public API change; ABI compatible (SOVERSION 3).  One test-only symbol
is added to the internal header.

### Fixed

- **An exiting thread could be walked by a stop-the-world it was not part
  of** (GitHub issue #3).  `thread_main` left the stop-the-world quorum
  (`runningThreads`) as soon as the thread's body returned, and only then
  rebuilt `space->threads` without the thread, allocating in its root
  context.  In that window a cycle could stop the world without the thread,
  scan its context and capture its young chain while it ran, and the
  concurrent young-chain walk read cells the rebuild was still constructing;
  an allocation in the window could also park the uncounted thread and let
  it stand in for a running one in the quorum count.  ThreadSanitizer
  reported it in about 3 of 40 runs of
  `GCRootScope.CandidateReachableOnlyFromAYoungCellSurvivesACycleForcedAtOnce`
  since at least 2.11.0.  The thread now leaves the quorum in the same
  `globalMutex` critical section that publishes the threads list without
  it.  New test `ThreadExitQuorum.AThreadStillInTheThreadsListHoldsTheStopTheWorld`
  (failed on every run before the fix) drives the window through a
  test-only hook in the internal header, `threadExitHook`, null in every
  build.

## [2.14.0] - 2026-10-03

Additive API; no ABI break (SOVERSION 3).  Milestone 4 of the
collector-throughput design ([spec](docs/specs/2026-10-03-collector-throughput-design.md)
§ 6 and decisions 5-6; [report](docs/reports/2026-10-03-collector-throughput.md),
"M4" and "Hardware class").  Synthetic workloads, median of 3, one
notebook-class CPU.

### Added

- **Helper threads for the sweep.**  While mutators wait for heap
  headroom, up to K helper threads of one process-wide pool sweep beside
  the collector thread, claiming runs of 128 segments; each sweeper keeps
  and publishes its own chunk, segment chains and finalized refs; the
  collector merges the refs before Phase 5b and runs the embedder
  (`ProtoExternalPointer`) finalizers the helpers left, one at a time on
  its own thread.  Built-in finalizers may run on helpers.  Helpers are
  protoCore-owned `std::thread`s (not `ProtoThread`s), never allocate,
  start lazily, stop with the last space, and a forked child gets a fresh
  pool.  One sweep at a time; a collector that finds the pool busy sweeps
  alone.  Mutex and condition variables only.
  - K defaults to half the physical cores of one NUMA node (3 on a 6-core
    notebook); `PROTOCORE_GC_SWEEP_THREADS` (0..64) and
    `ProtoSpace::setCollectorHelperThreads` set it; 0 is the serial sweep
    with no pool.
  - **Measured engagement** (default): helpers are kept only while they
    shorten the sweep; otherwise held back for 1, 2, 4 ... 64 sweeps.
    `PROTOCORE_GC_SWEEP_ENGAGE=measured|waiting|always`,
    `ProtoSpace::setCollectorHelperEngagement`.
  - **Hardware-sensitive parameters configurable**: the chains walked in
    lockstep (`PROTOCORE_GC_SWEEP_CURSORS`, default 8,
    `ProtoSpace::setSweepCursors`) and the prefetch
    (`PROTOCORE_GC_SWEEP_PREFETCH`, `ProtoSpace::setSweepPrefetch`).
  - `PROTOCORE_HAS_COLLECTOR_HELPERS`.
- **Measured**: with K = 3, the sweep's cost per cell fell 2.3-2.6x where
  the sweep is the bottleneck (protoJS `records` N = 12: 29 -> 12.5 ns),
  wall time -6 to -18 % on protoJS N = 12 workloads, -4 % / +6 % on
  protoClojure / protoScala t6, single-threaded unchanged; process CPU -2
  to +11 %.  K = 0 is 2.12.0's sweep within noise.  On aged heaps (all
  recycled memory): protoJS `records` N = 12 71.0 s (2.11.0) -> 31.0 s,
  protoClojure t6 117.1 -> 46.4 s.
- `test/ParallelSweepTests.cpp` (20 cases: claiming, equivalence with the
  serial sweep for every width/prefetch/helper setting, no mark bit left,
  embedder finalizers on the collector thread and never overlapping,
  mutable refs merged before Phase 5b, a helper that allocates aborts,
  teardown during a sweep, both destruction orders, pool stop and restart,
  exit with a live space, several spaces, fork, configuration, the
  engagement rule), and the GC suites re-run as ctest entries with three
  helpers engaged on every sweep.
- `.github/workflows/sweep-hardware.yml`: the sweep's tests and benchmark
  on arm64 Linux, macOS arm64 and x64 runners (informational).
- Compiled-out instrumentation: cells handed out from fresh OS blocks
  against recycled chunks.

### Fixed (before release)

- With K = 0 the first version of the parallel sweep was 8-70 % slower
  than 2.12.0's (its cursors drained at every claimed run; a claim walked
  128 links in a row); and ThreadSanitizer found an unlocked read of the
  shared list's head.  All three fixed and measured (report, M4).

### Changed

- `AdaptiveHeapFastAllocator.SoftLimitGrowthIsBoundedByTheBudgetAndTheProbes`
  gates on invariants only and prints the change count (it depended on
  sample noise).

## [2.13.0] - 2026-10-03

No ABI change (SOVERSION 3).  Milestone 3 of the collector-throughput
design ([spec](docs/specs/2026-10-03-collector-throughput-design.md) § 4.5
and the decisions of § 14;
[report](docs/reports/2026-10-03-collector-throughput.md), "M3").

### Changed

- **The adaptive heap controller minimises the mutators' waits within the
  budget**, from measurements only, and the 2.10.1 fitted constants
  `k_live`, `k_cap` and `p_high` are gone.  At each cycle end: the live set
  L, the allocation rate r while not waiting, the collector's reclamation
  throughput T and the wait share w.  While the mutators wait and
  rho = r / T < 1, S becomes `L + rho L / (1 - rho) x 1.25`, the headroom a
  cycle needs to run behind them; while they wait and rho >= 1, S doubles as
  long as each doubling reduces the waits and stops after two that do not; a
  change of L or r by a factor of 2 re-arms growth, and voids a probe whose
  verdict it straddles.  The first cycle under the controller is not used.
  S never decreases and never exceeds H.  Without waits S does not change.
- **Measured** (synthetic, median of 3): under the controller the wait
  share fell to a third on two protoJS N = 12 workloads (24 -> 9 %) and the
  wall time stayed within +0.4 to +7 %: the time went to collector CPU.
  Against fixed limits of 10-200 M cells (one run per point) the controller
  uses 0.55-1.4 GB where large limits take 4-13 GB and matches the fixed
  limit at its knee; it is slower (up to 40 %) only where a large fixed heap
  never collects.  **The controller may now use more memory than 2.10.1's**
  (the single-threaded benchmark: 531 -> 858 MB): memory below the budget
  that removes a wait is used.
- `AdaptiveHeapStats::lastPressure` reports the wait share w.
  `AdaptiveHeapConfig::highPressure`, `growthFactor` and `liveHeadroom`
  are ignored (kept: the struct's layout is ABI).

### Added

- Start at the budget, for a run known to fit (decision 2: the controller
  never grows there on speculation):
  `AdaptiveHeapConfig::initialSoftCells` at or above H, or
  `PROTOCORE_ADAPTIVE_HEAP_START=budget`.
- `PROTOCORE_HEAP_TRACE` prints w, r, T, rho, the probes and whether growth
  stopped.
- Tests: the law as a pure function (regime 1 in one step; rho towards 1
  bounded by H; doubling while waits fall; two non-improving probes stop
  growth, one does not; a probe across a workload change is void; a factor
  of 2 re-arms; 200 random runs never decrease, never exceed H and change
  at most log2(H / S0) + 1 times); on real spaces, the budget at every heap
  growth with two spaces (a new peak tracker), bounded changes under a fast
  allocator and a storm, start at the budget.  The expectations tied to the
  removed constants are replaced (listed in the commit).
  `AdaptiveHeapRate.SteadyAllocatorBelowCapacityStopsWaiting` (P1) is
  clock-dependent.
- The memory-as-only-variable experiment (`docs/reports/data/.../matrix.py`).

## [2.12.0] - 2026-10-03

No API or ABI change (SOVERSION 3), no change to the object model.  The
collector-throughput design, milestones 1 and 2
([spec](docs/specs/2026-10-03-collector-throughput-design.md), approved
2026-10-03; [report](docs/reports/2026-10-03-collector-throughput.md)).
Every number below is from synthetic workloads, median of 3.

### Changed

- **A wait for heap headroom ends when cells arrive.**  Every publication
  of free cells wakes one waiting thread, and the wait also ends on a
  non-empty freelist.  Before, a wait that began inside a running cycle
  lasted until the 50 ms watchdog: the mean wait was 48.6-50.2 ms on every
  multi-threaded run.
- **Pacing under fixed limits and the controller.**  Every space measures,
  at each cycle end, the mutators' allocation rate r and the cycle duration
  C from request to completion; a refill requests a cycle once the cells
  left before the ceiling fall below `min(headroom, r x C x 1.25)`.  A
  fixed limit requested a cycle only at the ceiling, so the mutators stopped
  for the whole cycle even when the collector kept up: on the protoJS
  N = 6 runs at 40 M cells the wait share fell from 19-21 % to 3 %.  This
  replaces the controller's fixed quarter of S - L.  Pacing starts cycles
  with less garbage, so there are more of them (5 -> 8 on one workload,
  where that ate the gain).  `PROTOCORE_GC_PACING=0` turns pacing and the
  early wake off (diagnosis).
- **The sweep walks 8 segment chains in lockstep, with prefetch.**  The
  per-cell cost under concurrent allocation was memory latency: about one
  serialized DRAM miss per swept cell (IPC 0.08 on the collector thread).
  Sweep cost per cell: 139 -> 46 ns (protoClojure, 6 tasks),
  84 -> 29 ns (protoJS `records`, 12 threads), 157 -> 91 ns (protoScala,
  6 tasks); wall time -63 %, -43 %, -26 %.  On a quiet machine with
  freshly built garbage the micro-benchmark shows about +2 ns per cell.
- **The sweep hands processed segments back in batches** (one
  compare-and-swap per 1,024 segments, and one per sweep for the survivor
  pen, instead of one per segment of about 6 cells), **unmarks and relinks
  a survivor with one store**, and **Phase 6 tests a mark bit before
  clearing it**.  Together about 10 % of the per-cell cost.
- The collector thread is named `protocore-gc` on Linux.
- `PROTOCORE_HEAP_TRACE` prints the pacing signals (r, C, runway) and, for
  fixed limits, the headroom-wait counters.

### Fixed (before release)

- With the early wake, the controller's soft-zone checkpoint returned at
  once while its pending flag stayed set, so every outermost critical
  section took `globalMutex` until the cycle ended: about 2 million empty
  waits per run, the single-threaded adaptive benchmark 4.2 -> 14.5 s.  A
  publication of cells now clears the flag (2.66 s).

### Added

- `test/CollectorPacingTests.cpp`: the pure runway function; a wait at the
  ceiling ends on a publication of cells (wake-reason counters); fixed
  limits request cycles before the ceiling; `PROTOCORE_GC_PACING=0`
  restores the old behaviour, which is also how these cases were seen to
  fail.  `CollectorPacingRate.*` (a steady allocator below capacity stops
  waiting) is clock-dependent and joins `CLOCK_DEPENDENT_TESTS`.
- `performance/sweep_contention_benchmark`: the sweep's ns per cell under
  the conditions that separate the spec's hypotheses (loaded DRAM, the
  shared segment pool, other cores' cache lines, readers of survivors,
  fresh against recycled memory).  Self-verifying.
- Always-on cycle measures (pause, mark, sweep, busy time, swept and freed
  cells): three clock reads per cycle and a counter per swept cell.
- The compiled-out per-phase instrumentation of `measure/gc-phases`
  (`-DPROTOCORE_GC_INSTRUMENT=ON`, `[GC-PHASES]`).

### Measured, not changed

- Single-threaded protoCore benchmarks: identical instruction counts;
  cycles -1.2 % to +1.8 % on four of six, `hash_quality` +3.5 to +6.8 %
  (150 ms; its hot code is unchanged: layout).
- The parallel sweep's gate (per-cell cost at 6-12 threads within 1.5x of
  one thread) is not met (2-3x): the helper threads come next.

## [2.11.0] - 2026-10-03

Additive API; no ABI break (SOVERSION 3), no change to the object model.

### Added

- **`ProtoObject::setAttributes(context, count, names, values)`**: a group of
  attribute writes published as one version.  `o.f1 = a; o.f2 = b; o.f3 = c`
  on a mutable object is three publications into the mutable table (three
  snapshot cells, three shard-root path copies, three compare-and-swaps).
  `setAttributes` is the same program in immutable style: read the current
  snapshot once, derive the new version from it, publish it once.
  - The result is what the chained `setAttribute` calls produce, in array
    order: a later entry for the same name wins, a `nullptr` value removes
    the name, a `nullptr` name is skipped, heap-string names are interned.
  - Immutable receiver: answers the new version (identical to the chained
    calls).  Mutable receiver: one compare-and-swap on the shard root,
    answering `this`; on a lost race the whole group is reapplied onto the
    newer snapshot (every entry is "set to value", so the retry is correct
    and other writers' names are kept).  The group is atomic to other
    threads: a reader sees all of it or none of it.
  - The new attribute tree is built in one pass
    (`sparse_avl::setSorted`, the update counterpart of the collector's
    join-based `removeSorted`): each changed path is copied once, and keys
    that land in an empty subtree are built into a balanced subtree
    directly instead of through every intermediate tree.  Measured, attribute
    tree plus the new object cell, on an immutable receiver:

    | existing attributes | writes | chained `setAttribute` | one-pass `setAttributes` |
    |---:|---:|---:|---:|
    | 0 | 5 | 19 cells | 6 cells |
    | 0 | 10 | 48 | 11 |
    | 20 | 5 | 29 | 17 |
    | 100 | 5 | 32 | 19 |

    Building the same tree by chaining `implSetAt` inside the call (one
    publication, intermediate trees kept) measured 18, 48, 23 and 28 cells
    for those rows, so the one-pass build is kept.  On a mutable receiver
    the saving adds one snapshot cell and one shard-root path copy per name
    after the first: a fresh 5-field mutable object costs 52 cells written
    field by field and 16 with one group.
- `test/SetAttributesTests.cpp` (12 cases): equivalence with the chained
  calls (including 1,500 random groups with removals and repeated names,
  each checked against the AVL invariants), exactly one publication per
  group, concurrent writers on one object from several protoCore threads
  with snapshot readers (no lost update, no partial group seen; the
  sequential form shows about 200,000 partial groups in the same test), and
  fresh values surviving collection cycles forced during the calls.

## [2.10.2] - 2026-10-03

Test-only release: the library is unchanged from 2.10.1 (SOVERSION 3).
Report: [docs/reports/2026-10-03-gcrootscope-macos-flake.md](docs/reports/2026-10-03-gcrootscope-macos-flake.md).

### Fixed

- **`GCRootScope.AllocationDuringConcurrentMarkIsSafe` failed about once in
  110 runs on macOS arm64** with one data mismatch.  The test, not the
  collector, was wrong: it built the young list that references the old
  list after the old list's context had died, so for the length of that
  allocation the old list was a candidate held only by a C++ local, which
  EMBEDDER-CONFORMANCE rule 3 forbids, and a cycle whose stop-the-world fell
  there freed it.  A probe that forces a cycle in that window fails on
  macOS arm64, Linux arm64, Linux x64 and under ThreadSanitizer, and passes
  on all four once the young list is built first.  The test, and
  `ObjectReachableOnlyThroughAYoungCellSurvives`, which had the same shape,
  now build the young list while the old list's context is alive: 0
  failures in 3,600 repetitions where macOS failed 9 times in 1,000.

### Added

- `GCRootScope.CandidateReachableOnlyFromAYoungCellSurvivesACycleForcedAtOnce`:
  the deterministic form of that test.  Two threads each force a whole
  cycle right after the context of an old list dies, so every old list is a
  candidate of a cycle that can reach it only through a young chain.

## [2.10.1] - 2026-10-03

Calibration of the adaptive heap controller on the runtimes' own programs.
No API or ABI change (SOVERSION 3); the controller is still opt-in, and the
runtimes have not adopted it.  Report:
[docs/reports/2026-10-03-adaptive-heap-calibration.md](docs/reports/2026-10-03-adaptive-heap-calibration.md);
design: section 7 of
[docs/specs/2026-10-02-adaptive-heap-controller-design.md](docs/specs/2026-10-02-adaptive-heap-controller-design.md).

### Changed

- **Pressure grows the soft limit only up to 8 x the live set** (or S0, or
  the floor).  In 2.10.0 a fast allocator took S to about 20 x the live set
  (1.2 GB against 672 MB under a 640 MB fixed limit on
  `adaptive_heap_benchmark` with a 1 M-cell live set): its stall was the
  collector's sweep throughput, which no soft limit removes, and each stall
  grew S by 1.5x.  Now 544 MB on that case (17 % slower than the fixed
  limit; 2.10.0 was 17 % faster).
- **Defaults:** `liveHeadroom` (k_live) 3 (was 1.5), initial soft limit
  128 MiB (was 32 MiB); `highPressure` 0.05 and `growthFactor` 1.5 are
  unchanged.  A configuration compiled against the 2.10.0 header passes
  `liveHeadroom = 1.5` explicitly and keeps it.
- **Pacing:** the next cycle is requested when a quarter of the headroom
  `S - L` is left (half in 2.10.0); fewer cycles for the same garbage.
- `PROTOCORE_HEAP_TRACE` lines carry the cycle's duration `Tc`, and spaces
  with fixed limits print a `fixed` line per cycle.

### Added

- `PROTOCORE_ADAPTIVE_HEAP=1` (diagnosis): every space enables the
  controller when it is created and `setHeapLimits` leaves it enabled, so an
  existing runtime binary can be measured under the controller against a
  development library without rebuilding it.

### Tests

- `AdaptiveHeapLaw.*` rewritten for the calibrated law, with new cases for
  the live cap (`PermanentStormStopsAtTheLiveCap`,
  `LiveCapIsNeverBelowS0NorTheFloor`, `NeverExceedsTheLiveCapThroughPressure`,
  `StormWithALargeLiveSetReachesH`, `DefaultsAreTheCalibratedOnes`).
- `AdaptiveHeapFastAllocator.SoftLimitStaysWithinTheLiveCap` (gating): a
  1.2 M-cell live set and 60 M cells of garbage at full speed; failed on
  2.10.0 (S = 30.2 M cells against a cap of 10.4 M).
- `AdaptiveHeap.AdaptiveOnEnablesTheControllerAtCreation`,
  `AdaptiveHeap.AdaptiveOnHonoursTheLimitVariable`.
- `AdaptiveHeapStorm.PressureFallsWithinBoundedCycles` became
  `SoftLimitSettlesWithinBoundedCycles`: pressure falls below p_high, or S
  reaches the live cap, within 60 cycles (clock-dependent list).
- `AdaptiveHeapSteady.HeapStaysFarBelowTheGarbageVolume` bound loosened from
  a quarter to half of the garbage volume (measured 13.6 % in ten runs; the
  2.10.0 bound failed once on macOS at 25.1 %).

## [2.10.0] - 2026-10-02

The adaptive heap controller: one call, `ProtoSpace::enableAdaptiveHeap()`,
lets protoCore size a space's heap instead of a fixed limit chosen by each
runtime.  New API, additive; the ABI is unchanged (SOVERSION 3: two new
structs, two new non-virtual members, no class layout changed).  Also a fix
for a deadlock at space destruction with several spaces.

### Adaptive heap controller

Design: [docs/specs/2026-10-02-adaptive-heap-controller-design.md](docs/specs/2026-10-02-adaptive-heap-controller-design.md)
(approved 2026-10-02; section 6 lists the implementation decisions).
Documentation: docs/GarbageCollector.md § "Adaptive heap controller".

- **API.**  `AdaptiveHeapConfig` (hard limit, initial soft limit, pressure
  threshold, growth factor, live headroom; every field has a default),
  `ProtoSpace::enableAdaptiveHeap(const AdaptiveHeapConfig& = {})`,
  `AdaptiveHeapStats` and `ProtoSpace::adaptiveHeapStats()`.  Feature macro
  `PROTOCORE_HAS_ADAPTIVE_HEAP`.  `setHeapLimits` keeps its meaning and
  disables the controller of its space.  Without the call nothing changes.
- **Soft limit S.**  Starts at 524,288 cells (32 MiB).  At the end of every
  cycle the collector applies the control law: S grows by 1.5x when the
  mutators lost more than 5 % of the interval since the previous cycle to
  collection (stop-the-world pause plus waits for a cycle), never falls
  below 1.5x the live set, never decreases, never exceeds H.  The law is a
  pure function (`core/AdaptiveHeap.cpp`) with deterministic unit tests.
- **Hard limit H.**  A safety cap on the sum of all spaces' heaps: 75 % of
  the smaller of physical memory and the process memory limit (cgroup v2
  `memory.max` / v1 `memory.limit_in_bytes` of the process's cgroup and its
  ancestors on Linux; the job object's limits on Windows; physical memory
  on macOS), clamped to `INT_MAX` cells (128 GiB) because cell counts are
  `int` in ABI 3.  `PROTOCORE_HEAP_LIMIT_CELLS` overrides it.
- **Pacing.**  A cycle is requested when the cells left before S fall below
  half of `S - L`, so the mutators keep a runway while the collector runs,
  and whenever the heap grows to S.  At S with an empty freelist a refill
  waits for the pending cycle, or, inside a critical section, takes one
  batch and waits at the thread's next critical-section checkpoint.
- **Out of memory** only when, after a full cycle, the cells it could not
  reclaim (plus the other spaces' heaps) exceed H less one refill batch per
  running thread, twice in a row; the callback and the controlled abort are
  unchanged.  Fixed limits keep the "two cycles reclaimed nothing" rule.
- **Environment.**  `PROTOCORE_HEAP_LIMIT_CELLS` sets H (and, with a soft
  part, S0) when the controller is enabled; `PROTOCORE_HEAP_TRACE=1` prints
  one line per cycle (`L`, `T`, `P`, `p`, S before and after, H,
  `heapSize`); `PROTOCORE_ADAPTIVE_HEAP=0` applies H as a fixed limit.
- **Tests.**  `AdaptiveHeapControlTests.cpp` (control law: steady, growing
  and storm sequences, monotonicity and the H bound over random sequences;
  cgroup v1/v2 parsing from a fake root, ancestors, containers, hybrid; the
  default H; a Windows job object, in a child process) and
  `AdaptiveHeapTests.cpp` (configuration and environment, a cycle at S with
  no waiting thread, steady and storm workloads, true out-of-memory, a
  retained set at 60 % of H with heavy garbage, the process budget across
  spaces and its return when a space dies).
  `AdaptiveHeapStorm.PressureFallsWithinBoundedCycles` asserts a ratio of
  measured times and joins the clock-dependent list in CI.
- **Measured** (`adaptive_heap_benchmark`, Release, Linux x86-64, three
  runs): with a 100,000-element live list and 200,000 calls of 100 objects,
  maximum RSS 237 MB, 30-31 cycles, 1.28-1.40 s with the controller, against
  646 MB, 5 cycles, 2.01-2.13 s under the 640 MB fixed limit runtimes used;
  with no live set 35 MB against 644 MB.  With a 1,000,000-element list the
  controller used more, 1.21-1.26 GB against 672 MB (2.5 s against 3.9 s):
  building the list stalled the program for 13-58 % of several intervals,
  so S grew to 20 x L.  Six benchmarks (`microbenchmark_final`,
  `mutable_access_benchmark`, `cache_timing_benchmark`,
  `hash_quality_benchmark`, `object_access_benchmark`,
  `immutable_sharing_benchmark`; `perf stat -r 3`, Release, against 2.9.6):
  user-space instructions +0.00 % on all six.  Cycles moved from -4.2 % to
  +5.8 % in both directions; an interleaved re-run put `mutable_access` and
  `cache_timing` at +3.5 % and `hash_quality` within noise.  With identical
  instruction counts this is code layout: every hot function moved (for
  example `getAttribute` from offset 48 to 0 within its cache line).
- **Runtimes** adopt it with `space.enableAdaptiveHeap()` in place of
  their own default limit (docs/EMBEDDER-CONFORMANCE.md § "Heap sizing").
- **Conformance.**  `heap.ceiling_progress` and `gc.host_stress` impose
  their ceiling with `setHeapLimits` and used to restore the saved values
  the same way, which on a controller-enabled space left fixed limits behind
  and read the soft limit without the lock the collector writes it under.
  They now save and restore through `adaptiveHeapStats()` and give the
  controller back (`SavedHeapLimits`, conformance/CycleDriver.h).  Test:
  `ConformanceSelfCheck.ACaseWithItsOwnCeilingRestoresTheAdaptiveHeap`.

### Fixed

- **Destroying a space no longer deadlocks with its collector when another
  space is live.**  `~ProtoSpace` joins its collector thread; with several
  spaces, a cycle ends with a grace period that waits for every registered
  thread of the process, including the destroying thread blocked in the
  join.  The thread is now marked out of grace periods for the join.
  Found by the controller's multi-space test (about one run in three hung).
  Test: `MultiSpaceTeardown.DestroyingASpaceWhileItsCollectorWaitsForAGracePeriod`
  (hung 3 runs out of 3 before the fix).
- **Test: the attribute-walk writer reaches safepoints.**  The writer thread
  of `AttributeEnumerationTest.ConcurrentMutationDuringWalkIsSafe` allocated
  only inside `setAttribute`'s critical section and called no safepoint, so
  a stop-the-world raised by the test's collector kicker could never
  complete: the walking thread parked for it, and the writer allocated until
  macOS killed the process (once in CI; usually the walk ended first).  A
  forced stop-the-world during the walk hung every run before the change.

## [2.9.6] - 2026-10-02

An allocation fix; the ABI is unchanged (SOVERSION 3, no class layout changed).

- **External buffers no longer call `aligned_alloc` with a size that is not a
  multiple of the alignment.**  `ProtoExternalBuffer` requested its 64-byte
  aligned segment with the caller's size unchanged, e.g.
  `aligned_alloc(64, 12)`: undefined behaviour per C11/C17, which glibc
  happens to accept and AddressSanitizer aborts on
  (`invalid-aligned-alloc-alignment`).  The rounding now lives in the internal
  `alignedAlloc` helper, so every caller is covered on every platform (the
  Windows `_aligned_malloc` path allocates the same rounded size): the request
  is rounded up to a multiple of the alignment, a zero-byte request becomes
  one alignment unit, and a size whose rounding would overflow `size_t`
  returns `nullptr` like any failed allocation.  `getSize` still reports the
  logical size the caller asked for.  Audit: the other `alignedAlloc` caller
  (the per-thread attribute cache) already requested a multiple of 64;
  `alignedArenaAlloc` uses `posix_memalign` / `_aligned_malloc`, which have no
  size requirement.  New test:
  `SwarmTest.ExternalBufferOddSizesAreFullyAddressable` (sizes 0, 1, 12, 63,
  64, 65, 4097; every byte zeroed, written and read back), which aborts under
  ASan before the fix.  The gating ASan CI job never reached the bug because
  the existing external-buffer tests used only 128 and 4096 bytes.

## [2.9.5] - 2026-10-02

A conformance-case fix; the library is unchanged (ABI SOVERSION 3).

- **Conformance: `join.parks` no longer reports an early-returning host as a
  rule-2b failure.**  When the runtime's joined thread finished before the
  case demanded its collection (150 ms in), the case's main thread waited in a
  bare `std::thread::join` of its 10 s timer, held the stop-the-world quorum
  itself, and reported "no collection cycle could complete" -- blaming a join
  it never observed blocking (protoScala's CI, 2026-09-30 to 2026-10-02).  The
  case now snapshots, at the moment the join returns, whether a cycle
  completed, whether the collection had been demanded and whether the release
  flag was up.  A join that returned on its own before any of that is a
  **Fail** headed "host contract violation, rule 2b not measured", with
  `joinReturnedAtMs` and `collectionRequestedAtMs`.  The timer is detached
  (never joined), the case's remaining wait runs in an `UnmanagedScope`, and
  the release flag is raised as soon as a cycle completes, so a conforming
  run takes ~0.2 s instead of 10 s.  `stw.quorum_completes` now also waits for
  its driver thread inside an `UnmanagedScope`.  New self-checks:
  `ConformanceSelfCheck.JoinParksReportsAHostThreadThatReturnsBeforeTheProbe`
  and `JoinParksPassesAClockBoundedHostThread` (the contract's
  wall-clock option stays legal).  The `Host::joinBlockingThread` contract in
  `protoCoreConformance.h` and docs/EMBEDDER-CONFORMANCE.md states the timing.
- **Correction to the 2.9.4 entry** (fixed in place there).  It said protoPython's and protoScala's
  conformance hosts "still read the flag plainly and should switch".  Neither
  host has ever read the flag: both ignore it and bound their thread by the
  clock, so they had no data race and need no change for the atomic read.

## [2.9.4] - 2026-10-02

The whole test suite now runs clean under ThreadSanitizer, and CI runs it
there.  One library field changed how it is accessed; the ABI is unchanged
(SOVERSION 3, no class layout changed).

- **`ProtoSpace::freeCellsCount` is updated atomically.**  Its writers hold
  `globalMutex`, but the conformance library's heap sampler reads it without
  the mutex -- and must, since the collector holds that mutex while it waits
  for the stop-the-world quorum.  TSan reported the read in
  `conformance.isolate.stw.quorum_completes`.  The field stays a plain `int`
  and gets `heapSize`'s treatment: atomic writes (`relaxedFetchAdd` /
  `relaxedStore`), and `relaxedLoad` for lock-free readers.
- **Conformance: the `joinBlockingThread` release flag is read and written
  atomically.**  It was a `volatile bool` shared between threads, a data race
  in `join.parks`, `stw.quorum_completes` and three `ConformanceSelfCheck`
  cases.  New `proto::conformance::releaseFlagRaised()` in
  `protoCoreConformance.h` is the read a host must use; the virtual's
  signature is unchanged, so existing hosts still build (protoPython's and
  protoScala's hosts never read the flag, so neither had the race; corrected
  in 2.9.5).
- **Conformance: detached helper threads no longer write to a returned stack
  frame.**  `stw.quorum_completes` detached a releaser that set the flag on the
  case's frame 25 s later, normally after the case had returned; `join.parks`
  detached a requester that wrote two stack atomics and may outlive the case
  while it waits on `globalMutex`.  Both now share heap-owned state.
- **Tests: every thread that holds a `ProtoObject*` is a protoCore thread.**
  Eight tests ran raw `std::thread`s with thread-less contexts (or, in
  `RootSetTest.ConcurrentAddRemoveIsSafe`, the main thread's `rootContext`
  shared by five threads), which races on `ProtoSpace::mainContext` and
  violates EMBEDDER-CONFORMANCE rule 11.  They use `ProtoSpace::newThread` and
  `ProtoThread::join` now.  `NewThreadRoots.AThreadLessContextSurvivesAThreadCreatedElsewhere`
  joins the thread it creates (TSan reported a thread leak).
- **Tests: the one deliberate race is excluded precisely.**
  `ConcurrentMarkSafety.ThreadCacheSlotFlipsDuringMark` writes the owner's cache
  slots from a helper thread on purpose; only that helper thread is excluded
  from TSan checking (`__tsan_ignore_thread_begin/end`).  There is no
  suppression file.  `StringBuildHeapLimitTest`'s resident-set bound is
  compiled out under TSan, whose shadow memory it would measure.
- **CI: new `tsan` job** (`.github/workflows/ci.yml`), the full suite minus the
  clock-dependent cases, gating.  docs/TESTING.md, "ThreadSanitizer", has the
  recipe and the rules the suite follows.

## [2.9.3] - 2026-10-02

A patch release with one thread-registration fix and one test made
independent of machine speed.  The ABI is unchanged (SOVERSION 3, no class
layout changed).

- **The threads list is snapshotted under `ProtoSpace::globalMutex`.**  Every
  update of `space->threads` (thread start, the adopted main thread, thread
  exit, the thread destructor) rebuilds the immutable list outside the mutex
  and publishes it under the mutex only if the list is still the snapshot it
  started from.  The publish was a plain store under the mutex, but the
  snapshot was a plain load taken without it.  ThreadSanitizer reported both
  consequences when threads exit together (19 warnings in 10 runs of
  `BulkListBuild.ConcurrentBuildersSurviveForcedCollections`): a data race on
  the non-atomic `space->threads` pointer, which is undefined behaviour, and
  `removeAt` reading the fields of list nodes built by another thread with no
  happens-before edge to their construction, which a weakly ordered CPU may
  show stale.  The snapshot is now taken under the mutex (`snapshotThreads`,
  core/Thread.cpp).  Nothing allocates or parks while the mutex is held for
  that load, so the rule that the rebuild must not allocate under the
  recursive mutex still holds.  New test
  `ThreadExitRelease.SimultaneousExitsLeaveTheThreadsListConsistent` releases
  eight threads at once for twenty rounds: under ThreadSanitizer it reported
  races in 5 of 5 runs before the fix, and it and the `BulkListBuild` suite
  ran 20 times with no report after.
- **`ConcurrentMarkSafety.ThreadCacheSlotFlipsDuringMark` is bounded by
  cycles, not by time, and gates CI again.**  It ran two fixed 2-second phases
  and then required 100 collection cycles, a throughput floor that MSVC Debug
  (61 cycles) and AddressSanitizer (31) failed with no defect involved.  Each
  phase now runs until it has observed 50 cycles, under a 300-second hang
  detector, and the case left the clock-dependent list in `ci.yml` and
  `cross-platform.yml`, so it now gates in every job, ASan and Windows Debug
  included.  It still detects the defect it guards: with the pre-2.6.0 mark
  of the thread caches restored (two loads per slot, no null skip in the mark
  loop) it crashes in 5 of 5 runs.
- Test counts in README and docs/INSTALLATION.md: 526 registered, 519 gating,
  7 clock-dependent.

## [2.9.2] - 2026-10-02

A patch release with one garbage-collector fix.  The ABI is unchanged
(SOVERSION 3, no class layout changed).

- **A context's return value is anchored before the context is popped.**
  `~ProtoContext` made `previous` the thread's current context (or
  `space->mainContext`, for a thread-less context) and only then allocated the
  `ReturnReference` that anchors the return value in `previous`.  That
  allocation can block for a collection -- at a stop-the-world poll, or in a
  refill at the hard heap limit -- and that collection's root scan then
  reached neither the return value nor the dying context's young chain.  A
  return value that was already a sweep candidate was freed while the caller
  was about to receive it: an object built in a nested call (whose context's
  destruction submitted it) and passed up, or objects reachable only through
  the young chain, such as the nodes of a list built in the context that an
  earlier `safepoint()` had submitted.  The destructor now anchors the return
  value while the context is still registered, then pops the context, then
  submits its young generation.  `ContextReturnAnchor.ReturnValueSurvivesACollectionInsideTheDestructor`
  forces that collection deterministically (hard limit at the current heap,
  every freelist drained); it failed in 50 of 50 runs before the fix and
  passes in every run after.  Found while investigating protoPython's
  Windows-only crash under `PROTOCORE_HEAP_LIMIT_CELLS`: with a protoPython fix
  and this one, its heap-limit test went from 9 crashes in 100 runs to none on
  Windows CI.

## [2.9.1] - 2026-10-02

A patch release.  No library code changed: the ABI and behaviour are those of
2.9.0.

- **The Windows hang of `MutableRootReclaim.ConcurrentWritersLoseNoUpdateDuringRelease`
  (the known issue of 2.8.0 and 2.9.0) is explained and fixed.**  It was not a
  deadlock and not corruption: the test ran four allocating writer threads
  without a heap limit, the configuration in which protoCore never makes an
  allocating thread wait for the collector.  A cycle sweeps every cell
  allocated during the previous one, and four threads allocating without pause
  produce cells faster than one collector thread sweeps them, so cycle times
  and the heap grow geometrically from wherever they start -- about twofold
  per cycle, measured on Linux too: with 11 cycles of writing instead of 5,
  2.9.0's test peaks at 0.25-1 GB resident instead of 40-80 MB.  Linux passed because its five cycles finish
  while they are a few milliseconds long.  On the 4-CPU Windows Server runners
  the four writers and the collector exceed the CPUs, and the thread that
  requests the cycles slept 150-200 ms instead of 2 ms after the first one (a
  woken thread waits a whole Server scheduling quantum for a CPU).  The writers
  allocated 11-15 million cells meanwhile; from that start the writers reached
  their iteration bound with 250-420 million cells (16-27 GB), the runner
  paged, and the cycle the test then waited for ran for minutes -- which a
  stack dump showed as a collector busy in mark, sweep or unmark.  The test now
  runs under a hard limit of 2^21 cells, at which a writer waits for the
  collector, and checks that the heap stays within twice that limit.  Windows
  CI, the test alone in a loop: 0 failures in 240 Release and 80 Debug runs,
  none longer than 1 s (run 36982874964), where 2.9.0 hung in 8 to 12 of 40
  Release runs and passing runs took up to 48 s.
- `docs/GarbageCollector.md` states the consequence for embedders: threads
  that allocate faster than the collector sweeps need a hard heap limit, or
  the heap grows without bound however often cycles are requested.

## [2.9.0] - 2026-10-02

A minor release from the review of the Windows port. One function is added
(`ProtoSpace::currentThreadStackBytes`); nothing is removed or changed for
existing code, and on Linux and macOS the ABI is unchanged (SOVERSION 3, no
class layout changed). On Windows the DLL is renamed (see below).

- **`setThreadStackBytes` is honoured on Linux and Windows too.** In 2.8.0
  only macOS created a thread of the requested size; elsewhere the value was
  stored and ignored, so a runtime that asked for 8 MiB on Windows ran on the
  default 1 MiB with no sign of it. `newThread` now runs the thread on a
  native thread created with that stack on every platform
  (`_beginthreadex` with `STACK_SIZE_PARAM_IS_A_RESERVATION` on Windows,
  `pthread_create` with `pthread_attr_setstacksize` elsewhere); with no size
  set (the default) nothing changes. If the platform refuses the size,
  `newThread` says so once on stderr and uses the default stack. New:
  `ProtoSpace::currentThreadStackBytes()` reports the calling thread's stack
  (`PROTOCORE_HAS_CURRENT_THREAD_STACK_BYTES`). The test asks for 32 MiB, checks
  the size from inside the thread and then recurses through 16 MiB; it failed
  on Linux (8 MiB) and on Windows (1 MiB) before the change.
- **Windows: the DLL is `protoCore-3.dll`** (the SOVERSION in the file name,
  the counterpart of `libprotoCore.so.3`); the import library is still
  `protoCore.lib` and names it, so consumers link as before and load the ABI
  they were built against. `protoCoreConfig.cmake` now checks for that file on
  Windows, where its ABI check used to be disabled. A program that copies the
  DLL by name must copy `protoCore-3.dll` (`$<TARGET_FILE:protoCore::protoCore>`
  already does).
- **Windows: the ZIP and the NSIS installer ship the Visual C++ runtime**
  app-local in `bin\` (`InstallRequiredSystemLibraries`), so they work without
  the redistributable; the Universal CRT is part of Windows 10 and later. NSIS
  is a generator only where `makensis` is found. CI unpacks the ZIP, builds a
  consumer against it, runs it from a clean directory, and checks that
  `find_package` refuses the prefix once `protoCore-3.dll` is removed.
- **No MSVC-internal 128-bit type.** `Integer.cpp` used `std::_Signed128` from
  MSVC's internal `<__msvc_int128.hpp>`. The bignum code now uses
  `core/WideArith.h`: `unsigned __int128` on GCC/Clang, `_umul128`/`_udiv128`
  on MSVC x64, and portable half-digit code elsewhere, all tested against
  values computed with Python. Knuth's Algorithm D is driven through its rare
  steps (trial quotient 2^64, rhat overflow, the D6 add-back) by operands
  found with a model of the algorithm; removing the add-back fails the test.
- **MSVC warning level 3** (it was the compiler's /W1 default) for
  protoCore's own targets, with every C4244/C4267 truncation, C4477 format,
  C4291 placement-delete, C4146, C4099 and C4624 warning fixed rather than
  disabled; `_CRT_SECURE_NO_WARNINGS` silences only the C4996 advice to use
  Microsoft-only `getenv`/`fopen` replacements.
  `PROTOCORE_MSVC_WARNINGS_AS_ERRORS=ON` adds /WX, and the Windows CI jobs
  use it.
- `PROTO_PREFETCH` is `_mm_prefetch` on MSVC (it compiled to nothing). The
  Windows `posix_memalign` stand-in is no longer a global function in an
  included header: it is `proto::alignedArenaAlloc`. `Cell` has the
  placement `operator delete` matching its `operator new` (a no-op, as the
  memory belongs to the context's arena).
- **CI:** a Windows Debug job (MSVC checked iterators) runs the suite; the
  clock-dependent cases run on macOS and Windows as a non-gating step, as on
  Linux. `ResolutionChain_DefaultEntriesPerPlatform` checks the default
  module resolution chain on every platform (it was empty on Windows).

## [2.8.0] - 2026-10-01

- **`ProtoSpace::setThreadStackBytes` / `threadStackBytes`** (new; test with
  `PROTOCORE_HAS_THREAD_STACK_BYTES`). macOS gives a secondary thread 512 KiB
  and has no process-wide default, so a runtime's deep recursion in a future
  or an actor ran out of stack there, while glibc
  (`pthread_setattr_default_np`) and Windows (`/STACK`) let the embedder set
  it. On macOS `newThread` now runs the thread's body on a pthread of at
  least the requested size. Linux and Windows are unchanged: the value is
  only stored there. Additive: two new exported functions, SOVERSION 3.

## [2.7.0] - 2026-10-01

A minor release: the API gains the names `proto::proto_long`,
`proto::proto_ulong`, `PROTO_L`, `PROTO_UL` and `PROTO_FMT_U`, so code written
against them needs 2.7.0. Nothing is removed or changed for existing code, and
the ABI (SOVERSION 3) is unchanged.

- **Windows: native MSVC build.** protoCore builds with Visual Studio 2022
  and passes its whole suite on Windows 11 (517/517); `cmake --install` and
  `cpack -G ZIP` work. The API's 64-bit integers are now spelled
  `proto::proto_long` / `proto::proto_ulong` (`PROTO_L`, `PROTO_UL`,
  `PROTO_FMT_U`): `long long` on Windows, where `long` is 32 bits, and exactly
  `long` / `unsigned long` everywhere else, so on Linux and macOS the types,
  the mangling and the ABI do not change (`libprotoCore.so` exports the same
  1053 symbols as 2.6.2, and the suite passes 517/517 on both). Platform shims
  cover `__int128`, aligned allocation, `__PRETTY_FUNCTION__` and
  `__builtin_prefetch`; static data read across the DLL boundary is marked
  `PROTOCORE_DATA`. The NSIS registry commands were double-escaped so
  CPackConfig.cmake parses them. No change for existing embedders on Linux
  or macOS.
- **macOS: builds and passes on Apple clang (arm64).** Apple's libc++ before
  LLVM 19 has no `std::atomic_ref`; the four relaxed `int` accesses that used
  it go through `relaxedLoad` / `relaxedStore` / `relaxedFetchAdd`, which are
  `std::atomic_ref` wherever the library has it. The residency tests measure
  resident memory through the Mach task on macOS (there is no `/proc`).
- **CI on macOS and Windows** (`.github/workflows/cross-platform.yml`), next
  to the Linux `ci.yml`.

## [2.6.2] - 2026-09-30

- **The heap ceiling holds while the collector releases dead mutables.**
  Each cycle the collector removes the entries of dead mutable objects from
  the process-wide mutable table. It did so with one `removeAt` per entry,
  path-copying the shard's tree once per dead object: about 20 cells each,
  over a million cells per cycle for a program that creates mutables in a
  loop. The collector cannot wait for its own cycle, so once the mutators had
  taken the reclaimed cells it took that garbage from the OS, past the
  ceiling, cycle after cycle (protoST's `cli_memory_bounded` reached 1.6-1.9
  GB under a 640 MB ceiling on a CI runner; 11.9M cells under a 10M ceiling
  on a workstation). The entries are now removed per shard in one join-based
  pass that shares every untouched subtree and drops wholly-dead ones without
  allocating: removing 16,000 of 20,000 dense keys costs 25 cells instead of
  178,292, and the heap ends exactly at its ceiling. The collector and other
  callers exempt from waiting are also clamped at the ceiling to one refill
  batch, no longer a whole OS block. Tests: `SparseListBulkRemove.*`.
  No API or ABI change.

## [2.6.1] - 2026-09-29

- **Creating a thread no longer detaches a live context's roots.**
  `ProtoSpace::newThread` builds a temporary context with no previous context,
  which the constructor registered as the calling main thread's current
  context or, on any other thread, as `ProtoSpace::mainContext`; neither was
  put back. A thread-less context in use while another thread created a
  thread stopped being scanned, and a later cycle freed the objects only it
  held (found by protoST, whose worker pool grows while actors block in I/O:
  the main program's variables were freed). The temporary context is now
  built with the collector's never-registered constructor, so there is no
  window in which it replaces either root, and it is destroyed at the end,
  returning its allocation batch (created from any thread but the main one,
  it used to leak that batch). Thread handles stay
  out of every collection until joined, as before. Tests: `NewThreadRoots.*`,
  `ThreadExitRelease.*`. No API or ABI change.

## [2.6.0] - 2026-09-29

Several ProtoSpaces in one process now share objects correctly: one table of
mutable states per process, a thread shared by several spaces no longer
stalls their collections, collection cycles are serialized, and a
multi-space cycle frees its dead cells only after a grace period. Also:
integer arithmetic fixes (multi-word division, negative bitwise operations,
shiftLeft overflow) and four data races found by ThreadSanitizer. With a
single space behaviour is unchanged and six benchmarks show no regression.
No API or ABI change: `PROTOCORE_ABI_SOVERSION` stays 3, so binaries built
against 2.5.0 run unchanged.

### Integer arithmetic

Found by a differential run of protoCore's integer operations against
Python's arbitrary-precision integers (100,000 random cases, operands from 1
to 1,000 bits, both signs); every case now matches.

- **Division and remainder by a divisor of two or more 64-bit words answered
  wrong values.** The multi-word path aligned the divisor with at most one
  corrective shift, so a quotient wider than one word lost its high bits:
  `2^140 / 2^70` gave `2^65 - 1`. It is replaced by Knuth's Algorithm D on
  64-bit digits, which also stops allocating a LargeInteger per quotient bit
  (the old loop shifted through `fromTempBignum(nullptr, ...)`).
- **`bitwiseAnd`/`bitwiseOr`/`bitwiseXor` lost the sign of a negative
  LargeInteger operand.** The two's-complement conversion pre-filled the high
  words with ones and then inverted them to zeros, so `-1 | x` answered
  `2^64 - 1` for any LargeInteger `x`.
- **`shiftLeft` of a negative SmallInteger overflowed silently** when the
  result exceeded 64 bits (`-7410793187882849 << 35` answered a positive
  number). The fast path now computes the exact product in 128 bits.
- **LargeInteger construction runs inside a GC critical section.** The 2.x
  critical-section audit (9f4ede54) placed the guard in an unused duplicate of
  `fromTempBignum` in `LargeInteger.cpp`; the live copy in `Integer.cpp` had
  none. The duplicate helpers (which also used a wrong SmallInteger bound of
  2^55) are removed.

Test: `NumericTest.ArbitraryPrecisionMatchesReferenceValues`.

### Several ProtoSpaces in one process

Design, rules and limitations: [docs/GLOBAL_MUTABLE_TABLE.md](docs/GLOBAL_MUTABLE_TABLE.md).

- **One table of mutable states per process.** A mutable object of one space
  read or written through another space's context answered the state of the
  other space's object with the same `mutable_ref`, or its own birth state,
  and never an error: each space had its own table and its own ref counter,
  both starting at 1. The 256 shard roots are now process-global
  (`globalMutableShards`), a ref carries its space's id in its high bits (the
  first space keeps the refs it always had), and every space's collector marks
  the whole table. `ProtoSpace::mutableRoot` stays in the class for the ABI 3
  layout and is not read; `findMutableCycles` reports the space's own entries.
- **A thread shared by several spaces no longer stalls their collections.** The
  thread that constructs a space counts in its quorum, so a thread that built
  two spaces was a member of both, but it only answered the stop-the-world of
  the space whose code it ran and only left the quorum of the space it blocked
  through: the other space's collection waited for it indefinitely (reproduced
  in `MultiSpaceThread.*`). Safepoints now answer any member space's
  stop-the-world, and unmanaged regions and heap waits leave every member
  quorum.
- **Collection cycles are serialized in the process** (a process-wide token
  from Phase 1 to Phase 6). Two spaces' cycles ran at once, and a marker that
  reaches another space's cells shares their single mark bit.
- **With more than one space live, a cycle's dead cells are freed after a grace
  period**: once every registered thread has passed a safepoint outside a
  critical section, or is parked or out of its quorum. A thread of another space
  could still be reading them; before, the sweep rewrote and reused them at once
  (302,398 corrupt reads in `AValueHeldByAnotherSpacesThreadSurvivesUntilItsSafepoint`).
  No thread is stopped and the lookup paths are unchanged.
- **The cache epoch is process-wide** (`multispace::gcEpoch`); threads clear
  their caches at their first quiescent point after any space's cycle.
- **A destroyed space's table entries are removed** by the next cycle of a live
  space.

With one space in the process every rule reduces to the previous behaviour.
No public API change; `PROTOCORE_ABI_SOVERSION` stays 3.

### Data races found by ThreadSanitizer (single-space as well)

- **A shard root was read with `memory_order_relaxed` and then dereferenced**
  (`resolveMutableState`). A reader could see a sparse-list node published by
  another thread's compare-and-swap before the node's fields: harmless on x86,
  a torn read on weakly ordered CPUs (arm64). Now `acquire`, free on x86.
- **The DirtySegment free pool's lock-free pop could hand one segment to two
  threads** (ABA: a popper read `segment->next` of a head that another thread
  popped and the collector pushed back before the first popper's CAS). Pops are
  now serialized by a small spin lock; pushes stay lock-free.
- **`triggerGC()` read and wrote `heapSize`, `freeCellsCount` and `gcStarted`
  without `globalMutex`**, although its only callers are embedders on arbitrary
  threads. It now takes the (recursive) mutex.
- **The heap-limit fast path read `heapSize`/`maxHeapSize` unsynchronized**
  while `getFreeCells` wrote them; both sides now use `std::atomic_ref` (relaxed;
  the value is still re-validated under the mutex). Layout unchanged.

Remaining TSan reports come only from tests that use a `ProtoContext` on an
unregistered `std::thread` (EMBEDDER-CONFORMANCE rule 11).

### Packaging

- **The DEB now ships a `DEBIAN/shlibs` file** (`libprotoCore 3 protocore (>= 2.5.0)`),
  so a consumer's ABI dependency is derived from the binary instead of hand-written.
  This closes a hole that was measured: the runtimes declare a `Depends` version
  *range* on `protocore`, and a range is not an ABI check — a decoy protoCore 2.1.0
  was accepted by dpkg for all five runtimes, and every binary then died at startup
  with `libprotoCore.so.3: cannot open shared object file`, because
  `PROTOCORE_ABI_SOVERSION` went 2 → 3 in 2.2.0 while their `find_package` floors
  are 2.0 and 2.1. The RPM never had this hole, since `rpm` derives
  `Requires: libprotoCore.so.3()(64bit)` from the library itself.

  The five runtimes had already set `CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON`, which by
  itself produced nothing: `dpkg-shlibdeps` resolves the SONAME to this package and
  then reads its `shlibs` control file, which did not exist, and CPack passes
  `--ignore-missing-info`, so the dependency was dropped **silently** rather than
  failing the build. The missing piece was in the producer.

  The policy is `>=`, not CPack's default `=`, which would pin a consumer to the
  exact protoCore it was built against and break every installed runtime on a
  2.5.1 bugfix release. The SONAME remains the compatibility statement.

- **The DEB now refreshes the shared-library cache.** Generating `shlibs` also makes
  CPack emit `postinst`/`postrm` scripts that run `ldconfig`. Previously no protoCore
  DEB did, so `libprotoCore.so.3` was absent from `ldconfig -p` immediately after
  installation; binaries still ran, via their `RUNPATH`, but any consumer relying on
  the cache could not find the library.

No source, API or ABI change; `PROTOCORE_ABI_SOVERSION` stays 3.

## [2.5.0] - 2026-09-25

Documents a retention property of the collector, and ships the exact detector
for it as a public diagnostic. **The collector is not changed.**

> **A cycle among mutable objects is never collected.**

**Why 2.5.0 and not 2.4.1, and why `PROTOCORE_ABI_SOVERSION` stays 3.** This
release adds a public API -- `ProtoSpace::findMutableCycles`, the structs
`proto::MutableCycle` and `proto::MutableGraphReport`, and two virtual methods on
`proto::conformance::Host` -- so it is not a patch: a consumer can now call
something it could not call before, and a header a runtime compiles against has
changed. It is nevertheless purely **additive**: `ProtoSpace` gained no data
member and no virtual, so every embedder's compiled layout and call sites are
byte-identical and a stale binary keeps working. A minor bump is the exact
statement of that. Note also that 2.4.0's "no public header change" claim
described 2.4.0's commits; this one does change `headers/protoCore.h`, and
embedders should rebuild to see the new API.

### Added

- **`ProtoSpace::findMutableCycles(ProtoContext*, cellBudget = 0)`**
  (`headers/protoCore.h`, `core/MutableCycles.cpp`) -- an **exact** detector for
  cycles in the mutable-reference graph, reporting the participating handles and
  a closed walk through them that names the attribute at every hop it can name.

  It can be exact rather than heuristic because the mutables table enumerates
  every written handle in the space and every cell field is `const` after
  construction, so the only edge in the heap that can close a loop is the
  handle-to-state indirection the table implements. The scan builds the
  *augmented cell graph* -- each cell's ordinary references plus one synthetic
  edge per handle to its current value -- and runs one Tarjan pass:
  O(cells + references). It allocates no `Cell`, and it holds a
  `ProtoContext::CriticalSection` for the walk, which is what makes it safe
  (a thread in a critical section does not park, so no new stop-the-world can
  begin and no new sweep can free a cell under the scan). The corollary is that
  it stalls the collector, so it belongs at a quiescent point.

  **Only cycles are reported.** An acyclic handle-to-handle edge is not a
  violation -- the downstream handle's entry is released a cycle after the
  upstream one dies -- and reporting them would mean one line per object in the
  program.

- **`PROTOCORE_MUTABLE_CYCLE_CHECK`** -- set the environment variable and every
  `ProtoSpace` prints one cycle report as it is destroyed, from any binary that
  links `libprotoCore`. The value selects the destination: `1` or `stderr` writes
  to stderr, anything else is a file path the report is appended to, one block
  per space, tagged with the process id. The file form is what makes the useful
  recipe work -- `PROTOCORE_MUTABLE_CYCLE_CHECK=<file> ctest` sweeps a whole
  suite in one run -- because a suite that diffs a script's stderr fails the
  moment a diagnostic appears there. One `getenv` when unset. This exists so a runtime with no conformance Host adaptor can still ask
  the question without writing any code. It is deliberately not hooked into the
  end of a GC cycle: the walk is O(live mutable graph), and a per-cycle hook
  would need a frequency policy and would stall the collector on a schedule
  nobody chose.

- **Conformance rule 13** (`mutable.graph_cycles`,
  `conformance/CaseMutables.cpp`), with `Host::makeMutableGraph()` and
  `Host::declaredMutableCycles()`. The verdict is a **declaration verified
  against a measurement**, not "no cycles": a cycle can be an oversight (a
  diagnostic back-pointer) or exactly what the program means (a captured
  variable that refers to itself, a doubly-linked list of mutable nodes, two
  actors referencing each other), and protoCore cannot tell those apart. A
  runtime declares how many of its cycles are structural and the case fails on
  the ones it did not declare. Self-checked in both directions
  (`ConformanceSelfCheck.MutableGraphCycles*`): it fires on a cycle and stays
  silent on the acyclic instance-to-class edges every runtime in this family
  builds by the thousand.

### Documented

- **`docs/MemoryModel.md` § 7** -- the property, with the two collector sites
  that combine to produce it (`core/ProtoSpace.cpp:519-524`, Phase 2's
  unconditional per-shard root push, and `core/ProtoSpace.cpp:992-1000`, Phase
  5b's release only of finalized handles); the acyclic case, which works; the
  rule, and the two shapes where it does **not** apply and the retention simply
  stands; and the two fixes considered and rejected -- an ephemeron pass, which
  would put a fixpoint into a concurrent mark phase whose failure mode is a
  use-after-free rather than a leak, and splitting assignment by destination,
  which would make `x = y` alias or freeze depending on what `x` is and would
  silently freeze legitimate cyclic structures.

- Two things the code contradicted a first reading of, both found by writing the
  detector: **a table entry exists only from a mutable's first write**, not from
  its creation (so a bare `ProtoSpace` has zero entries even though
  `objectPrototype` is created mutable); and **a handle references other handles
  through the `parent` chain and `attributes` it was born with**, which are
  traced, so a cycle can be closed without passing through any state.

- Cross-references in `docs/GarbageCollector.md` beside the mutable-shard table,
  Phase 2's root capture and Phase 5b's release, and rule 13 plus checklist item
  C13 in `docs/EMBEDDER-CONFORMANCE.md`.

### Tests

`test/MutableCycleDetectorTests.cpp`, 9 cases. The mutation matrix is one line
wide: the same builder either captures the mutable handle (the detector fires,
and the reported path names both closing attributes) or the handle's current
value (it is silent, while still counting the reference, so its silence is not
the silence of a scan that saw nothing). The property itself is measured on
table entries against the count the workload created, never against `> 0`: 400
acyclic mutables dropped in a child context fall to under 10% of what was
created after 8 cycles, while 200 two-handle cycles retain all 400 entries and
the retained count is **identical** after a second round of 8 cycles -- the "no
monotone progress" half of the claim.

## [2.4.0] - 2026-09-25

Two kernel defects, on the maintainer's instruction to *"fix both without fail
even though they touch the kernel"*. Both were found by the P4 embedder
conformance work; neither was reachable from the embedders' own test suites.

**`PROTOCORE_ABI_SOVERSION` stays 3.** No class gained or lost a member, no
virtual was added or removed, no signature and no return convention changed.
The two new symbols (`proto::returnUnusedCellBatch` and
`proto::pmqTakeAllWindowHook`) are additive and declared only in
`headers/proto_internal.h`, so no embedder's compiled layout or call sites
move, and `SameMajorVersion` consumers keep working.

**Why 2.4.0 and not 2.3.1.** A patch release would say "nothing here you need
to know about", and that is not true of the second fix. It removes a
documented Known Issue from `docs/GarbageCollector.md` -- an exiting thread now
gives its allocation batch back -- which is a new guarantee an embedder may
rely on, and it gives `ProtoThread` new post-conditions after a completed
`join`: `getCurrentContext()` now returns `nullptr` instead of a stale pointer
to a context nobody owned, and the `std::thread` object is gone. Reading a
finished thread's context was never defined, but it used to return something;
that is a behaviour change, so it gets a minor bump.

### Fixed

- **`ProtoMPSCQueue::takeAll` could lose the last message of a batch**
  (`core/ProtoMPSCQueue.cpp`). Silent data loss, shipped. `takeAll` published
  its retain cell with the chain it had **loaded**, then detached a possibly
  **longer** chain -- every node a producer prepended in between was in the
  detached chain and in no retain cell. The file's own argument for those nodes
  was that they "were allocated after S, so they are young cells of the pushing
  context and are not candidates of the running cycle", which is true and
  insufficient: it covers only the cycle running when the window closed, and
  the walk that follows parks for stop-the-world every 64 nodes, so the call
  routinely spans a cycle **boundary**. For the next cycle those nodes are
  ordinary candidates, hanging off nothing but a C++ local, and the collector
  has no view of C++ locals. Observed signature, in the field: of a 182-message
  batch exactly one element lost every own attribute, and always the **last** --
  the chain is LIFO, so the node prepended inside the window is the last item
  out.

  The fix is one store: after the detaching exchange, publish the chain that
  was actually detached into the same retain cell. It cannot narrow anything
  (only prepends happen, so the loaded chain is a suffix of the detached one),
  it leaves the publish-before-detach ordering the GC-safety proof depends on
  untouched, and it is still inside the window, so it is visible before this
  thread can park and therefore before any cycle for which those nodes are
  candidates can begin. The window's ABA argument is also intact: a store is
  neither an allocation nor a protoCore call, so nothing there can complete a
  sweep between the `head` load and the CAS that races it.

  **Reproduced deterministically before being fixed.** Racing a producer
  against a consumer gave 4 failures in 40 runs against 0 in 40 -- p ~ 0.12,
  which cannot distinguish a fix from luck, and this project does not ship on
  that. `test/ProtoMPSCQueueWindowTests.cpp` instead **enters** the window
  through a test-only hook (`proto::pmqTakeAllWindowHook`, null in every build,
  one relaxed load per `takeAll`) and asserts two things: that every node the
  detach took is reachable from `retained`, and -- with a pause armed from
  inside the window, so the candidate set is always fixed with the walk in
  flight -- that a canary `Cell` reachable only through the window node is
  traced and never finalized. Removing the widening store fails both, 10 runs
  out of 10, with identical numbers.

- **An exiting thread no longer leaks its allocation batch**
  (`core/Thread.cpp`, `core/ProtoSpace.cpp`). Measured in a bare `ProtoSpace`
  with no runtime at all: `freeCellsCount` fell by **4,096 cells per
  empty-bodied thread** and **8,192 per allocating one**, while `heapSize` and
  `liveCellsLastCycle` stayed constant -- the signature of memory that is
  neither live nor free. The thread's root `ProtoContext` (and with it its
  entire un-submitted young generation, and its `automaticLocals` array), the
  two per-thread caches (32 KiB `aligned_alloc` + 24 KiB `malloc`, invisible to
  `heapSize`) and the `std::thread` object leaked with it. After the fix,
  200 threads through a bare space leave `heapSize` unchanged and `inUse`
  **lower** than the baseline.

  **The release is not in `finalize`, and that is the point.** Sweep calls only
  `finalize()`, and `ProtoThreadImplementation::finalize` is empty, so the
  obvious fix is to fill it in. It is illegal three times over.
  `docs/GarbageCollector.md` section 7 forbids a finalizer from blocking,
  allocating or publishing with compare-and-swap -- and returning the batch
  blocks on `globalMutex`, destroying the context publishes a young generation,
  and joining the `std::thread` blocks outright. A finalizer is also no proof
  that the OS thread has stopped, since sweep runs with the world going, so it
  could free caches a live thread is still reading. And it would never run at
  all: the thread cells live in the young chain of the scratch `ProtoContext`
  that `ProtoSpace::newThread` never destroys, so they are never sweep
  candidates.

  So the release happens where the owner gives the resource up. On the exiting
  thread, in `thread_main`'s tail (`releaseExitingThread`), after it has left
  `runningThreads` and the threads list -- so it can block freely without
  holding the stop-the-world quorum, and no root scan can be walking its
  context. Order is load-bearing: the root context is destroyed **first**,
  because `~ProtoContext` is what submits the young generation (including the
  cells `removeAt` just allocated for the new threads list, which are reachable
  from `space->threads` and so were never garbage, merely unaccounted); the
  batch goes back **last**, via the new `returnUnusedCellBatch`, because until
  the context is gone the thread could still allocate from it. The
  `std::thread` object is released by `ProtoThread::join` -- the only place
  that can, since a thread cannot join itself and a completed join is the only
  proof protoCore ever gets that the OS thread is gone. A thread that is never
  joined still leaks its `std::thread`; that is the embedder's side of the
  contract.

  This also closes a latent GC hazard that the fix would otherwise have turned
  into a use-after-free. `ProtoContext`'s constructor registers every context
  it builds as the current context of its thread, or as `ProtoSpace::mainContext`
  when it has none; a new thread's root context has `previous == nullptr`, so it
  looked like a thread root and silently took over one of the **creating**
  thread's root slots. That was merely wrong while nothing deleted the context.
  `ProtoThreadImplementation`'s constructor now snapshots both slots and puts
  them back.

  Regression cover: `test/ThreadExitReleaseTests.cpp`, asserting the
  per-thread cell delta against a denominator and, one by one, that a joined
  thread holds no context, no batch, neither cache and no `std::thread`.

### Changed

- `ProtoThread::join` releases the `std::thread` object once the join has
  completed, and nulls it, so a second `join` on the same `ProtoThread` returns
  at the existing null check instead of touching a freed object.
- `ProtoThreadExtension::clearCachesAfterStopTheWorld` returns immediately when
  either cache is null, which is the state of a thread that has exited.

### Documentation

- `docs/GarbageCollector.md` section 7 gains a worked example of what the
  finalizer contract rules out, using the thread release as the case, and
  names the Phase 5b record-then-drain pattern as the escape hatch for release
  work that must publish.
- `docs/GarbageCollector.md` Known issues: the exiting-thread batch leak is
  struck out and its fix described; the `newThread` scratch-context leak, which
  remains, is written down for the first time -- it is a handful of cells per
  thread rather than a batch, and it is the reason a thread's release cannot be
  driven from a finalizer.

## [2.3.0] - 2026-09-25

Phase P4. Maintainer's instruction of 2026-09-25: *"add an audit phase for all
embedders that checks protoCore's rules"*, delivered as an executable suite
rather than a review; plus the maintainer's ruling, given during the phase, to
fix `ProtoThread::join` in the kernel instead of auditing every embedder for it.

**`PROTOCORE_ABI_SOVERSION` stays 3.** No class gained a member, no signature
changed, no return convention changed. The version moves 2.2.0 -> 2.3.0 for one
behaviour change and one new artefact, and `SameMajorVersion` consumers keep
working. But read the behaviour change below before linking an old embedder
binary against this library and assuming nothing moved.

### Fixed

- **`ProtoThread::join` now leaves protoCore's running set while it blocks**
  (`core/Thread.cpp`). This closes a **deadlock**, not slow shutdown.
  `runningThreads` starts at 1 -- the main thread is counted from `ProtoSpace`
  construction -- and every managed thread adds one, while a stop-the-world
  phase cannot begin until `parkedThreads >= runningThreads`. A bare
  `std::thread::join` reaches no safepoint, so a registered thread blocked there
  still counted as running: the quorum could never be met, no cycle could start,
  and every thread that then needed memory waited for a cycle that could not
  begin -- usually including the thread being joined, which is why the join never
  returned either. Measured in protoClojure: four blocking joins each hung to a
  90-second timeout and each completed in about three seconds once bracketed.

  No documentation stated the obligation **for a join**: `ProtoThread::join`
  carried no doc comment at all before this fix. The general rule was written
  down -- `DESIGN.md`, "Unmanaged regions" (pre-fix `:126-202`), documents
  `UnmanagedScope` and the quorum formula with a "when to use it" table -- but
  that table's rows are `read`/`write`, `sleep`, `poll`, `accept` and
  "third-party C library that may block", and **there was no row for a thread
  join**. (This entry first said no documentation stated the obligation at all;
  corrected 2026-09-25, `docs/FIELD-NOTES.md` case 3.) `join` is protoCore's own
  blocking call, so an embedder had no way to know it had to bracket a kernel API
  against the kernel's own quorum. Wrapping the call in an `UnmanagedScope` as
  well remains harmless and idempotent (`unmanagedDepth` is a counter; only the
  outermost pair moves `parkedThreads`), so existing embedder guards need not be
  removed -- and four runtimes in this family have them.

  **The one exception, and it is the caller's bug:** inside a
  `ProtoContext::CriticalSection` (`criticalSectionDepth > 0`) `join` does NOT
  leave the running set, because the caller holds cells reachable only from C++
  locals and a root scan would miss them. Leaving would trade a deadlock for
  memory corruption, which is the worse trade. The join still happens, the quorum
  is still held for its duration, and a one-time diagnostic names
  `docs/EMBEDDER-CONFORMANCE.md` rule 12. Pinned by
  `ConformanceSelfCheck.JoinInsideCriticalSectionStillJoinsAndDoesNotPark`.

  This covers `ProtoThread::join` only. A runtime that calls `std::thread::join`
  or `pthread_join` directly on a thread it registered is still broken, and the
  kernel cannot see it -- which is why that stayed a conformance rule.

### Added

- **`libprotoCoreConformance` and `protoCore::conformance`** -- the embedder
  conformance suite: twelve executable cases driven through a
  `proto::conformance::Host` adaptor each runtime implements itself. Framework-free
  (cases return results as data), and protoCore never names a runtime -- both
  asserted by tests rather than intended.
- **`headers/protoCoreConformance.h`**, plus three-line GoogleTest and Catch2
  adapters and `PROTOCORE_CONFORMANCE_ISOLATE_MAIN`, all installed.
- **`scripts/conformance/check_static.py`** and `rules.json` -- the static half,
  as a per-repository ratchet with written justifications and a hash of each
  allowlisted line. The script tests itself: eight positive fixtures must fire
  and four negative fixtures must stay quiet -- run
  `python3 scripts/conformance/check_static.py --self-test` and count, rather
  than trusting this line. (It said seven until 2026-09-25, when running the
  self-test printed eight positive and four negative.)
- **`docs/EMBEDDER-CONFORMANCE.md`** -- the twelve rules as normative text, the
  per-function absent-value sentinel table, the three conforming shapes of rule
  11, and the three judgement items with what IS mechanised beside what is not.
- **`test/ConformanceSelfCheckTests.cpp`** -- rule 10 applied to the suite
  itself. Six deliberately non-conforming hosts each break exactly one rule and
  each must turn its case red; four more tests police the harness.

### Documented

- **`ProtoContext::safepoint()` is the only place a context's young generation is
  submitted.** The header documented it as the stop-the-world handshake hook and
  said nothing about submission, yet under `PROTOCORE_GC_REINCLUDE_SURVIVORS` it
  is the sole submission point. An embedder reading only that paragraph would
  conclude that a CPU-bound loop needs a safepoint and an allocating loop does
  not, which is the opposite of the truth for reclamation. That omission is a
  plausible contributing cause of two measured bugs: one runtime reclaimed 0
  cells of 2,748,398 across its whole history with 848 tests green, and another's
  apparent live set was 90.8x its real one -- 196,519 cells against 2,164 at a
  400,000-cell ceiling (`protoClojure/tests/cli/loop-garbage-is-reclaimed.sh`).
  Corrected on 2026-09-25: this entry first said 833 tests, which was that
  suite's size at an earlier fix, and 110x, for which no operand pair was ever
  recorded (`docs/FIELD-NOTES.md`, cases 1 and 2).

### Known, and reported rather than fixed

- **`ProtoThread::getCurrentThread` and `ProtoSpace::getCurrentThread` are
  declared in `headers/protoCore.h` and defined nowhere.** An embedder that calls
  either gets an undefined reference at link time. Found while writing the
  rule-11 case, which now reads `space->threads` directly.

## [2.2.0] - 2026-09-25

Phase P3. Maintainer's ruling of 2026-09-24: *"hacer la internación global y la
lista de módulos como raíz"*, and, separately, that a module's identity in that
list is provider + path + version.

### Added

- **`globalSymbolTable()` and `globalSymbolCount()`** (`headers/proto_internal.h`,
  `core/SymbolTable.cpp`). One `SymbolTable` for the lifetime of the process,
  created on first use and deliberately never destroyed.
- **`ModuleIdentity`** (`headers/protoCore.h`, `core/ModuleIdentity.cpp`): a
  module's identity as provider GUID + logical path + version, rendered as one
  canonical string with `\x1F` between the components. `unversioned()`,
  `getProviderGUID()`, `getLogicalPath()`, `getVersion()`, `asKey()`,
  `operator==`.
- **`ModuleRootTable`** (`headers/proto_internal.h`, `core/ModuleRoots.cpp`) and
  `globalModuleRootTable()`: the process-global module list, and a GC root. 8
  shards, append-only chunks that never move, a per-shard published count, the
  `TupleInterner` capture/walk split, and `purgeSpace()` for teardown.
- **Four additive `ProtoSpace` methods**: `addModuleRoot`, `moduleRootCount`,
  `registerModule`, `findModule`. Non-virtual; no vtable and no layout change.

### Changed

- **All string interning is process-global.** `ProtoString::createSymbol` returns
  the same address for the same bytes in every `ProtoSpace` of the process.
  `ProtoSpace::symbolTable` keeps its type and slot and is now a **borrowed**
  pointer to the one global table, so all eleven existing
  `ctx->space->symbolTable` call sites are untouched and the mid-construction
  sentinel that six null checks in `core/ProtoObject.cpp` rely on still fires
  exactly when it used to.
- **`~ProtoSpace` no longer frees the symbol table.** The first space to die
  would otherwise free the table every other space of the process is still
  using. This fixes a leak rather than creating one: `~SymbolTable` frees only
  the `Bucket` nodes, never the symbol cells, so every destroyed space already
  leaked its whole symbol set. `delete tupleInterner` stays.
- **`SharedModuleCache` is keyed by `ModuleIdentity::asKey()`**, not by the bare
  logical path.
- **The cache probe moved INSIDE the resolution-chain loop**, one probe per
  entry, because the provider is not known until an entry is selected. Chain
  order is therefore now respected: a module already loaded from a later chain
  entry no longer shadows an earlier entry that can serve the same path.
- **The O(modules) stop-the-world module loop is gone.** GC Phase 2 calls
  `ModuleRootTable::captureForGC()` — 8 counter reads, no entry dereferenced —
  and GC Phase 4 pushes the captured entries after the world resumes, filtered to
  the collecting space's own entries. Measured on the same test with 2000 module
  roots: cumulative Phase 2 over three cycles 329 μs before, 138 μs after.
- **`ProtoSpace::moduleRoots` and `moduleRootsMutex` are retired** — held empty,
  never iterated, retained only so the layout does not change. Use
  `addModuleRoot()` for a module, or `createRootSet()` for anything that must be
  unpinned.
- **`getImportModuleImpl` builds its wrapper once**, for both the cache hit and
  the fresh load, and parents it to `space->objectPrototype` in both. The two old
  branches differed on that, so the same call returned a wrapper with a different
  prototype chain depending on whether the module happened to be cached already.
- **The `"Absolute fall back (rare or error)"` comment in
  `ProtoContext::allocCell` is corrected.** That branch is the perennial
  allocation path that `SymbolTable::intern` and `ProtoString::createSymbol`
  depend on by contract; describing it as an error path invited a future reader
  to delete it. The comment now also states the distinction the phase rests on: a
  perennial cell is never swept but also never **scanned**.

### Fixed

- **The same attribute name had a different address in each `ProtoSpace` unless
  it fitted in the pointer word.** An attribute key is the address of an interned
  symbol, and protoCore embeds a short ASCII string in the pointer word
  (`INLINE_STRING_MAX_BYTES == 6`), so a 5-byte name matched across spaces by
  accident while a 7-byte one missed with **no error at all** — `getAttribute`
  returned `PROTO_NONE`, which is also a legitimate value. Half-global identity
  with silent partial failure.
- **A module loaded through a provider called directly was rooted only inside the
  providing runtime.** It reached neither `SharedModuleCache` nor any
  `moduleRoots`, so destroying that runtime while an importer still held its
  values dropped the only anchor. `ProtoSpace::registerModule` closes it.
- **`provider:st/counter_lib` and a local `counter_lib` were one module**, because
  the cache key was the path with the provider prefix stripped, with the first
  load winning for both.
- **A `ModuleRootTable` entry could outlive the `ProtoSpace` it names**, and the
  allocator can hand a later `ProtoSpace` the same address, whose collector would
  then match the dead space's entries by owner and trace cells in a heap with no
  owner. `~ProtoSpace` calls `purgeSpace()` after its GC thread has been joined.
  Found by a test, not by argument.

### Unchanged, on purpose

- **Tuple interning stays per space**, with a test that keeps it that way
  (`GlobalInterning.TupleInternerStaysPerSpace`) and the reasoning in
  `TupleInterner`'s doc block. Intern globally what is keyed by **content**; keep
  per-space what is keyed by **address**. A symbol's key is its bytes, which are
  space-independent; a tuple's key is its element **addresses**, which are not, so
  a global tuple table would deliver no cross-space identity for the ordinary
  case at all — and the one case where it would alias (a tuple of
  now-globally-interned symbols built in two spaces) would put one collector into
  another's heap for no benefit.
- **Global interning does not make objects portable across spaces.** It fixes
  attribute *keys*. Prototypes, `PROTO_NONE`, the mutables tree and every
  per-space callback remain per space.

### ABI

`PROTOCORE_ABI_SOVERSION` **2 → 3**.

The `ProtoSpace` layout is **byte-for-byte identical** — no field added, removed
or reordered, and the public additions are one class and four non-virtual
methods. An `offsetof` program compiled against this tree's headers and against
the base commit's produces an empty diff.

The soname nevertheless moves, and that is the point: unlike every previous
protoCore change, **a stale embedder binary here links successfully and runs, and
is simply wrong about symbol identity in a multi-space process.** There is no
load-time error, no crash and no diagnostic — just a `getAttribute` that returns
`PROTO_NONE`. A soname bump converts that into a load-time error.

**A clean rebuild of every embedder is mandatory.** A stale binary links.

## [Unreleased]

### Added

- **CMake package configuration.** `install(EXPORT protoCoreTargets)` with the
  namespace `protoCore::`, a `protoCoreConfig.cmake` generated from
  `cmake/protoCoreConfig.cmake.in`, and a `SameMajorVersion`
  `protoCoreConfigVersion.cmake`. Consumers now use
  `find_package(protoCore 2.0 REQUIRED CONFIG)` and link
  `protoCore::protoCore`; the configuration also asserts that the library
  matching `SOVERSION` is present in the prefix, so a prefix whose CMake files
  outlived its library fails with a message instead of a link error. Before
  this, `install(TARGETS ... EXPORT protoCoreTargets ...)` named an export set
  that was never written out, so no consumer could tell 1.x from 2.x.
- **`lib/pkgconfig/protoCore.pc`**, generated from `cmake/protoCore.pc.in`,
  including a `soversion` pkg-config variable for consumers that are not CMake
  projects.
- **The NSIS installer records `Version`, `Soversion` and `InstallDir` under
  `HKLM\SOFTWARE\protoCore`**, so dependent Windows installers have something
  to test — a DLL carries no soname. Configured but unverified: no Windows host.

### Changed

- **`SOVERSION` is derived from the new `PROTOCORE_ABI_SOVERSION` variable**,
  which is also what the package configuration and `protoCore.pc` report, so a
  consumer's check and the file on disk cannot disagree.
- **The exported interface include directory uses `CMAKE_INSTALL_INCLUDEDIR`**
  instead of the hardcoded `include`, matching the install destination.
- **`CPACK_DEBIAN_PACKAGE_NAME` (`protocore`) and `CPACK_RPM_PACKAGE_NAME`
  (`protoCore`) are set explicitly** instead of relying on each generator's
  default casing. The resulting package names are unchanged.
- The Linux CPack branch now also prints a `STATUS` line when the DEB or RPM
  generator is *disabled*, so a packaging run that produced fewer artefacts
  than expected says why.

- **The two `ProtoMPSCQueue` stress tests now apply backpressure.** Both aborted
  on protoCore's OOM guard, and the diagnosis is that the tests, not the queue,
  were at fault: they pushed 8 x 1,000,000 (and 400,000) messages with no flow
  control at all, so the backlog — which is live, and which only the consumer
  can release — grew past whatever heap ceiling it was given.

  The evidence that decides it:

  * **The wall is not the queue's.** A control program with no queue anywhere,
    doing nothing but `ProtoContext::newList(n, items)` under the same
    412,144-cell ceiling, completes at n = 20,000 and runs out of memory at
    n = 30,000 — the same boundary, and the same reported live set (348,469
    cells), as the queue stress with the same in-flight bound. One `takeAll` of
    N items transiently allocates about `N*log2(N)` cells, because the bulk
    builder appends element by element and the whole path-copy trail stays in
    the consumer's young generation until the build ends.
  * **The failure tracks the ceiling, not the producer count.** Given 1.76 M
    cells the live set stops at 2.00 M; given 6.26 M it stops at 5.89 M. With
    the in-flight set bounded at 30,000, one producer fails exactly as eight do.
  * **Rate-limiting the producers removes it entirely.** The full 8 x 1,000,000
    run completes under the *same* 412,144-cell ceiling in 11.6 s with 398 GC
    cycles, 8,000,000 items consumed, none lost, none duplicated, per-producer
    FIFO intact.

  Recorded for the record, because it is real and shapes how an embedder must
  size a mailbox: once the backlog has filled the heap the system is in a
  genuine circular wait, and the OOM abort is the only exit. At the abort
  (gdb, `thread apply all bt`) all eight producers were parked in
  `ProtoSpace::waitForHeapHeadroom` inside `push`, the consumer was parked in
  the same wait inside `newList` inside `takeAll` — unable to allocate the list
  whose completion was the only thing that could have released the backlog —
  and the GC thread was idle with nothing to reclaim. **The heap-ceiling
  protocol is unchanged; sizing is the caller's job.** The rule the numbers
  give is that a mailbox's in-flight set must stay well under the point where
  `N*log2(N)` approaches the heap ceiling.

  Both tests now bound the in-flight set (10,000 items for the 8 x 1M stress,
  250 for the heavier mark-race probes), wait inside an `UnmanagedScope` so a
  throttled producer never delays a pause, and carry an abort flag so a
  consumer that gives up can never leave a producer blocked and hang the join.
  The in-loop `ASSERT`s that could return from the test body with producers
  still running are replaced by counters checked after the join. Two new guards
  keep the result honest: the collector must complete at least ten cycles, and
  the in-flight bound must actually have bound at least once — a bound raised
  until it stops binding would silently restore the unbounded test.

### Fixed

- **`ProtoMPSCQueue::takeAll` no longer holds the world stopped for the length
  of the batch it drains.** With `newList(n, items)` fixed (below), what was
  left of the pause was `takeAll`'s own chain walk and reversal: two O(batch)
  loops that make no protoCore call, and therefore never reach a
  stop-the-world poll, however far outside a critical section they run. The
  pause was still linear in the batch — measured medians of **338 us** at
  50,000 items and **3,731 us** at 400,000, against a flat 27-34 us for a
  plain bulk build of the same sizes. PMQ-SPEC section 3 constraint 1 (no
  stop-the-world work proportional to queue length) was not met.

  Both loops now call `parkForStopTheWorld` every 64 nodes — the park-only
  half of `safepoint()` that the `newList` fix factored out, matching
  `allocCell`'s every-64-allocations cadence. After the fix the same medians
  are **32 us** at 50,000, 32 us at 100,000, 31 us at 200,000 and **27 us** at
  400,000: flat, and level with the plain bulk builder. Constraint 1 is met.

  The poll is placed strictly *after* the publish window (read epoch → maybe
  release → load `head` → fill and publish the retain cell → detach) and after
  its `CriticalSection` has been destroyed. Nothing was added inside that
  window, which is what keeps ABA impossible by construction (PMQ-SPEC section
  7): reusing the address loaded from `head` would still require a sweep
  between that load and the CAS, hence a pause, hence this thread parking
  between them — which it still cannot do. Parking in the walk is safe because
  nothing the walk needs lives only in a C++ local: the nodes hang off the
  retain cell this `takeAll` already published onto `retained`, and the items
  hang off the nodes.

  `MPSCQueueGC.LargeDrainDoesNotBlockStopTheWorld` now measures two batch
  sizes a factor of four apart in one run and asserts the pause does not grow
  with the batch, which is what constraint 1 actually forbids; the previous
  single-size bound is kept as a sanity check. `parkForStopTheWorld` moved
  from an anonymous namespace in `core/ProtoContext.cpp` to a protoCore-
  internal declaration in `headers/proto_internal.h`. No public API or ABI
  change.

- **`ProtoContext::newList(n, items)` no longer holds the world stopped for the
  length of the list it builds.** The bulk builder wrapped its whole O(n) AVL
  construction in a `ProtoContext::CriticalSection`. A thread inside a critical
  section never parks, so the collector could not begin its stop-the-world
  phase until the last element was in: the pause grew with the size of the
  list. Measured on a 100,000-element build with a collection requested against
  it, the stop-the-world pause (phases P1 + P2) falls from a median of **39 ms**
  to **31 us**; the collector's own instrumented P1 total over a run falls from
  about 48 ms per cycle to under 0.25 ms per cycle. No embedder API changes and
  no ABI change.

  The section is replaced by an anchor: every intermediate of the build is
  parked in `ProtoContext::pendingRoot`, which the stop-the-world root scan
  reads, so a collection landing mid-build traces the spine the loop is
  standing on from a real root. The slot's previous occupant is saved and
  restored, the same discipline `ProtoObject::processOwnAttributes` uses. The
  heap-ceiling backpressure the section's constructor took at depth 0 is kept,
  at the same point in the control flow — before the first allocation, with
  nothing half-built — exactly as `newStringFromUTF8` keeps it.

  Callers are unaffected in what they may pass, with one contract made
  explicit: a collection can now run while `newList` is executing, so elements
  the caller supplies must be reachable from a GC root, as they must be around
  any other allocation. Elements freshly built in a live context are on that
  context's young chain and therefore already safe.

  Cost: within 1% at 10,000 and 100,000 elements; about 6% on a 1,000-element
  build (least-contended sample of 144), for the anchor store per element and a
  stop-the-world poll every sixteen.

  New tests in `test/BulkListBuildTests.cpp` cover both halves: elements
  survive collections forced during the build (single-threaded under a hard
  heap limit, and with three concurrent builders), a value named by
  `pendingRoot` survives once its context's young generation has been
  submitted, and the stop-the-world pause during a large build is bounded well
  below the duration of the build.

## [2.1.0] - 2026-09-23

### Added

- **`ProtoMPSCQueue`** — a mutable, lock-free, multi-producer /
  single-consumer FIFO of `ProtoObject*` items whose contents the collector
  traces (spec `protoScala/docs/platform/PMQ-SPEC.md`). `push` is lock-free,
  O(1) and allocates one cell; `takeAll` returns every queued item in push
  order as an immutable `ProtoList`; `isEmpty` is a snapshot. One pointer
  tag (28) for the handle, three `CellType`s, a dedicated prototype, and
  `ProtoContext::newMPSCQueue` / `ProtoObject::isMPSCQueue` /
  `ProtoObject::asMPSCQueue`.

  It is the shared actor mailbox of protoScala Phase 5, protoClojure and
  protoST, and it closes protoClojure's unrooted-payload defect: a queued
  message, its arguments and its reply future become GC roots.

  **It adds nothing to the stop-the-world pause** beyond one O(1) global
  prototype root, needs no write barrier and changes no collector phase.
  Correctness under concurrent marking rests on two orderings, proved and
  documented in `core/ProtoMPSCQueue.cpp` and `docs/GarbageCollector.md`:
  `processReferences` loads `head` before `retained`, and `takeAll`
  publishes its retain cell before it detaches a chain.

  Caller contract worth repeating: the queue must stay reachable for the
  duration of a call, and each producer turn should use its own
  `ProtoContext`, exactly as every other protoCore allocation does — a
  context owns its young generation until it is destroyed.

  > **SUPERSEDED — both open items below were closed in the same merge
  > (`f60baf11`).** The two blocks that follow are kept as the record of what
  > was true when `ProtoMPSCQueue` first landed, but neither still holds:
  > `takeAll`'s pause is no longer proportional to the batch (see "`takeAll` no
  > longer holds the world stopped for the length of the batch it drains" under
  > *Unreleased → Fixed*; PMQ-SPEC §3 constraint 1 is now **met**), and the two
  > stress tests no longer abort on the OOM guard (see "The two
  > `ProtoMPSCQueue` stress tests now apply backpressure" under *Unreleased →
  > Changed*; the fault was in the tests, not the queue). Read both blocks
  > below as history, not as current status.
  >
  > Note for the maintainer: the entries currently under `[Unreleased]` are all
  > ancestors of `f60baf11` and so shipped *as part of* 2.1.0. Whether to fold
  > them into this section or cut a 2.1.1 is a release-numbering decision left
  > to you; nothing in the code depends on it.

  **Known limitation (recorded when the queue first landed; now fixed — see the
  note above).** With the
  bulk-builder fix above in place, the stop-the-world pause during a
  200 000-item drain falls from a median of 88 ms (min 82 ms, max 330 ms
  over 9 samples) to about 2 ms (min 22 us, max 5.3 ms) —
  `MPSCQueueGC.LargeDrainDoesNotBlockStopTheWorld`. What remains is in the
  queue, not in the builder: `takeAll` walks the detached node chain into a
  vector and reverses it without making any protoCore call, so that loop
  never polls the stop-the-world flag and the pause is still linear in the
  batch (340 us at 50 000 items, 3.67 ms at 400 000, a flat ~0.7% of the
  drain, against a flat 27-34 us for a plain bulk build of the same sizes).
  PMQ-SPEC §3 constraint 1 was therefore **not met yet at this point**. `push`
  is unaffected, and a consumer that drains often keeps its batches small.
  (Closed later in the same merge: the two loops now poll every 64 nodes and
  the pause is flat at 20-36 us from 50,000 to 400,000 items.)

  **Also open at this point (since closed — see the note above).** On top of
  the new builder,
  `MPSCQueueConcurrency.EightProducersOneConsumerLoseNothingAndDuplicate-
  Nothing` and `MPSCQueueGC.PushAndTakeAllDuringConcurrentMarking` abort
  with protoCore's out-of-memory guard. The consumer stops draining (the
  probe in `.agent_scratch` shows `consumed` frozen at 5 732 while
  `produced` runs to 54 600) and the backlog fills whatever heap it is
  given: the live set at the abort tracks the ceiling (320 k cells at a
  302 k ceiling, 1.25 M at a 1.26 M ceiling). Before the builder fix these
  tests passed, but they were not testing what they claimed — with the old
  builder the collector completed **zero** cycles at 10 000 and 50 000
  pushes per producer and the heap ran to 2.5 M cells against a declared
  302 k ceiling, so the ceiling was never enforced. Whether the fix belongs
  in the queue, in the tests' unbounded mailbox under a hard cap, or in the
  heap-headroom back-pressure is a maintainer decision.

### Changed

- `ProtoSpace` gains one field (`mpscQueuePrototype`). The soname stays
  `libprotoCore.so.2`, so **every embedder must still be rebuilt from
  clean**: a stale binary would use the old layout.
- Pointer-tag budget: used 0-28 (29), free 29-63 (35).

### Verified

- **The mandatory clean rebuild of every embedder was carried out against this
  merge (`f60baf11`) on 2026-09-23** (PMQ-SPEC §6 step 2), each one confirmed by
  `ldd` to resolve `libprotoCore.so.2` from the workspace build and not the
  stale 1.0.0 in `/usr/local/lib`:

  | Project | Result | Baseline |
  |---|---|---|
  | protoCore | 438/438 | 438/438 |
  | protoPython | 582/583 | 560/561 (count has since grown) |
  | protoJS | ctest 34/34, conformity 5/5, test262 `built-ins/{Object,Reflect,Proxy}` 3619 passed | 33/33, 5/5, 3619 |
  | protoST | 833/833 | 833/833 |
  | protoClojure | 383/383 | 383/383 |
  | protoScala | 694/694 | 694/694 |

  No regressions. protoPython's single failure is the pre-existing
  `protopy_import_site`, which a `.pth` in a sibling `venv/` triggers. The
  test262 subset matches its baseline exactly with **no newly failing test**;
  two tests newly pass, from protoJS's own integrity-levels fix.

- **protoScala's mailbox seam now selects the queue.** With `newMPSCQueue`
  present in `headers/protoCore.h`, protoScala's configure reports "actor
  mailboxes on protoCore ProtoMPSCQueue", `protoscala --version` reports
  `(actor mailboxes: ProtoMPSCQueue)`, and `nm -uC` shows the binary
  referencing `ProtoMPSCQueue::push`, `takeAll`, `isEmpty`, `asObject`,
  `ProtoContext::newMPSCQueue` and `ProtoObject::asMPSCQueue`. The CAS-list
  fallback is no longer compiled in. PMQ-SPEC §6 step 3 is unblocked.

## [2.0.0] - 2026-09-23

A major release that merges two independent lines of work:

- **`ProtoMap`** and the shared hashed-collection helper — the persistent map
  from language objects to values every runtime on the platform was building
  for itself (`feature/pslo-p1`, spec
  `protoScala/docs/platform/PROTOMAP-SPEC.md`).
- **The parent-chain lookup fixes** — `isInstanceOf`, `hasParent`,
  `hasAttribute`, `getAttributes`, `getAttribute`, `newChild` and `setParents`
  (`feature/descendant-of`, spec
  `protoScala/docs/platform/ISINSTANCEOF-FIX.md`).

### Upgrading from 1.2.0 — read this first

**The ABI version goes from `SOVERSION 1` to `SOVERSION 2`** (library
`libprotoCore.so.2.0.0`, soname `libprotoCore.so.2`). The `ProtoSpace` layout
changed (a new `mapPrototype` root and the concurrent-mark tables) and the
lookup and `setParents` semantics changed, so **every embedder must be rebuilt
from clean**. The soname bump is deliberate: a stale embedder binary now fails
to load with a missing-soname error instead of linking against an
incompatible layout and crashing at some later, unrelated point.

Four behaviour changes are visible to embedder code that was correct against
1.2.0. None of them is a bug being reintroduced; each is a previously wrong
answer becoming right, and each can change what an embedder observes:

1. **`setParents` flattens the chain.** The installed chain is now the listed
   parents, de-duplicated and in the given order, followed by every ancestor
   of each listed parent that is not already present (each parent's own chain
   order, in listed-parent order). Previously the chain held only what the
   caller listed. Consequences: `getParents()` on the result returns MORE
   entries than were passed (a caller that compares its list against
   `getParents()` for equality will now see a longer list); attribute
   precedence between two ancestors that were previously only reachable
   through different intermediate parents is now decided by this flattened
   order; and an object's ancestors are all directly visible to
   `hasParent`/`isInstanceOf` without a recursive walk. A list that already
   contains every ancestor of every listed parent (a full linearization)
   installs exactly as given — for those callers the change is a no-op.
2. **`setParents` with the receiver among the new parents is a silent no-op
   for that entry** instead of throwing `std::invalid_argument`. Nothing in
   the embedders catches that exception, and the check it replaced only ever
   caught a direct one-hop self-reference, never a longer cycle — skipping is
   exactly as complete a guard, without an exception embedders must catch.
3. **`isInstanceOf` and `hasParent` see all parents, with no step cap, and
   resolve a mutable receiver's current snapshot.** The old `isInstanceOf`
   gave up after 50 steps and answered `PROTO_FALSE`, and both read a mutable
   object's birth-state chain, so parents added later through
   `addParent`/`setParents` were invisible. Deep hierarchies and mutable
   receivers now answer correctly; code that relied on the old `false` (for
   example as a depth guard) will see `true`. The same cap removal applies to
   `getAttribute`, `hasAttribute` and `getAttributes`, which now merge the
   whole flattened chain.
4. **`newChild` captures the prototype's CURRENT chain.** A child of a mutable
   prototype used to inherit the prototype's birth-state ancestors; it now
   captures whatever the prototype's chain is at the moment `newChild` is
   called.

**Known embedder breakage, to be fixed in those repositories, not here:**

- **protoJS** — its parent-chain integrity markers assume `setParents`
  installs exactly the list it was given, so the flattened chain trips them.
- **protoPython** — its metaclass isolation relies on `newChild` capturing the
  prototype's birth-state chain and on `setParents` not flattening; with the
  corrected semantics a metaclass's ancestors become visible on instances that
  previously did not see them.

Both are consequences of protoCore answering correctly where it used to answer
wrongly; the fixes belong in protoJS and protoPython.

### Added
- **`ProtoMap`** — a persistent AVL map
  identical to `ProtoSparseList` except that its key is a `const ProtoObject*`
  the garbage collector traces: an object referenced only as a key stays
  alive. Keys are ordered and compared by their word (identity, tag included);
  embedded values (SmallInteger, booleans, chars, None) are valid keys and are
  never traced; `nullptr` is not a valid key. `getAt` returns `nullptr` for an
  absent key, so a stored `PROTO_NONE` stays distinguishable. Small inline
  form up to three entries, AVL beyond, exactly like `ProtoSparseList`, whose
  algorithms it shares through `core/SparseListAlgorithms.h`
  (`ProtoSparseList`'s behaviour, API, ABI and performance are unchanged).
  New API: `ProtoContext::newMap`,
  `ProtoObject::isMap` / `asMap`,
  `ProtoSpace::mapPrototype`, `ProtoMapIterator`.
  Uses one new pointer tag (27); the tag table now records the platform tag
  budget and the maintainer-approval rule. ABI change: every embedder must be
  rebuilt. Specification: protoScala/docs/platform/PROTOMAP-SPEC.md. The
  performance gate and the ASan run, left pending on the branch, were both
  run at merge time — see "Performance gate and sanitizer run (2026-09-23)"
  under Performance below.
- **Hashed-collection helper** — `KeySemantics`, `hashedPut`, `hashedGet`,
  `hashedRemove`, `hashedForEach` over `ProtoMap`: identity keys
  are stored directly; value-equality keys are stored under a SmallInteger
  hash word with a flat `[k0, v0, k1, v1, …]` bucket list for collisions.
  Language callbacks run outside GC critical sections.
- **`ProtoObject::processOwnAttributes`** — walks an object's OWN attributes
  as `(name, value)` pairs, the enumeration the public API was missing.
  Attribute keys are stored as the interned symbol pointer reinterpreted as
  an integer (`getAttribute` computes `reinterpret_cast<uintptr_t>(name)`),
  so `getOwnAttributes()` returns a sparse list whose keys are opaque
  numbers: an embedder could read an object's own values but could not
  recover their names. That is what blocked protoJS from copying or stamping
  an object's own attributes. The callback receives the canonical symbol for
  each name, so it compares equal by identity to the symbol the attribute was
  set with, and `getAttribute` on it returns the value the callback was
  handed. Casting the stored key back to a symbol is sound because symbols
  are perennial: interned with a null `ProtoContext`, allocated outside the
  GC, never marked, swept, moved or evicted.

  The walk resolves the mutable snapshot exactly as `getAttribute` does,
  visits own attributes only (never the prototype chain), reports a
  `PROTO_NONE` value like any other and a removed attribute not at all,
  allocates nothing (`ProtoContext::allocatedCellsCount` is unchanged by it),
  and runs the callback OUTSIDE any GC critical section — so the callback may
  allocate, reach a safepoint and run arbitrary embedder code. For the
  duration of the walk the snapshot is anchored in
  `ProtoContext::pendingRoot` (saved and restored around the call), which is
  what keeps the attribute tree reachable while the callback runs. **The
  order in which attributes are visited is unspecified.**

  Tests: `test/AttributeEnumerationTests.cpp` (12 cases).
- **`PROTOCORE_HEAP_LIMIT_CELLS` environment variable** — a `ProtoSpace` reads
  it once it is fully constructed and calls `setHeapLimits(soft, hard)`:
  - `<hard>` sets the hard ceiling only;
  - `<soft>,<hard>` sets both limits, in cells;
  - unset, `0` or a hard part of `0` means no limit, which stays the default;
  - any other value is ignored without a message.

  Without a limit protoCore starts no collection cycle by itself, and
  embedders do not call `setHeapLimits`. This variable is how an embedder
  (protopy, protost, protoclj) runs under a low memory limit, for example so
  that its GC-stress tests exercise collection:
  `PROTOCORE_HEAP_LIMIT_CELLS=500000`. The README gains a "Runtime
  Configuration" table of every environment variable protoCore reads.

  Tests: `test/HeapLimitEnvTests.cpp`.
- **`ProtoObject::partialCompare`** — an IEEE partial-order comparison that
  returns `std::partial_ordering`. Numbers compare by exact value across
  SmallInteger, LargeInteger and double (-0.0 is equivalent to 0.0 and to 0);
  any NaN is unordered with everything, itself included, so `<`, `<=`, `==`,
  `>=` and `>` against 0 are false and `!=` is true; strings compare by
  content; other pairs are equivalent when identical and unordered otherwise.
  `compare` now delegates to it and keeps its behaviour: an unordered numeric
  pair (a NaN) still compares as 0, and non-numeric pairs still order by
  address. Embedders that implement comparison operators with `compare`
  (where `NaN = 1` and `NaN <= 1` are true) can switch to `partialCompare`.
  The public header now includes `<compare>`; adding a non-virtual member
  does not change any layout.
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
- **`ProtoSpace` layout** — `ProtoSpace` gains the `gcMutableSnapshot[]` table
  and a `tupleInterner` pointer. Embedders built against 1.2.0, or against an
  earlier build of this release, must be rebuilt; no source change is required.
- **No GC trigger on free-list exhaustion without a heap limit** —
  `getFreeCells` refills from the OS without waking the collector. With no
  heap limit configured (the default), collections are started by
  `triggerGC()` and at shutdown; configured soft and hard limits drive
  collections as before. Previously every exhaustion woke the collector, which
  then waited in the stop-the-world quorum for threads that never reached a
  safepoint: in a protoST actor benchmark with 8 workers it waited 2.27 s of a
  2.49 s run and swept once, at the end.
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
- **`ProtoObject::setParents` self-reference is now a silent no-op
  (the offending entry is skipped) instead of throwing
  `std::invalid_argument`; the true termination argument was corrected
  in every place it was documented.**

  Two separate problems, one fix. First: nothing in the embedders this
  branch was validated against catches `std::invalid_argument` from
  `setParents`, and it is reachable from ordinary code — e.g.
  protoPython's metaclass-resolution fallback does not re-check
  `metacls != targetClass` before a `setParents`-based rebuild, so a
  pattern that ends up there can raise uncaught. Second: the check this
  replaces was never a complete cycle guard to begin with — it only ever
  caught a DIRECT reference back to the receiver (a listed parent equal
  to it, or an ancestor found while walking a LISTED parent's own
  one-level chain). It does not, and structurally cannot without
  unbounded work, catch a longer chain of references built up across
  several SEPARATE `setParents` calls on different mutable objects:
  `a.setParents(ctx,[b])`, then `b.setParents(ctx,[c])`, then
  `c.setParents(ctx,[a])` — none of these three individual calls sees
  enough to reject the third. Continuing to throw for only the narrower,
  directly-detectable case was therefore incomplete protection with all
  of the uncaught-exception downside.

  Skipping the offending entry — omitting it from the flattened list,
  while every OTHER listed parent/ancestor is still applied normally —
  is exactly as complete a guard as throwing was (it prevents THE SAME
  set of direct cases; the longer, cross-call case was never caught
  either way), without the exception. It also matches `addParent`, which
  already tolerates `obj->addParent(ctx, obj)` as a silent no-op (via
  `hasParent`'s `target == this` short-circuit). A single-entry list
  whose one entry is the receiver itself ends up empty, same as passing
  an empty list directly — already-documented `setParents` behaviour, not
  a new case.

  **The true termination/safety argument** (corrected everywhere the old,
  false one was written down — the header doc comments for
  `getAttribute`/`setParents` and this file): every chain-lookup method
  (`getAttribute`, `hasAttribute`, `isInstanceOf`, `hasParent`,
  `getAttributes`) is a single-level walk of ONE receiver's own,
  already-built `ParentLinkImplementation` list — built once, forward
  only, by `newChild`/`addParent`/`setParents`, never mutated afterward —
  and none of them ever follows a visited entry into THAT entry's own
  separate chain. Termination therefore never depended on "no
  self-reference of any shape can exist" (false, as the three-object
  example above demonstrates); it depends only on each individual list
  being finite, which is guaranteed by how it was built, regardless of
  what any OTHER object's chain happens to reference.

  Tests: `test/SetParentsFlattenTests.cpp` — the direct and
  two-mutable-object self-reference cases now assert a skip, not a
  throw (`MutualMutableSelfReferenceIsSkipped`,
  `DirectSelfReferenceIsSkippedLeavingAnEmptyChain`,
  `SelfReferenceAmongOtherParentsOnlySkipsItself`,
  `ImmutableReceiverInItsOwnNewParentsListIsNotSkipped`), plus a new
  `ThreeObjectCycleIsNotDetectedButCausesNoHarm` reproducing the
  three-`setParents`-call case above and confirming it does not crash,
  hang, or make any of the four lookup methods report a false ancestor.

- **`ProtoObject::getAttributes` aborted the process on a non-object
  parent — e.g. a heap `ProtoString` added via `addParent`, or a
  SmallInteger installed via `setParents` (neither is rejected at
  construction time: `addParent` only rejects an EMBEDDED value, and
  `setParents`'s own flattening has no tag filter at all). Its rewrite
  (an earlier round in this branch) called
  `toImpl<const ProtoObjectCell>(ancestor)` on every chain entry with no
  tag check first; a non-object tagged pointer does not address a
  `ProtoObjectCell`-shaped `Cell`, so dereferencing it through that cast
  aborted the process (a debug-build `toImpl` type assertion, `SIGABRT`;
  a release build would corrupt memory instead).**

  Fixed by mirroring `getAttribute`'s own, already-correct policy for
  this case (its chain-navigation loop already redirects a non-object
  `currentPointer` to `currentPointer->getPrototype(context)`, one hop,
  rather than dereferencing it as an object): a non-object chain entry in
  `getAttributes()` now contributes its OWN prototype's OWN attributes —
  one hop, not the prototype's further chain — instead of crashing.
  `isInstanceOf`/`hasParent` were never at risk (they only ever compare
  chain-entry pointers, never dereference one as a `ProtoObjectCell`);
  `hasAttribute` already had the same non-object handling `getAttribute`
  has, being a direct port of its chain-navigation loop.

  `flattenParentsOrder`'s own policy is stated explicitly where the
  asymmetry lives: step 1 (the listed parents) accepts any entry
  regardless of tag, matching `addParent`/`setParents`; step 2 (walking
  each listed parent's OWN chain) cannot walk a non-object entry's chain
  (it does not have one), so it is skipped as a source of further
  ancestors there — but is NOT removed from the flattened list step 1
  already added it to.

  Tests: `test/NonObjectParentTests.cpp` (8 cases) — a heap-string
  parent via `addParent` and a SmallInteger parent via `setParents`, then
  `getAttributes`/`getAttribute`/`hasAttribute`/`isInstanceOf` against
  both, confirmed not to crash and to answer through the entry's own
  prototype, plus a not-found lookup past a non-object entry still
  terminating cleanly.

- **`ProtoObject::newChild` resolved a mutable prototype's snapshot
  BEFORE opening its `ProtoContext::CriticalSection`, leaving the
  resolved snapshot unprotected across a GC park.**

  `CriticalSection`'s constructor calls `heapLimitCheckpoint()` at the
  outermost nesting depth, which can block waiting for a GC cycle when
  the heap is over its configured limit (`PROTOCORE_HEAP_LIMIT_CELLS`).
  `newChild` called `resolveOwnCell(context, this)` — which can itself
  resolve a mutable snapshot — and held the result in a C++ local (`oc`)
  across that constructor call. If a GC cycle ran during that park and
  nothing else kept the resolved snapshot reachable, `oc` could be left
  dangling by the time the critical section's body dereferences
  `oc->parent`. `getParents`/`getFirstParent`/`getAttributes` already
  open their `CriticalSection` before resolving anything, precisely to
  avoid this; `newChild` now does too — the same three-cell allocation
  it always protected is still covered, just with the mutable-snapshot
  resolve moved inside the section as well.

  This needs `PROTOCORE_HEAP_LIMIT_CELLS` set low enough to actually
  trigger a checkpoint park during the window between resolve and use to
  manifest, which made it impractical to turn into a deterministic
  regression test in the time available for this round; flagging this
  here rather than shipping a flaky or non-reproducing one.

- **`ProtoObject::getAttribute` no longer caps its chain walk at 500
  steps — no lookup or traversal method in protoCore has a depth cap any
  more.**

  `getAttribute` gave up (`iterationCount > 500`) and returned
  `PROTO_NONE` ("not found") for an attribute living further than 500
  own-chain entries from the receiver — a false negative for a perfectly
  good hierarchy, the same class of bug `isInstanceOf`'s old 50-step cap
  and `hasAttribute`'s old 50-step cap both had (both already fixed in
  earlier rounds). This was the one depth cap left in any lookup path,
  and the one remaining place `getAttribute` could disagree with
  `isInstanceOf`/`hasParent`/`hasAttribute`/`getAttributes` (all already
  uncapped). It is gone: all five now always agree, at any depth.

  Termination without a cap is guaranteed by construction, not by a
  limit, and was already true before this fix — removing the cap adds no
  new risk: every one of these walks only ever follows ONE receiver's
  own, already-built `ParentLinkImplementation` list (built once, forward
  only, by `newChild`/`addParent`/`setParents`, never mutated afterward),
  and never follows a visited entry into THAT entry's own separate
  chain — so it is always a single forward pass over one strictly finite
  list, regardless of what any OTHER object's chain references. (This
  replaces an earlier, incorrect version of this paragraph that claimed
  `setParents` rejects any input that could create a cycle at all — it
  does not; see the self-reference entry above for what it actually
  catches. The argument this paragraph needs never depended on that
  claim: it only needs that a single walk never crosses from one chain
  into another, which was true throughout.)

  Surveyed every lookup/traversal path in `core/*.cpp` for any other
  step/depth-limit constant: none remain. The one numeric bound left
  anywhere near an attribute walk is `processOwnAttributes`'s
  `kMaxDepth = 64` stack for its OWN in-order AVL traversal — a
  different kind of bound entirely: it is not a parent-chain depth cap
  (it never gives up on the SEARCH; it bounds the recursion-free
  in-order walk of ONE object's own attribute tree), and it is not
  arbitrary — the sparse list's `size` field is 24 bits, so a balanced
  tree of that many entries is at most ~35 deep, making 64 a
  mathematically safe upper bound, never an approximation that could be
  exceeded by a legitimately larger hierarchy.

  Tests: `test/GetAttributeNoCapTests.cpp` (5 cases) — the old cap pinned
  first (depths 501 and 2000 both returned `PROTO_NONE` for a root
  attribute against the pre-fix code, confirmed before removing the
  cap), then found at depth 501 and depth 2000, agreement with
  `hasAttribute`/`isInstanceOf`/`getAttributes` on the same 900-level
  chain, a not-found lookup on a 2,000-level chain still terminating as
  `PROTO_NONE`, and shallow own/inherited baselines. Also updated
  `test/HasAttributeChainTests.cpp`'s `DivergesFromGetAttributeBeyond500
  Levels` (renamed `AgreesWithGetAttributeBeyond500Levels`) now that the
  divergence it pinned no longer exists.

- **`ProtoObject::getAttributes` (the merged-attribute-view snapshot) now
  walks the receiver's whole flattened chain instead of recursing into
  only the first parent link — a second or later DIRECT parent's
  attributes are no longer silently dropped from the merge.**

  `getAttributes()` recursed as `pl->getObject(context)->getAttributes
  (context)` on `oc->parent` — the FIRST link of the receiver's own
  chain — and never followed `pl->getParent(context)` (the chain's
  remaining entries) at all. For an object built via more than one
  `addParent` call (a diamond) or via `setParents` with more than one
  listed parent, every attribute that lived only on the second-or-later
  parent was invisible through `getAttributes()`, even though
  `getAttribute`/`hasAttribute`/`isInstanceOf` (all fixed in earlier
  rounds to walk the receiver's own chain directly) already saw it — the
  three disagreed.

  `getAttributes()` is now the same iterative walk of the receiver's own
  chain those methods use, with an explicit **merge order (shadowing
  rule)**, stated in its header doc comment: own attributes first, then
  the chain head to tail; a key already set by a nearer entry is never
  overwritten by a farther one. This is exactly `getAttribute`'s/
  `hasAttribute`'s own "first match wins" precedence, so all three always
  agree on which value a key resolves to — including the ordering
  divergence between `addParent` (interleaves a parent's own ancestors
  right after that parent) and `setParents` (batches all missing
  ancestors after all listed parents), which now produces the same
  `getAttributes()` result as `getAttribute` in both cases. No step cap
  (unlike `getAttribute`'s 500-step one — the chain is walked in full),
  and no more C++ recursion depth proportional to chain length either
  (the old recursive-into-first-parent shape, applied to a very deep
  single-parent-per-level chain, would recurse once per level).

  Tests: `test/GetAttributesMergeTests.cpp` (10 cases) — the old
  first-parent-only bug pinned first (an `addParent` diamond and a
  multi-parent `setParents` list both dropped the second parent's
  attribute against the pre-fix code, confirmed before writing the fix),
  then own-attributes-only and single-parent-chain baselines, the
  shadowing precedence (own over any ancestor, nearer over farther), the
  exact `addParent`-vs-`setParents` ordering-divergence case agreeing
  with `getAttribute`, a 1,000-level single-parent chain, a mutable
  receiver, and agreement with `getAttribute`/`hasAttribute` on a
  diamond.

- **`ProtoObject::hasAttribute` now walks the flattened chain the way
  `getAttribute` does, allocation-free and with no step cap — fixing the
  same class of false-negative bug `isInstanceOf` had.**

  `hasAttribute` used a fixed-size (64-slot) sibling-stack DFS with an
  arbitrary 50-step cap and returned `PROTO_FALSE` — a false negative —
  for any hierarchy deeper than 50 links. It is now the same linear
  chain-navigation loop `getAttribute` uses (own attributes, then the
  chain head to tail), minus `getAttribute`'s attribute cache and its
  500-step cap: `hasAttribute` has no cap at all, and resolves a mutable
  receiver (and every mutable object visited along the chain) to its
  current snapshot exactly as `getAttribute` and the already-fixed
  `isInstanceOf`/`hasParent` do.

  At the time of this fix `getAttribute` still had its own separate
  500-step cap, so the two were not guaranteed to agree for very deep
  hierarchies; that cap is gone too now (see the later entry in this
  file) and they always agree, at any depth.

  Surveyed the other attribute-lookup helpers for the same defect:
  `hasOwnAttribute`, `getOwnAttributeDirect` and `processOwnAttributes`
  only ever probe the receiver's OWN attributes — no chain walk, no
  defect possible. `getAttributes()` (the merged-view snapshot) does walk
  the chain, but via true recursion into only the FIRST parent link — a
  different bug shape (missing siblings, not a step cap), fixed in a
  later round (see the entry near the top of this section, which also
  covers a separate crash the same rewrite introduced).

  Tests: `test/HasAttributeChainTests.cpp` (13 cases) — the old cap
  pinned first (a 60- and a 520-level chain both returned `PROTO_FALSE`
  against the pre-fix code, confirmed before writing the fix), then own/
  inherited/absent/`None`-valued baselines, an `addParent` diamond, chains
  past 50 and past 500 levels, agreement with `getAttribute` within its
  cap, the documented divergence beyond it, a mutable receiver (plain and
  via `newChild`), and a non-object receiver answered through its
  prototype.

- **`ProtoObject::newChild` now captures a MUTABLE prototype's CURRENT
  chain, not its birth-time chain — fixing instances of a mutable class
  that was re-parented after the class was made mutable.**

  `newChild` read the prototype handle cell's own `parent` field directly.
  For a mutable object that field is fixed at `newObject(true)` time and
  never updated in place (`addParent`/`setParents` publish a fresh state
  into the mutable shard instead), so `cls = newObject(true);
  cls->setParents(ctx, [base]); inst = cls->newChild(ctx)` silently built
  `inst` with NO ancestors at all: `inst->isInstanceOf(ctx, base)` was
  `PROTO_NONE` and `inst->getAttribute` never found any of `base`'s
  attributes. `newChild` now resolves the prototype to its current
  snapshot first (the same resolution `getAttribute`/`getParents`/
  `hasParent`/`isInstanceOf` already used), matching how every other
  chain-reading method treats a mutable object.

  The child's chain tail is still captured BY VALUE, once, at the moment
  of the `newChild` call: an instance created BEFORE a later re-parenting
  of its class does NOT retroactively gain the new ancestor; only
  instances created AFTER do. This is ordinary "capture at creation time"
  semantics, and matches the shape protoST's `addBehavior:` mechanism
  documents relying on for its own "future instances" contract
  (`protoST/src/primitives/object_prims.cpp`, the D21 "DOCUMENTED
  LIMITATION" comment) — that mechanism rebuilds the class as a fresh
  object rather than mutating an existing one, so it never depended on
  `newChild` observing a mutation of an EXISTING class object and is
  unaffected by this fix either way. What this fix DOES retire is the
  narrower "PROTOCORE CONSTRAINT" documented a few lines above that
  comment in the same file: mutating an EXISTING mutable class directly
  via `addParent`/`setParents` used to be invisible to instances created
  AFTER the mutation too (not only ones created before) — that half of the
  documented constraint no longer holds.

  Tests: `test/InstanceOfHasParentTests.cpp` — a `setParents`-built mutable
  class seen by a `newChild` instance, protoPython's own pattern
  (`newObject(true)` → `addParent` → `newChild`, both immutable and
  mutable children), and a mutable class re-parented after an instance
  already exists (the existing instance keeps the old ancestor, a new
  instance created afterwards gets the new one).

- **`ProtoObject::isInstanceOf` lost the "parentless object is an instance
  of `objectPrototype`" answer when its DFS-removal rewrite stopped
  bootstrapping from `getPrototype()` — restored.**

  A plain object with no parent chain of its own (never `newChild`'d,
  `addParent`'d or `setParents`'d anything) is, by convention, considered
  a descendant of the universal root `space->objectPrototype` —
  `getPrototype()` has always returned `objectPrototype` for exactly this
  case. The linear-walk rewrite searched the receiver's own chain directly
  and, for a chain-less receiver, found nothing and answered `PROTO_NONE`
  instead. `isInstanceOf` now applies the same fallback `getPrototype()`
  does, but ONLY at the top level for the receiver itself (an object WITH
  an explicit chain of its own is not implicitly rooted at
  `objectPrototype` unless its own construction put it there), and never
  for `objectPrototype` asking about itself (an object is not its own
  instance).

  Test: `test/InstanceOfHasParentTests.cpp` (`ParentlessObjectIsInstanceOf
  ObjectPrototype`, `ObjectPrototypeIsNotItsOwnInstance`,
  `ObjectWithExplicitChainIsNotImplicitlyRootedAtObjectPrototype`).

- **`setParents`'s DEDUPLICATION check is no longer O(n²), and flattening
  no longer allocates or re-runs inside the mutable CAS retry loop.**
  **Correction: this is narrower than an earlier version of this entry
  claimed** — see the numbers below; a `setParents` call whose listed
  parents each carry substantial ancestry of their own is still
  quadratic overall, just with a far smaller constant factor.

  The de-duplication check the flattening algorithm added (an entry
  equal to one already kept is dropped) was a linear scan of the
  accumulator built so far — O(n) per candidate, O(n²) total for n
  candidates that share no ancestry — running inside a GC critical
  section, and for a mutable receiver, inside its CAS retry loop (so a
  contested retry redid the whole O(n²) computation from scratch, even
  though the flattened chain never depends on the receiver's own current
  state). Measured before this fix: roughly 3 ms for a 4,000-entry list
  of independent (no shared ancestry) candidates.

  Fixed by: (1) a small open-addressing pointer set for the dedup check —
  O(1) amortised per candidate instead of O(current-size); (2) an inline-
  capacity-then-heap-fallback buffer (`SmallVector`/`ObjectPointerSet`,
  256/512 inline slots) for both the ordered accumulator and the dedup
  set, so the common case (a parent list of a few dozen to a couple
  hundred entries) never touches the heap at all; (3) moving the entire
  flattening computation (both the dedup pass and the ancestor-walk pass)
  OUTSIDE any GC critical section — it allocates no Cell, so it needs no
  GC protection — and outside the mutable receiver's CAS retry loop
  entirely, so a contested retry only rebuilds the cheap wrapping
  `ProtoObjectCell`, not the flattened chain.

  **What this fixes, and what it does not.** For N listed parents that
  share NO ancestry (step 2 does ~no work; the old cost was almost
  entirely the dedup scan in step 1), re-measured after this fix
  (average of 3 runs, `newObject(false)` release build):

  | N (listed parents) | before (reported) | after, immutable | after, mutable |
  |---:|---:|---:|---:|
  | 100  | — | ~0.008 ms | ~0.007 ms |
  | 1000 | — | ~0.12 ms  | ~0.10 ms  |
  | 4000 | ~3 ms | ~0.6–0.9 ms | ~0.5–0.7 ms |

  This shape is now roughly linear (a 4–6× improvement at N=4000). But
  step 2 itself — walking every LISTED parent's own chain to collect its
  ancestors — is inherent work: it must visit every (parent, own-chain-
  entry) pair at least once, and each check is now O(1) instead of O(n),
  but the NUMBER of pairs is not reduced. For N listed parents that
  together already form a complete linearization (e.g. N objects
  P₁..P_N with Pᵢ = Pᵢ₋₁.newChild(), listed in full as
  [P_N, ..., P₁] — every entry's own ancestors already listed elsewhere,
  the realistic "pass an existing MRO to setParents" shape), the total
  work is Σᵢ (i-1) = Θ(N²) checks regardless of the dedup fix. Measured
  (average of 3 runs, same build):

  | N (full linearization) | setParents time |
  |---:|---:|
  | 250  | ~0.56 ms |
  | 500  | ~2.1 ms  |
  | 1000 | ~8.5 ms  |
  | 2000 | ~43.5 ms |

  Each doubling of N costs roughly 4×: quadratic, as expected — the dedup
  fix made each of the Θ(N²) checks O(1) instead of O(current-size), a
  large constant-factor win (illustrated by the independent-parents
  table above), but it did not, and could not, change the Σᵢ shape this
  input pattern inherently requires.

- **Documented, explicitly, that `setParents` and `addParent` place a
  listed parent's own missing ancestors in DIFFERENT positions, which can
  flip attribute lookup precedence.** `setParents` appends ALL missing
  ancestors AFTER ALL listed parents; `addParent`, called once per parent,
  interleaves each call's own missing ancestors immediately after that
  call's parent — so `d.addParent(ctx, b); d.addParent(ctx, c);` and
  `d.setParents(ctx, [c, b])` (same listed parents, same order) can
  produce chains that disagree on which of two candidate ancestors'
  attributes wins. See `addParent`'s and `setParents`'s header doc
  comments for the worked example, and
  `test/SetParentsFlattenTests.cpp`'s
  `OrderingDiffersFromAddParentAndAffectsAttributePrecedence` for a case
  where the two constructions produce different `getAttribute` results
  for the exact same set of parents and ancestors.

- **Corrected the "`hasParent` doesn't see a mutable object's children's
  full ancestry" gap** — that gap was exactly the `newChild` bug fixed
  above; `hasParent` itself was already correct (it resolves a mutable
  receiver's current snapshot, and always did), it was just fed an
  incomplete chain by the old `newChild`. With that fixed, `hasParent`
  (like `isInstanceOf`) now answers the full, transitive ancestry
  question for every object, including instances of a mutable class. (At
  the time of this entry `getAttribute` still had a separate 500-step
  cap that was the one remaining gap; removed in a later round (see
  the entry near the top of this section).)

- **`ProtoObject::setParents` now flattens the chain it installs, so
  `ProtoObject::isInstanceOf` is a pure linear walk with no recursion, no
  cap and no allocation — for every object, with no exceptions.**

  This supersedes the entry below: `setParents` was, until now, the one
  construction path that did not flatten (it installed exactly the given
  list, without copying in each listed parent's own ancestors), which is
  why `isInstanceOf` originally needed a recursive fallback for chains it
  had touched. It no longer does.

  **`setParents`'s new chain**, in order: (1) the entries of the given
  list, in the given order, de-duplicated; (2) every ancestor of each of
  those listed parents — walking each parent's own chain in that parent's
  own order, in the same order the parents were listed — that is not
  already present. A list that already contains every ancestor of every
  listed parent (e.g. a full linearization) is unaffected by step 2 and
  installs exactly as given, in the exact same order — a no-op relative to
  the old verbatim behaviour.

  **Behaviour change**: `getAttribute` (and `hasParent`, and
  `isInstanceOf`) can now see a grand-parent's attribute through a
  `setParents`-built object that could not see it before — e.g.
  `x = obj.setParents(ctx, [p])` where `p` has its own ancestor `g` with
  attribute `a`: `x.getAttribute(ctx, a)` used to return `PROTO_NONE`
  (only `p`'s own attributes were visible) and now finds `g`'s value,
  because `g` is flattened into `x`'s own chain alongside `p`. This is
  intended: it is the same completeness `newChild`/`addParent` already
  guaranteed, now extended to `setParents`, and it is why
  `isInstanceOf`/`hasParent`/`getAttribute` now agree on what an object
  inherits, regardless of which construction path built its chain — WITH
  ONE EXCEPTION, corrected in a later round (see the entry near the top
  of this section): `getAttribute` used to still cap its walk at 500
  steps, so it could still disagree with the other two for a very deep
  chain.

  **Self-reference handling** (this originally threw
  `std::invalid_argument`; corrected to a silent skip in a later round —
  see the entry near the top of this section for why): a mutable object's handle is stable
  across mutation, so it is the only case where `setParents` could be
  asked to make an object its own ancestor — directly
  (`a.setParents(ctx, [a])`) or through another mutable object's chain
  (`a.setParents(ctx, [b])` then `b.setParents(ctx, [a])`, where `a`'s
  chain now contains `b`). An immutable `setParents` call can never
  create a real self-reference this way — it always builds a brand-new
  handle nothing could have referenced yet.

  Tests: `test/SetParentsFlattenTests.cpp` (10 cases: the no-op
  linearization property, a non-flat list being flattened, listed-parent
  order preservation with overlapping ancestors, de-duplication of a
  repeated listed parent, the `getAttribute` visibility change, agreement
  between `isInstanceOf`/`hasParent`/`getAttribute`, a mutable object
  after `setParents`, self-reference handling (a two-mutable-object case
  and a direct case), and an immutable receiver listing its own old
  handle — not a self-reference).

- **`ProtoObject::isInstanceOf` and `ProtoObject::hasParent` now walk the
  flattened parent chain directly instead of allocating or capping the
  search.**

  `isInstanceOf` used a depth-first walk with a fixed-size (64-slot)
  sibling stack and gave up after an arbitrary 50-step cap, returning
  `PROTO_FALSE` (a third, distinct value, never documented as part of the
  found/not-found contract) instead of the correct answer for any hierarchy
  deeper than 50 links. `hasParent` allocated a `ProtoList` via
  `getParents()` on every call just to test membership.

  `newChild`, `addParent` and (as of the entry above) `setParents` all
  guarantee that an object's own chain already contains every one of its
  ancestors as a direct entry, so `isInstanceOf` is now a single
  allocation-free linear scan of that chain, the same one `getAttribute`
  walks, with **no length limit and no recursion**. `hasParent` keeps its
  existing single-level contract (`target == this`, or a direct entry in
  the receiver's own chain) and is now just that scan without the
  `ProtoList` allocation — which, now that every chain is flat by
  construction, agrees with `isInstanceOf` on every ancestor, not only a
  direct one.

  Fixing this surfaced and corrected two bugs the old implementation had:
  `isInstanceOf` explored a receiver's own chain only through
  `getPrototype()`, which returns just the first entry, so a second or
  third parent added via `addParent` (e.g. the classic diamond,
  `chain=[C,B,A]`) was silently unreachable even though `hasParent`
  correctly reported it present; and `isInstanceOf` never resolved a
  mutable receiver's current snapshot (`getPrototype()` does not), so it
  answered false for every parent ever added to a mutable object. Both are
  an unavoidable consequence of scanning the receiver's own resolved chain
  directly instead of bootstrapping from `getPrototype()`.

  Tests: `test/InstanceOfHasParentTests.cpp` (15 cases, covering every
  chain-shaping construction path: `newChild`, `addParent` including the
  diamond case, `setParents`, `clone`, mutable objects after
  `addParent`/`setParents`, non-object receivers, and a 1,000-level
  `newChild` chain that used to hit the 50-step cap and now correctly
  returns `PROTO_TRUE`).
- **`ProtoObject::isByte` is now defined and exported.** It was declared in
  the public header but had no definition anywhere, so an embedder that
  called it failed to link. `nm -D --defined-only` on the shipped library
  listed 18 `ProtoObject` type predicates and `isByte` in neither the defined
  nor the undefined table; no commit in the repository ever added a body, and
  protoJS had already documented the workaround in
  `ProtoCoreNativeBindings.cpp` ("several others are declared in protoCore.h
  but not defined in the library").

  It answers **true for a SmallInteger whose value fits in one byte** — an
  EMBEDDED_VALUE of embedded type SMALLINT whose value is in the closed range
  `[-128, 255]`. protoCore has no distinct byte type (no `POINTER_TAG_BYTE`,
  no `EMBEDDED_TYPE_BYTE`; `fromByte(char)` is `fromInteger(char)` and
  `asByte` reads the low 8 bits of a SmallInteger), so the predicate answers
  "would this value survive the byte round trip", and that range is exactly
  the set that does: every `char` `fromByte` can encode plus the unsigned
  0..255 reading byte buffers use. `isByte(fromByte(c))` holds for every
  `char c`. It is false for integers outside the range, LargeIntegers,
  booleans, unicode chars, `PROTO_NONE`, strings, symbols, byte buffers
  (`isByteBuffer` is unrelated), doubles, methods and objects, and is safe on
  a null receiver.

  Tests: `test/AttributeEnumerationTests.cpp` (4 cases).
- **`ProtoObject::clone` now carries the own attributes and parents a MUTABLE
  receiver holds at the time of the call**, instead of the ones it was created
  with.

  **Cause.** A mutable object never writes back into its handle cell:
  `setAttribute` publishes a fresh state cell into the mutable shard table
  (built with `mutable_ref = 0`) and returns the SAME handle. `clone` read
  `oc->attributes` and `oc->parent` straight off that handle, skipping the
  `resolveMutableSnapshot` step that `getAttribute`, `getOwnAttributeDirect`,
  `getAttributes` and `isInstanceOf` all perform. The copy therefore carried
  the object's birth-time state — for a freshly created mutable object, an
  empty attribute table. protoJS's `protoCore.ImmutableObject({a:1})` came
  back as `{}`; `MutableObject`, `MakeImmutable` and `MakeMutable` share the
  same call and the same breakage, since all four use `clone` as a
  freeze / thaw operation.

  The documented contract — "new instances with the same attributes" — was the
  correct one, so the behaviour was fixed rather than the documentation.
  `clone` had neither header documentation nor a single test referencing it,
  which is how the gap survived; it now has both.

  Note: `newChild` carries the same unresolved-snapshot pattern for its parent
  link and is deliberately NOT changed here.

  Tests: `test/AttributeEnumerationTests.cpp` (4 cases).
- **`Integer::asLong` no longer returns 0 for values above the second 64-bit
  digit.**

  **Cause.** A `LargeInteger` cell holds
  `LargeIntegerImplementation::DIGIT_COUNT` (4) 64-bit digits, chained through
  `next`. `asLong` rejected only `next != nullptr` or `digits[1] != 0`, so a
  value whose only non-zero digit was `digits[2]` or `digits[3]` passed the
  check and was returned as `digits[0]`. 2^128, 2^192 and 2^200 all read back
  as 0, while 2^64 (`digits[1]`) and 2^256 (a second chunk) were rejected
  correctly, which is why the window between them looked arbitrary. protoCore's
  own bitwise operations, which call `asLong` on their operands, were affected
  as well. Reported from protoClojure, where such literals evaluated to 0.

  **Change.** `asLong` throws unless `next` is null and every digit above
  `digits[0]` is zero. Parsing, printing and arithmetic were already correct:
  a probe covering 2^(64k) for k = 1..8, the values on either side, both signs
  and round trips through `asIntegerString` in bases 10 and 16 had 11 failures,
  all of them `asLong`, and now has none.

  **Tests.** `test/LargeIntegerRangeTests.cpp`.
- **`ProtoString::createSymbol` of an existing symbol no longer allocates.**

  **Cause.** For a name longer than `INLINE_STRING_MAX_BYTES`, or with
  non-ASCII bytes, `createSymbol` built a `ProtoStringImplementation` with a
  null `ProtoContext` and only then called `SymbolTable::intern`, whose
  `normalizeForSymbol` built a second such copy. Both are perennial: their
  cells come from `posix_memalign`, belong to no young generation and no
  freelist, and no cycle ever reclaims them. When the spelling was already
  interned, both copies were dropped on the re-check inside the shard lock and
  leaked. Every protoClojure map operation on a string key longer than 6 bytes,
  and every global access with such a name, paid it.

  **Change.** Both entry points look the spelling up before building anything:
  - new `SymbolTable::lookupUTF8(ctx, bytes, len)` finds a symbol from raw
    UTF-8 bytes without allocating, hashing them exactly as
    `computeContentHash` does, so it lands in the same shard and matches the
    same bucket as a lookup keyed by a `ProtoString`;
  - `createSymbol` calls it after the inline-string path;
  - `intern` calls `lookupByContent` before `normalizeForSymbol`, which also
    covers the auto-interning of attribute names in `setAttribute`.

  Interning semantics are unchanged: symbols stay unique and perennial.

  **Measured.** 1,000,000 `createSymbol` calls for an existing 20-byte name:
  resident memory grew by 132 KB instead of 500,132 KB (about 500 bytes per
  call), and the loop took 644 ms instead of 2,615 ms. The returned pointer was
  the canonical symbol in every call, before and after.

  **Tests.** `test/SymbolInternTests.cpp`.
- **The per-thread caches are no longer GC roots.**

  **Problem.** `ProtoThreadExtension::processReferences` traced every
  `AttributeCacheEntry` (object, result, name) and every
  `MutableValueCacheEntry` (shard_root, current_value) in the concurrent
  mark. A cached old shard root kept a whole old version of its shard alive.
  After mutables-tree entries started being released, that kept released
  states alive: a probe that writes and drops 1,000,000 mutable objects kept
  256,581 cells live after settling, against 437 for an immutable control. The
  marker also read slots that their owning threads rewrite concurrently.

  **Change (maintainer's option Q).**
  - The caches are no longer traced.
  - Each thread clears both of its caches when it resumes after a
    stop-the-world, before it can look anything up again. It does so when it
    leaves the allocation poll's park, `ProtoContext::safepoint()`,
    `ProtoThread::synchToGC()` or a heap-headroom wait, and when it returns
    from an unmanaged region.
  - A per-thread copy of `gcCycleCount`
    (`ProtoThreadExtension::lastClearedEpoch`) limits the clear to once per
    cycle.
  - Parking itself does not change: when threads park, the stop-the-world
    quorum and what runs under stop-the-world are unchanged. Only what a
    thread does right after it resumes changes.
  - Entry layouts and lookup paths are unchanged.

  **Why it is sound.** Every entry a lookup sees was written after the last
  stop-the-world, so the cells it names were marked or young then, and none is
  freed or has its address reused before the next stop-the-world clears the
  entry. A thread inside a critical section delays the stop-the-world, so it
  cannot miss a clear.

  **Note for embedders.** A value read from shared mutable state and held in
  C++ across allocations was sometimes kept alive, incidentally, by the
  reading thread's own cache entry. That is no longer the case. The existing
  rule applies unchanged: root such values or keep them inside a critical
  section.

  **ABI change.** `ProtoThreadExtension` (internal) uses its last free 8 bytes
  for `lastClearedEpoch`, and stays within its 64-byte cell. Embedders must
  rebuild.

  **Tests.** New `test/ThreadCacheClearTests.cpp`:
  - both caches are empty once their owner resumes from a cycle;
  - new objects at reused addresses never get a stale cached attribute;
  - a thread returning from an unmanaged region across cycles has its caches
    cleared;
  - a thread whose heap-headroom wait spans a cycle has its caches cleared.

  `ConcurrentMarkSafety.ThreadCacheSlotFlipsDuringMark` now checks that the
  marker never reads the caches.

  **Measured.** A probe that writes and drops 1,000,000 mutable objects now
  settles at 437 live cells, equal to the immutable control, with no cache
  refresh of any kind; it settled at 256,581 before. Maximum RSS 107 MB.

  Hot paths are unchanged. `perf stat -r 5` instructions, the same binaries
  run against the library at `e0793341` and against this commit:

  | Benchmark | e0793341 | this commit | Change |
  |---|---:|---:|---:|
  | `microbenchmark_final` | 8,704,375,885 | 8,703,402,870 | −0.01% |
  | `mutable_access_benchmark` | 17,048,728,009 | 17,045,648,828 | −0.02% |
  | `cache_timing_benchmark` | 4,627,674,728 | 4,626,199,150 | −0.03% |
  | `hash_quality_benchmark` | 1,692,479,243 | 1,694,592,140 | +0.12% |
  | `object_access_benchmark` | 60,376,632,041 | 60,377,961,373 | +0.00% |
  | protoPython `attr_lookup` | 18,282,758,398 | 18,277,865,604 | −0.03% |
  | protoPython `richards_lite` | 240,453,130 | 240,306,415 | −0.06% |
  | protoClojure `fib` | 4,835,137,649 | 4,832,224,960 | −0.06% |
  | protoST `attr_lookup` | 692,787,317 | 689,030,577 | −0.54% |

  Instruction stddev is at most 0.09% on every row.

  **Cost of the clear.** One clear of both tables (57,344 bytes) takes about
  800 ns. In a probe with 16 threads and forced cycles under a heap limit, 16
  cycles produced 248 clears, about 0.2 ms of a 1,183 ms run.
- **Per-thread refill batches are sized to the heap limit.**

  **Cause.** A thread allocates from a private freelist that `getFreeCells`
  refills in batches of up to 65,536 cells when several threads run, or a whole
  8,192-cell chunk. Those cells count against the heap limit, but no cycle can
  reclaim what a thread holds. Under `PROTOCORE_HEAP_LIMIT_CELLS=500000`, eight
  worker threads' batches alone exceeded the limit, so protoST aborted with
  "out of memory" while the live set was 48,000–350,000 cells.

  **Change.** Only when a hard limit is configured, each refill is capped at
  `maxHeapSize / (8 × runningThreads)` cells, never below 512, so all threads'
  batches together use at most one eighth of the limit. A larger free chunk is
  split. An OS request keeps its usual size, and the surplus is published as
  free chunks. Without a limit, batch sizes and the path are unchanged.

  **Constants.** `kLimitBatchFraction = 8`, the suggested starting point.
  `kMinLimitedBatchCells = 512`, so a tiny limit or many threads do not turn
  every few allocations into a refill under `globalMutex`. The cap uses
  `runningThreads` at each refill. That count dips while a thread waits for
  headroom, but a waiting thread holds almost nothing, so the sum stays bounded.
  A free chunk is split only when it is larger than the cap. When the cap does
  not bind, as with a single thread under a generous limit, chunks are handed
  out whole as before.

  **Tests.** `test/HeapLimitBatchTests.cpp`:
  - Eight threads start together under a 500,000-cell limit. The sum of the
    cells held in their freelists stays within a quarter of the limit, and live
    data survives. Without the cap, the sum reached 466,392 cells, with 65,423
    in a single thread.
  - Without a limit, a refill still hands out a full chunk.

  **Measured.** `perf stat` user-space instructions, same binaries, uncapped
  against capped library:

  | Benchmark | No limit | `PROTOCORE_HEAP_LIMIT_CELLS=64000000` |
  |---|---|---|
  | Single-thread allocation probe (5,000,000 objects) | unchanged (−0.01%) | unchanged (±0.001%) |
  | `immutable_sharing_benchmark` | unchanged (−0.02%) | unchanged (±0.001%) |
  | `concurrent_append_benchmark` | ±8% run-to-run noise, no usable signal | same |

  **protoST at 500,000 cells.** The out-of-memory aborts with small live sets
  are gone. The remaining aborts report live sets of 406,000 to 583,000 cells.
- **A snapshot read from the mutables tree is never held across a
  park point** — preparation for the per-thread caches no longer being GC
  roots.

  **Why.** Three kinds of code resolve a mutable object's current state, then
  keep using it, or the shard root they read, across allocations that can
  park for a stop-the-world. Only C++ locals reference those cells. If
  another thread publishes a new state meanwhile, nothing else keeps the old
  one alive. Until now the thread's own mutable value cache entry happened to
  keep them alive, because the collector traced the caches.

  **Changes.**
  - `ProtoObject::getParents` and `ProtoObject::getAttributes` open their
    outermost critical section before resolving the snapshot and keep it until
    the result is built. Their heap checkpoint now runs before the resolve, not
    during the build.
  - No thread parks inside a critical section in any configuration:
    - `ProtoContext::allocCell`, `ProtoContext::safepoint()`,
      `ProtoThread::synchToGC()` and the headroom wait now skip parking inside
      a critical section even with `PROTOCORE_GC_REINCLUDE_SURVIVORS=OFF`, as
      they already did with it ON;
    - in that configuration, the `setAttribute` family could previously park
      while path-copying a shard root it had read;
    - the stop-the-world quorum now waits for critical sections to end in
      every configuration.
  - This also removes a documented known issue of the OFF configuration: an
    exiting thread, parked inside `removeAt`'s critical section, could be
    counted twice by the quorum.
- **Waiting for heap headroom no longer hands the waiting context's young
  generation to the collector** — under a hard heap limit, a thread that
  reaches the ceiling waits in `ProtoSpace::waitForHeapHeadroom`, called from
  the heap checkpoint at the entry of an outermost critical section. After the
  wait, `reclaimWaitLocked` called `ProtoContext::safepoint()`. With
  `PROTOCORE_GC_REINCLUDE_SURVIVORS`, once the context had allocated more than
  `maxAllocatedCellsPerContext` cells (10,000 by default), that call submitted
  its young chain.

  The wait runs inside native code, where the caller may hold a half-built
  structure only in C++ locals and in that chain, so the next cycle freed
  cells still in use. For example, protoClojure's `vector` primitive builds a
  list and passes it to `newTupleFromList`: the list was freed while the tuple
  was being built, and protoClojure's `cli/large-program` test crashed with
  SIGSEGV in about half the runs at `PROTOCORE_HEAP_LIMIT_CELLS=2000000`. This
  contradicted the rule that a young generation is submitted only when its
  context is destroyed or at a `safepoint()` the embedder calls.

  The wait now parks through the thread's park-only entry
  (`ProtoThread::synchToGC`); a context without a thread parks the same way.
  No public API changes. The only other place protoCore calls `safepoint()`
  itself is `thread_main`, on a new thread's fresh context before its method
  runs; that context holds no cells, and the call is unchanged.

  Test: `test/HeapHeadroomWaitTests.cpp`. It fails before the change in 3 of 3
  runs: a 1,000-element list read back with size 0.
- **A mutable object's state is released after the object is collected** —
  every update of a mutable object (`newObject(true)`,
  `newChild(context, true)`, `clone(context, true)`) stores its state in
  `ProtoSpace::mutableRoot`, keyed by its `mutable_ref`, and no code ever
  removed an entry. The last state of every mutable object that was ever
  written, and everything it referenced, therefore stayed reachable after the
  object became garbage, and every cycle marked it again. The collector now
  releases the entry:
  - `ProtoObjectCell::finalize` of a collected handle only records its
    `mutable_ref`;
  - after sweep, the GC thread removes the recorded refs shard by shard with
    one compare-and-swap per shard, redone from the new root when a mutator
    published to the shard meanwhile;
  - the path copies are allocated through the collector's own context,
    `ProtoSpace::gcContext`, built with a new `ProtoContext::GCOwnedTag`
    constructor that never registers as `mainContext`.

  The entry disappears in the cycle that collects the handle, and the state is
  freed in the next cycle. Two limits remain, both documented:
  - With `PROTOCORE_GC_REINCLUDE_SURVIVORS=OFF`, a handle that survives a cycle
    is never collected, so its entry stays. This is the expected behaviour of
    that configuration.
  - A thread's mutable value cache keeps a pre-release shard root alive until
    its slot is reused.

  Finalizers now have a written contract: they complete an action on a
  structure, and never allocate, publish or process
  (`docs/GarbageCollector.md` § "Finalizer contract"). Embedder guidance on
  when to use mutable objects is in
  `docs/Structural description/architecture/02_mutability_model.md`.

  Measured with a probe that writes and drops 1,000,000 mutable objects, each
  with one attribute holding a fresh object, in batches of 50,000 with one
  cycle per batch:

  | | Before | After |
  |---|---|---|
  | Entries left | 1,000,000 | 0 |
  | Live cells after settling | 5,009,397 | 256,581 (1,737 once the thread's 1,024 cache entries are replaced; immutable control: 437) |
  | Maximum RSS | 479 MB | 107 MB |
  | Wall time | 14.4 s | 5.2 s |
  | Mark, per settling cycle | 404–649 ms | 24–29 ms |
  | Sweep, per settling cycle | 367–640 ms | 31–36 ms |
  | Bulk unmark, per settling cycle | 92–114 ms | 3.4–3.6 ms |

  The release itself took 17.6 ms on average and 24.2 ms at most per cycle
  that released 50,000 entries. It is reported in the new `REL=` field of the
  `PROTOCORE_GC_PROFILE` line.

  **ABI change:** `ProtoSpace` gains the trailing members `gcContext` and
  `gcFinalizedMutableRefs`, and `ProtoContext` gains a constructor and a
  private flag. Embedders must rebuild.

  Tests: `test/MutableRootReclaimTests.cpp`.
- **Stop-the-world collects only roots** — Phase 2 of the collector walked
  every context's young chain and called `processReferences` on each young
  cell while all threads were stopped, so the pause grew with the number of
  young cells. With `PROTOCORE_GC_REINCLUDE_SURVIVORS` it also scanned every
  survivor-pen cell on non-fold cycles and relinked a folded pen one segment
  at a time under the pause. Stop-the-world now records one handle per
  context, the head of its young chain, and captures the survivor pen in
  O(1); the chains, the pen cells and the pen fold are handled by the
  concurrent mark. In a probe with one thread holding 20,000,000 young
  cells, P1 + P2 per cycle fell from 299–373 ms to 10–70 µs (20,000 young
  cells: from 244–508 µs to 13–98 µs). With `PROTOCORE_GC_INSTRUMENT`, P2
  now ends when the world resumes rather than when mark starts.
- **The collector no longer dereferences a null work-list entry** — the
  concurrent mark crashed with a segmentation fault at address 0x8 (reading
  `Cell::next_and_flags` of a null `Cell*`, `gcThreadLoop+0xdf8`), seen in
  0.2–0.3 % of protoPython runs in a stress configuration with very frequent
  collection cycles.
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
- **A double's hash agrees with equality** — `DoubleImplementation::getHash`
  hashed the bit pattern, so NaNs of different sign or payload (x86
  `0.0/0.0` is a negative NaN, `std::nan("")` a positive one) were different
  `ProtoSet`/`ProtoMultiset` elements, and whether `-0.0` and `0.0` collided
  depended on the standard library. All NaNs now hash as the canonical quiet
  NaN and `-0.0` hashes as `0.0`; other doubles keep their hash. Hashes still
  differ across numeric kinds (`1` and `1.0`), as documented for `ProtoSet`:
  a language whose equality crosses kinds supplies its own hash.
- **`ProtoList::has` with integers beyond `long long`** — both list forms
  compared integer elements with `asLong`, which throws
  `std::overflow_error` for a `LargeInteger` outside that range, so `has`
  threw on a list holding such an element (or when asked for one). Integers
  are now compared by value with `Integer::compare`, as `ProtoTuple::has`
  already did.
- **The GC cycle counter advances in every build configuration** — the only
  increment of `gcCycleCount` sat inside the `PROTOCORE_GC_REINCLUDE_SURVIVORS`
  block, so with `-DPROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` `getGCCycleCount()`
  stayed at zero and heap-limit reclamation waits, which wait for the counter
  to advance, never saw a cycle complete. The counter is now incremented for
  every cycle; the survivor re-chain still uses the same cycle number to decide
  fold cycles.
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
- **Performance gate and sanitizer run (2026-09-23)** — measured at merge
  time on the release host with `perf stat -e cycles,instructions -r 3`,
  baseline `e43fa2e4` versus this release, with the SAME benchmark sources on
  both sides (the branch's rewritten, self-verifying `sparse_list_benchmark`
  was compiled against the baseline library too, so the two columns run the
  same program). The host was loaded throughout (1-minute load average 6.5 to
  7.9 on 12 cores), so the three interleaved rounds below are reported in
  full and the conclusion rests on retired instructions, which are
  load-independent; cycles are given for completeness and their spread across
  rounds is larger than the difference between the two columns.

  | Benchmark | Round | Instructions, baseline | Instructions, 2.0.0 | Delta |
  |---|---|---|---|---|
  | `sparse_list_benchmark` | 1 | 561,955,175 | 558,644,067 | −0.59% |
  | `sparse_list_benchmark` | 2 | 559,208,979 | 559,064,235 | −0.03% |
  | `sparse_list_benchmark` | 3 | 562,104,844 | 558,678,437 | −0.61% |
  | `object_access_benchmark` | 1 | 60,374,194,360 | 60,332,010,971 | −0.07% |
  | `object_access_benchmark` | 2 | 60,370,335,560 | 60,333,693,343 | −0.06% |
  | `object_access_benchmark` | 3 | 60,373,632,245 | 60,322,191,846 | −0.09% |

  Cycles, same runs: `sparse_list_benchmark` 535.8M / 537.2M / 545.1M
  (baseline) against 530.6M / 531.6M / 545.8M; `object_access_benchmark`
  24.99G / 24.78G / 25.25G against 25.97G / 25.10G / 25.09G. The one outlier
  (round 1, +3.9% cycles) carried a ±3.29% run-to-run spread of its own and
  did not reproduce in rounds 2 and 3, whose instruction counts are flat.
  **Conclusion: no measurable regression on either benchmark.** Both
  benchmarks self-verify (`VERIFIED` / `Checksum verified.`), so a crash
  could not be counted as a fast run.

  AddressSanitizer (`-fsanitize=address`, RelWithDebInfo): the full 412-case
  suite ran with **zero AddressSanitizer reports**. Three cases fail under
  ASan and all three fail identically at the `e43fa2e4` baseline, so they are
  pre-existing and not caused by this release:
  `ConcurrentMarkSafety.ThreadCacheSlotFlipsDuringMark` (asserts that at
  least 100 GC cycles run inside a fixed 4-second budget — throughput
  sensitive, it also fails in a plain Debug build at the baseline, 31 cycles
  there and here) and the two `SymbolIntern` cases that cap resident-set
  growth at 4 MiB (ASan's shadow memory and redzones exceed that cap by
  construction).
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
- **One codepoint pass per byte when building a string** — `buildAVL` counted
  the codepoints of its whole byte range on entry, then discarded the count
  whenever the range was larger than a 32-byte leaf and it recursed, so the
  scan ran once per recursion level: 15 redundant full passes over a 1 MiB
  buffer. The count is now taken in the leaf branch only, the one place that
  uses it; `StringInternalNode`'s constructor already derives `total_chars`
  and `left_chars` from its children in O(1). Every byte is scanned exactly
  once and the rope is unchanged — same leaves, same internal nodes, same
  depth, same per-node counts, same content hash. This is the path taken by
  every bulk constructor (`ProtoString::fromUTF8Buffer`,
  `ProtoStringImplementation::fromUTF8Bytes`, `ProtoString::create`,
  `ProtoString::createSymbol`), so it speeds up every file read and every
  embedder that builds a string from a buffer. Measured with `fromUTF8Bytes`
  on ASCII, cells identical at every size: 9.37 to 6.82 ns/char at 467 B,
  11.63 to 5.88 at 4 KiB, 17.25 to 6.14 at 64 KiB and 16.32 to 6.38 at 1 MiB
  (2.6x; 17.1 ms to 6.7 ms for the whole build).
- **String construction no longer builds a list of code point objects** —
  `ProtoContext::fromUTF8String`, and therefore `ProtoString::fromUTF8`,
  `fromUTF8String`, `fromStdString` and `fromCodepointTuple`, built an
  N-element `ProtoList` of code point objects one `appendLast` at a time,
  re-encoded that list into a `std::string` and then called the bottom-up rope
  builder anyway. Every append copied a root-to-leaf path, so construction
  cost O(N log N) cells of which all but the final rope were dead before the
  constructor returned: at 1 MiB, 23.1 million cells to produce a 65,536-cell
  rope, 99.7% garbage. It now decodes the bytes once and builds the rope
  directly, allocating exactly the cells the rope needs — the theoretical
  minimum of `2 * ceil(B / 32)`, about 4 bytes of heap per ASCII character,
  with no garbage at any size. Measured on ASCII:

  | length | before | after | speedup | cells before | cells after |
  |---|---|---|---|---|---|
  | 7 B | 194.4 ns/char | 11.8 ns/char | 16x | 32 | 2 |
  | 467 B | 554.5 ns/char | 7.28 ns/char | 76x | 5,108 | 32 |
  | 4 KiB | 720.4 ns/char | 6.80 ns/char | 106x | 57,576 | 256 |
  | 64 KiB | 931.7 ns/char | 6.52 ns/char | 143x | 1,183,712 | 4,096 |
  | 1 MiB | 1138.3 ns/char | 6.55 ns/char | 174x | 23,134,168 | 65,536 |

  The resulting string is unchanged in every observable way — same bytes, same
  size, same content hash, same rope (leaves, internal nodes, depth), same
  inline-versus-heap representation, same symbol behaviour — including for
  malformed UTF-8, which is still decoded and re-encoded so that a truncated
  sequence degrades to its lead byte and an overlong sequence collapses to its
  shortest form.

  The construction also no longer holds a GC critical section across the
  per-character loop. That section suppressed this thread's stop-the-world
  parking for the entire build (1.38 s for 1 MiB) while protecting nothing:
  cells allocated during the build sit on the context's young chain, which the
  collector records as a root and which can only become a sweep candidate once
  `ProtoContext::safepoint()` or context destruction submits it — neither of
  which the builder calls. The heap-ceiling backpressure the section's
  constructor performed is kept, as an explicit `heapLimitCheckpoint()` before
  the first allocation. Repeated builds of a 467-character value under a hard
  ceiling now run collections and stay inside it.

  Elsewhere the two string changes are neutral or better:
  `immutable_sharing_benchmark`, `list_benchmark` and
  `object_access_benchmark` move by +0.8%, +0.2% and -0.6% of cycles with
  instruction counts flat. One microbenchmark is slower:
  `string_concat_benchmark` (10,000 rope joins through `appendLast`, a path
  neither change touches) takes 2.8% more wall clock and 3.8% more cycles
  while executing 0.3% more instructions — a code-layout effect of the new
  functions in `core/ProtoString.cpp`, not extra work.

### Tests
- `test/MapParentChainTests.cpp` (nine cases) — the one place where this
  release's two halves meet. A `ProtoMap` handle is a NON-OBJECT cell
  pointer, so every rule the parent-chain rewrite states for a non-object
  receiver or a non-object chain entry has to hold for it through
  `ProtoSpace::mapPrototype`. The cases pin: `getPrototype` answers
  `mapPrototype` for tag 27; `isInstanceOf` on a map answers through that
  prototype and walks the prototype's own flattened chain;
  `getAttribute`/`hasAttribute`/`getAttributes` on a map receiver reach
  `mapPrototype`'s attributes and a missing key still terminates; `newChild`
  on a map childs its prototype; a map stored as a parent is kept by
  `addParent` and by `setParents`' flattening, contributes no ancestors of
  its own, preserves the listed order around it, and is found by
  `hasParent`/`isInstanceOf`; a mutable receiver sees a map parent added
  after creation; and a map reachable ONLY through a parent-link chain (the
  child pinned in a root set, single-root pinning) survives forced collection
  cycles with its 64 entries intact. Neither half needed a code change to
  satisfy them — the file exists so a later change to either half cannot
  quietly drop the map case.
- `StringBuildTests` (eleven cases) pins what string construction *produces*,
  so that changes to how it is built cannot change what is built. A 42-entry
  golden corpus — the well-formed ladder from empty to 64 KiB, 2/3/4-byte
  sequences, combining marks, and 18 malformed-UTF-8 cases — was captured from
  the library before the change and is checked for content bytes, codepoint
  size, content hash, inline-versus-rope representation and rope shape (leaf
  count, internal count, depth). The same corpus is compared, node for node,
  against a reference implementation of the old code-point-list construction
  kept in the test file. Every leaf's `char_count` is recounted from its own
  payload and every internal node's
  `total_chars` / `left_chars` / `total_bytes` is checked against its
  children; multi-byte sequences are built at every length around the 32-byte
  leaf boundary. Further cases bound the cells one build may allocate to the
  size of the rope it produces (a bound the list construction misses by two
  orders of magnitude); run a thousand 467-character builds under a hard heap
  ceiling and check that collection cycles run and that both the heap and the
  resident set stay bounded; and build 1 MiB through the public and the bulk
  entry points. Plus the inline boundary, symbol interning and identity, and
  `fromStdString` agreeing with `fromUTF8` over the whole corpus.
- `GCRootScope` (five cases, cycles forced with a small heap limit): a probe
  cell in a live young chain and one in the survivor pen are traversed by the
  collector but never while `stwFlag` is raised (each failed against the
  library before the corresponding fix); an object referenced only by a
  young cell, and one referenced only through the mutables tree, survive
  cycles; four threads allocate and check young cells over old candidates
  while cycles run.
- `ConcurrentMarkSafety.ThreadCacheSlotFlipsDuringMark` flips the main
  thread's attribute-cache and mutable-value-cache slots between a cell and a
  non-cell from a helper thread while a hard heap limit just above the
  startup heap forces repeated cycles; it crashed before the null work-list
  fix.
  `GCMark.NullReferenceIsSkipped` pins a tagged null in a root set (it crashed
  before the fix), and
  `GCMarkDeathTest.NullReferenceFromProcessReferencesIsReported` checks that
  instrumented and debug builds name a cell type whose `processReferences`
  reports `nullptr`.
- `NumericCompareTest` (four cases): every NaN is unordered in both operand
  orders against NaNs, ±0.0, ±inf, SmallIntegers and ±2^70; for ordered
  numbers from -inf to +inf (including -2^70, -0.0/0/0.0, 2^53+1 and 2^70)
  the sign of `partialCompare` matches `compare` and the expected order;
  strings, objects and mixed pairs; and NaN elements do not alias numbers in
  `ProtoSet`, `ProtoMultiset`, `ProtoList` and `ProtoTuple`.
- `NumericTest.DoubleHashAgreesWithEquality` checks that quiet, negative and
  payload NaNs hash alike, that `-0.0` and `0.0` hash alike, and that a set
  treats them as one element (the NaN cases failed before the fix).
- `ListTest.HasComparesLargeIntegersByValue` checks `has` on inline and AVL
  lists holding 2^70 against an equal distinct object, neighbours and small
  integers (it threw `std::overflow_error` before the fix).
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
