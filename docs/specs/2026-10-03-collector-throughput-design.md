# Collector throughput: diagnosis, parallel sweep and pacing

Status: **draft for review**.  Date: 2026-10-03.  Base: protoCore 2.10.2
(master `294822fc`).  Author: Gustavo Marino, with Claude.

Inputs:
[../reports/2026-10-03-gc-phase-breakdown.md](../reports/2026-10-03-gc-phase-breakdown.md)
(the data this spec answers; "the phase report" below),
[../reports/2026-10-03-adaptive-heap-calibration.md](../reports/2026-10-03-adaptive-heap-calibration.md),
[2026-10-02-adaptive-heap-controller-design.md](2026-10-02-adaptive-heap-controller-design.md),
[../GarbageCollector.md](../GarbageCollector.md).  Line numbers refer to
`294822fc`.

This is a design, not an implementation.  Nothing in it has been built or
measured beyond what the phase report measured.  Every expected gain below is
a bound or a hypothesis, and is labelled as such.

## 0. Summary

1. **Diagnose first.**  The sweep's cost per cell triples with concurrent
   allocators (40 -> 158 ns) while the collector stays on the CPU.  The cause
   is unmeasured, and it decides whether more sweeping threads help.  Section 3
   lists five hypotheses, the counters and differential benchmarks that
   separate them, and seven cheap fixes.  Two of the fixes, batched segment
   recycling and a multi-cursor sweep, may recover much of the cost on one
   thread.
2. **Parallel sweep** (section 4).  The sweep becomes a job that the space's
   collector thread runs together with K helper threads from one process-wide
   pool.  Sweepers claim runs of segments.  Each one builds its own free
   chunks, survivor chain, recycled-segment chain, finalized-ref vector and
   dead-cell vector, and publishes them in batches.  Phase 5b, the bulk
   unmark, Phase 7 and the token release stay on the collector thread, after
   the join.  Finalizers still run on collector-owned threads and never on
   mutator threads.  Embedder callbacks (`ProtoExternalPointer`) stay serial
   on the collector thread.  There are no barriers, no model change and no
   ABI change.  K = 0 keeps today's behaviour.
3. **Pacing** (section 5).  Under a fixed limit, a cycle should start before
   the ceiling, at a runway equal to allocation rate x last cycle duration.
   The wait should also stop being quantised by the 50 ms watchdog: in every
   multi-threaded run of the phase report, the mean wait is 49-50 ms.  The
   controller's pacing is unchanged in v1.  Section 5.4 explains its N = 12
   result: more cycles, each re-marking the live set, on a collector that is
   already saturated.
4. **Mark stays serial** (section 6), with explicit triggers for revisiting
   it.
5. **Acceptance** (section 8): the phase report's workloads, median of 3,
   self-verifying.  At N = 12 the sweep's throughput must at least double.
   N = 1 must show no regression beyond noise.  TSan and ASan must be clean.
   Deterministic tests are listed in section 9.

## 1. Problem

From the phase report (one run per configuration, AMD 6 cores / 12 threads):

- The sweep is 41-99 % of the collector's busy time (median 61 %) and the
  largest phase on 30 of 32 completed runs.  Stop-the-world is at most
  0.23 ms per cycle.  Phase 5b and the bulk unmark together are 0-14 %.
- With 12 allocating threads, the single collector is busy 56-95 % of the
  wall time.  The mutators wait for headroom 17-63 % of their thread time.
  protoScala and protoClojure with 6 tasks: 92-94 % busy, 45-66 % waiting.
- Sweep cost per cell rises with concurrent allocators: protoClojure 45 ->
  143 ns (t1 -> t6, same cells per cycle), protoScala 40 -> 158 ns, protoJS
  `records` 17 -> 58 -> 85 ns (N = 1, 6, 12).  The collector thread was on
  the CPU 99-100 % of the sweep's wall time, so each cell costs more; the
  collector is not being descheduled.
- At N = 6 with a 40 M-cell limit, the collector is busy for less time than
  the mutators' own work, yet the mutators wait 10-22 %.  A fixed limit
  requests a cycle only at the ceiling.
- At N = 12 the adaptive controller was slower than the fixed limit on all
  five workloads (+2 % to +91 %).

**A derived fact the phase report does not state.**  Dividing each run's
`headroom_wait` by its number of waits gives the mean length of one
`reclaimWaitLocked` call:

| Run | Waits | Mean wait |
|---|---:|---:|
| js40_records_n6 | 428 | 50.2 ms |
| js40_records_n12 | 4,603 | 50.2 ms |
| js40_graph_n12 | 7,233 | 50.3 ms |
| jsad_records_n12 | 7,094 | 49.4 ms |
| jsad_wordfreq_n12 | 3,887 | 49.1 ms |
| scala_tree_t6 | 4,824 | 50.2 ms |
| clj_coll_t6 | 10,253 | 50.2 ms |
| cad_20k | 13,695 | 50.2 ms |
| core_fixed640_live1M (1 thread) | 25 | 50.0 ms |

A wait is bounded by the 50 ms watchdog of `reclaimWaitLocked`
(`core/ProtoSpace.cpp:1755-1756`).  A mean this close to 50 ms means nearly every
wait ended on the watchdog, not on a notification.  Under a fixed limit this
follows from the code.  The predicate waits for a change of `gcCycleCount`,
which counts cycle **starts** (line 1761).  `memoryReclaimedCV` is notified
only at cycle end (line 1221).  A wait that begins inside a running cycle
therefore always lasts 50 ms.  The thread then re-checks the freelist
(`waitForHeapHeadroom`, line 1833) and waits again if it is still empty.
Under the controller, the cycles are longer than 50 ms, so the watchdog fires
first there too.  The mutators therefore poll the freelist every 50 ms while
the sweep publishes chunks.  The share of the measured wait that is polling
latency, rather than missing cells, is unmeasured (section 3.2, counter C6).

## 2. Constraints

- **No model change.**  The sweep stays a sweep of the segments captured in
  Phase 2.  There is no new cell kind, no new collector subsystem and no
  special case for a language.  Helpers run the same sweep code as the
  collector thread, and the serial sweep is the K = 0 case of the parallel one
  ([feedback: purity over performance]).  Collector throughput is
  paradigm-aligned work: immutable style produces more garbage, so the sweep
  is the platform's cost, not a mutable-style artefact.
- **No write barriers, and mark is unchanged.**  "Concurrent Mark Without
  Barriers" (GarbageCollector.md) holds as written.
- **No ABI change.**  The layouts of `ProtoSpace`, `ProtoContext` and `Cell`
  in `headers/protoCore.h` stay as they are, and no virtual is added.  New
  state lives in side structures owned by `core/` (the pattern of
  `core/AdaptiveHeap.cpp` and of the `popLocks` array in
  `submitYoungGeneration`).  `DirtySegment` is declared in
  `headers/proto_internal.h`, which is not installed
  (`CMakeLists.txt:113`, only `protoCore.h` is a `PUBLIC_HEADER`), so its
  layout may change.
- **Finalizer contract** (GarbageCollector.md § 7).  Finalizers run on
  collector-owned threads, never on mutator threads.  They never allocate,
  never publish with CAS, never loop over protoCore data and never block.
- **Synchronisation primitives.**  Never `std::counting_semaphore` or
  `std::binary_semaphore`: libstdc++ 13 loses wakeups (protoST S19).  This
  design also avoids `std::latch` and `std::barrier`, which sit on the same
  atomic-wait machinery and are not needed.
- **Process sizing rule.**  The heap is still the sum of each space's peak.
  Helpers allocate no cells and no heap.

## 3. Diagnose before parallelising

### 3.1 What the sweep does per cell today

`gcThreadLoop`, Phase 5 (`core/ProtoSpace.cpp:918-1056`), for each segment of
`segmentsToProcess` and each cell of its `cellChain`:

| Event | Operation | Line kind |
|---|---|---|
| every cell | `getNext()` (seq_cst load) and `isMarked()` (load) of `next_and_flags` | read of the cell's only line (cells are 64-byte aligned, one cell per line) |
| dead cell | virtual `finalize()`: an indirect call through the vtable (word 0 of the same line).  Non-trivial only for `ProtoObjectCell` with `mutable_ref > 0` (push to `gcFinalizedMutableRefs`), `ProtoExternalPointer` (embedder callback), `ProtoExternalBuffer` (`alignedFree`) and `ProtoByteBuffer` (`delete[]`) | read, plus `free()` into the allocating thread's malloc arena |
| dead cell | `internalSetNextRaw(batchHead)` (relaxed store) | write: the line is taken exclusive |
| survivor | `unmark()`: `fetch_and`, a locked read-modify-write | write to a **live** cell's line |
| survivor | `setNext(survHead)`: load, then release store | second write to the same line |
| every segment (about 6 cells: "~5.86 cells avg", comment at line 985) | CAS push onto `survivorPen` (survivors) or onto `dirtySegmentFreePool` (none) | the pool head is the line that **every mutator** pops in `submitYoungGeneration` (`ProtoSpace.cpp:2255`), at each context destruction |
| every 8,192 cells (`CELL_CHUNK_SIZE`, `protoCore.h:2652`; the phase report says 4,096) | `publishFreeChunk` under `ProtoSpace::globalMutex`, a **process-wide static** recursive mutex (`protoCore.h:2834`) that every refill of every space also takes in `getFreeCells` | lock line, `freeChunks` head, `freeCellsCount` |

Around the sweep:

- Mark sets bit 0 with `fetch_or` on every reached cell
  (`ProtoSpace.cpp:862`), which is a write to every live line.
- Phase 6 runs `fetch_and` again on every entry of `markedList`
  (lines 1110-1114).  That includes survivors the sweep already unmarked, so
  each live candidate cell is written four times per cycle: mark, unmark,
  relink, unmark.

A dead cell's line was written last by the mutator that built it, possibly on
another core.  After the sweep, the cell goes into a free chunk that another
mutator will write.  A survivor's line is read by mutators, and every
collector write to it invalidates their copies.

### 3.2 Hypotheses and how to tell them apart

| # | Hypothesis | Predicts |
|---|---|---|
| H1 | **Coherence on swept cells.**  Dead cells are still Modified in the allocating core's private cache, and survivors are Shared with readers.  Each line costs a cross-core or cross-CCX transfer. | High cache-to-cache (HITM / remote-cache fill) counts on the collector, at the `next_and_flags` load (`ProtoSpace.cpp:936-937`) and the survivor writes; the cost scales with the number of mutators that touch the cells, not with their memory traffic. |
| H2 | **Loaded DRAM latency.**  The chains are scattered (recycled chunks are prepended in sweep order), so every cell is a dependent miss.  N mutators raise the loaded latency. | DRAM fills dominate.  The cost also rises when the extra threads only stream private memory, and falls with chain contiguity. |
| H3 | **Contended collector-shared lines.**  The per-segment CAS on `dirtySegmentFreePool` (pushed by the collector, popped by every mutator at every function return), `survivorPen`, `globalMutex`/`freeChunks`, and 16-byte `DirtySegment` structs sharing lines across threads (allocated with `new`, recycled between threads). | Samples concentrated on the CAS loops at lines 1012-1018, 1022-1028 and 1037-1042 and on `publishFreeChunk`; CAS retry counts that grow with N; HITM on `DirtySegment` lines.  At about 6 cells per segment, +100 ns per cell is about +600 ns per segment, the order of one contended line transfer with retries. |
| H4 | **Finalizer work.**  `free()` from the collector into the arena of the thread that allocated the memory, contending with that thread's `malloc`. | Cost correlated with the count of `ExternalBuffer`/`ByteBuffer` finalizations; samples in glibc `free` / `_int_free` lock paths.  Likely small on the phase report's workloads (to be verified). |
| H5 | **Writes to live lines.**  The survivor unmark and relink (sweep) and the Phase 6 unmark force line ownership away from readers. | Cost scales with the survivor share (`swept - freed`).  protoClojure frees 99 % of what it sweeps and still triples, so H5 cannot be the main cause there. |

Note on the phase report's argument against lock contention: the collector
was on the CPU 99-100 % of the sweep.  That excludes *blocking* on
`globalMutex`.  It does not exclude H3, because a contended CAS or a cache
line transfer stalls on the CPU.

**Measurements** (on branch `measure/gc-phases`, compiled out by default,
like the existing `[GC-PHASES]` fields):

- C1: segments swept per cycle, and cells per segment (mean and a
  power-of-two histogram).
- C2: CAS retries on `dirtySegmentFreePool` and `survivorPen` in the sweep,
  and on `dirtySegmentFreePool` pops in `submitYoungGeneration`.
- C3: time spent acquiring `globalMutex` in `publishFreeChunk`, and the
  number of publications.
- C4: finalizations by cell type, counting only the four non-trivial kinds.
- C5: chain contiguity, the fraction of `next` links within ±1 cell of the
  current cell (a proxy for H2's "scattered").
- C6: the wait histogram.  For each `reclaimWaitLocked` call, record whether
  it ended on the watchdog or on the predicate, and whether the freelist was
  non-empty when it returned.  This splits polling latency from missing cells
  (section 1).

**PMU** (single run per configuration, on an idle machine, never while
another agent builds or tests):

- `perf stat -t <collector TID>` for cycles, instructions, and L2/L3 misses.
  On Zen 2, also the `ls_refills_from_sys.*` (or `ls_mab_alloc` / data-source)
  events that split fills into local L2, other core / other CCX, and DRAM.
  Event names vary by kernel; pick them from `perf list` first.
- `perf record -t <collector TID>` with precise (IBS op) sampling, to
  attribute stall cycles to the instruction: the header load, the CAS loops,
  `fetch_and`, `free`.
- `perf c2c record` where the CPU and kernel support it (Intel; AMD via IBS
  on recent kernels).  If it is unavailable on DEV12, say so in the report and
  rely on the differential benchmarks.

**Differential microbenchmark** (`performance/sweep_contention_benchmark`,
protoCore only, self-verifying: it prints the cells it built, swept and freed,
and checks them against the expected numbers).  One space and a fixed garbage
set of G cells in segments of about 6 cells.  One cycle is measured under
each condition:

| Scenario | Other threads during the sweep | Separates |
|---|---|---|
| S0 | none | baseline |
| S1 | N threads streaming private memory (no protoCore cells) | H2 (load on memory), not H1 or H3 |
| S2 | N threads submitting and destroying empty contexts in a loop | H3 (`dirtySegmentFreePool`, `dirtySegments`) |
| S3 | N threads reading the survivors | H1/H5 on live lines |
| S4 | garbage built by N threads (then idle), against S0 | H1 on dead lines (last writer on another core) |
| S5 | S0 with garbage from fresh contiguous memory, against recycled memory | H2 (contiguity) |

Each scenario prints the sweep's ns per cell.  The pattern of the six numbers
identifies the dominant hypothesis without any PMU support.

### 3.3 Cheap fixes, each measured on its own

Ordered by expected value per line of code.  Each fix is a separate commit
with its own before/after numbers on S0-S5 and on two runtime workloads
(`clj_coll_t6`, `js10_records_n12`).

- **F1 Batch segment recycling (H3).**  Chain the processed segments locally
  and push the chain onto `dirtySegmentFreePool` with one CAS: at the end of
  the sweep, or every 1,024 segments so that mutators are not starved of
  segments.  Do the same for `survivorPen`: one CAS per sweep, since the pen
  is folded only under a later stop-the-world.  This removes about one CAS
  per 6 cells from a line that every mutator hammers.  The push side stays
  lock-free.  The pops keep their spin lock (`popLocks`), which this does not
  change.
- **F2 One write per survivor (H5).**  Replace `unmark()` followed by
  `setNext(survHead)` with a single release store of
  `survHead | (old & 0x3E)`.  This is sound because the collector is the only
  writer of a candidate cell's `next_and_flags` during sweep: the invariant
  documented on `Cell::setNext`, `proto_internal.h:852`.  It saves a locked
  instruction, not a line transfer.  Small on its own, and free.
- **F3 Phase 6 tests before it writes (H5).**  `if (m->isMarked())
  m->unmark();`.  A survivor the sweep already unmarked is then only read, so
  the mutators' Shared copies stay valid.  Cells marked outside the segments
  (perennials, young chains of live contexts) are still cleared.  The token
  invariant is preserved: every mark bit is clear before
  `multispace::cycleActive = false`.
- **F4 Multi-cursor sweep (H1/H2: latency).**  Walk M segments (M = 4-8)
  in lockstep.  At each step, prefetch for write (`PROTO_PREFETCH` with write
  intent) the next cell of every cursor before processing the current ones.
  One chain is a dependent pointer chase with one miss in flight.  M chains
  give M misses in flight on one thread.  Each cursor keeps its own batch and
  survivor head, merged as segments end, so the output is unchanged.  This is
  the fix most likely to recover the 3x on one thread if the cost is latency.
  It is also the inner loop the helpers will run.
- **F5 Batched chunk publication (H3).**  Accumulate up to B = 4 chunks
  locally and publish them under one `globalMutex` acquisition.  If C6 shows
  waiting mutators, publish immediately instead (section 5.3).
- **F6 `DirtySegment` alignment (H3).**  If C2 or c2c shows false sharing on
  segment structs, align `DirtySegment` to 64 bytes.  It is internal and not
  installed.  It costs 48 bytes per segment.
- **F7 Finalizer frees (H4).**  Only if C4 and the profile show `free()`
  contention: collect the external pointers to free and release them after
  the sweep.  This is not proposed by default.

**Gate.**  The parallel sweep is implemented only after F1-F4 are measured.
If F1 + F4 bring the per-cell cost at N = 12 to within 1.5x of N = 1, the
report re-evaluates the Amdahl table (section 7) with the new sweep times
before helpers are built.

Out of scope here, recorded for completeness: a **side mark bitmap** would
remove every collector write to live lines (mark, unmark, Phase 6 becomes a
`memset`).  It needs an address-to-bitmap map for every heap block, plus a
fallback for perennial cells that live outside heap blocks (symbols from
`posix_memalign`).  It is a larger change, justified only if H1/H5 dominate
after F2/F3.

## 4. Parallel sweep with collector helper threads

### 4.1 Shape

The sweep loop body becomes `sweepRun(SweepJob&, SweeperLocal&)`, with the
F4 multi-cursor inner loop.  The collector thread and up to K helpers each
run `claim -> sweepRun -> claim ...` until the segment list is exhausted.
With K = 0, the collector runs it alone and the cycle is today's cycle.
Everything before the sweep (Phases 1-4) and after it (5b, 6, 7, the token,
the cycle-end bookkeeping at lines 1203-1222) stays on the space's collector
thread, in today's order.

### 4.2 Partitioning `segmentsToProcess`

`segmentsToProcess` is a singly linked list, private to the collector after
the Phase-2 `exchange` (line 666) and the pen fold (line 781).
`DirtySegment` has no count, and segments average about 6 cells.

- **Chunked claiming.**  `SweepJob` holds `cursor` (the next unclaimed
  segment) and a claim lock (`std::mutex`).  A claim takes the lock, detaches
  the next R segments by walking R links (R = 128 by default, about 770
  cells), and releases the lock.  The walk under the lock costs R dependent
  loads of 16-byte structs.  At about 100 ns per cell of sweep work, that is
  a lock occupancy of about 2 % per sweeper.
- **No ABA.**  Nodes leave this list only by claims, and no node is ever
  pushed back onto it: a processed segment goes to `dirtySegmentFreePool` or
  to `survivorPen`.  The list's `next` fields are read only under the claim
  lock, so TSan sees ordered accesses.
- **No small-cycle overhead.**  The collector starts alone.  It wakes the
  helpers only if its first claim did not exhaust the list, so a cycle of a
  few thousand cells never touches the pool.
- **Load balance.**  Small fixed runs balance themselves; the tail is at most
  R segments per sweeper.
- **Considered and deferred.**  The collector could pre-split the list into
  stripes, at the cost of a serial pass over every segment.  Per-thread dirty
  stacks would give a natural partition and remove mutator contention on the
  single `dirtySegments` head.  They need a side array keyed by space (the
  `dirtySegments` field is in the ABI), and their merit depends on C2.  That
  is an open question (section 12).

### 4.3 Per-sweeper state and merge

| State | Today (single thread) | Per sweeper | Merge |
|---|---|---|---|
| free chunk under construction | `chunkHead/Tail/Count` | own | published under `globalMutex` at `CELL_CHUNK_SIZE` (or in batches, F5); the partial trailing chunk published by its owner at the end |
| `reclaimedThisCycle` | local | own counter | summed by the collector after the join |
| survivor segments | CAS per segment onto `survivorPen` | own LIFO chain | one CAS of the chain per sweeper (F1) |
| recycled segments | CAS per segment onto `dirtySegmentFreePool` | own chain | one CAS per sweeper (or per 1,024 segments) |
| finalized mutable refs | `space->gcFinalizedMutableRefs` | own `std::vector<proto_ulong>` | appended to `space->gcFinalizedMutableRefs` by the collector after the join, **before** Phase 5b sorts and deduplicates it |
| dead cells (several spaces) | `deadCells` | own vector | concatenated by the collector before the grace period |
| deferred embedder finalizations | none | own vector of `ProtoExternalPointer` cells (4.5) | run by the collector after the join |

Chunk order and pen order are irrelevant today (mutators take any chunk, and
the pen is a set), so a different interleaving changes nothing observable.

### 4.4 Cycle order with helpers

1. Phases 1-4 as today, on the collector thread.
2. The collector publishes the `SweepJob` (space, list head, `deferFree`)
   and sweeps.  The helpers join the job if they are woken (4.2).
3. **Join.**  The collector waits until `cursor == nullptr` and
   `activeSweepers == 0`.  It never waits for a helper to *arrive*, only for
   helpers that have claimed work to finish it.
4. The collector merges the per-sweeper state (4.3) and runs the deferred
   embedder finalizations.  It publishes those cells as one chunk, which
   counts towards `reclaimedThisCycle`.
5. Phase 5b (`releaseFinalizedMutableEntries`), on the collector thread,
   through `gcContext`.  It is unchanged: at most 256 CAS operations, and it
   allocates, so it must stay on the one thread the design allows to allocate
   inside a cycle.
6. Phase 6 on the collector thread with F3.  It parallelises trivially over
   index ranges of `markedList` through the same pool, but this is done only
   if, after F3, it exceeds 5 % of busy time on a workload of the acceptance
   set.  It is below that today.
7. Phase 7, token release, grace period (several spaces), cycle end: as
   today.

### 4.5 Finalizers

The contract changes in one respect, which needs the maintainer's approval
(section 12): **built-in finalizers may run concurrently with each other**, on
the collector thread and its helpers.

- The built-in ones are safe:
  - `alignedFree` and `delete[]` are thread-safe;
  - `ProtoObjectCell::finalize` (`core/ProtoObject.cpp:515`) appends to a
    vector.  It appends to a thread-local sink set by `sweepRun` (the
    sweeper's own vector) instead of `space->gcFinalizedMutableRefs`, which
    is the fallback when no sink is set.  This change is internal: `finalize`
    is not inline and its signature is unchanged.
- **Embedder callbacks stay serial.**  A helper that meets a
  `ProtoExternalPointer` cell (`getType() == CellType::ExternalPointer`)
  records it instead of finalizing it.  The collector thread finalizes all
  recorded cells, one at a time, after the join (4.4 step 4).  Runtimes'
  callbacks were written for one finalizing thread, and this keeps them
  correct without an audit.  Allowing them to run concurrently is an open
  question.
- **Never on a mutator thread.**  Helpers are collector-owned threads
  (4.7).  No mutator ever runs `sweepRun`.  This rules out "sweep on the
  allocating core" through the mutators' refills, which the calibration
  report and the phase report mention as an option: it would move finalizers
  onto mutator threads and collection work onto the mutators.
- **Helpers never allocate.**  They do not call `getFreeCells` or
  `allocCell`.  Debug and instrumented builds assert this through a
  thread-local "is helper" flag checked in `getFreeCells`.

GarbageCollector.md § 7 gets one paragraph stating the above.  The sentence
"it runs on the single GC thread inside the sweep, so a wait there stalls
collection for the whole space" becomes "on the collector's threads".

### 4.6 Several spaces

- **One process-wide pool, not one per space.**  The cycle token
  (GarbageCollector.md § "Several spaces") already serialises cycles across
  the process, so at most one sweep inside a token runs at a time.  A pool
  per space would multiply idle threads by the number of spaces and would
  oversubscribe the machine when spaces collect back to back.
- **One job at a time.**  The pool runs a single job.  A collector that finds
  the pool busy runs its work alone (K = 0 for that phase) and never waits
  for the pool.  This can happen on the multi-space path, because the
  post-grace finalization of space A runs after A released the token, while
  B's sweep may hold the pool.  "Never wait for the pool" means no new wait
  edge exists between collectors, so no deadlock between spaces can come from
  the pool.
- **The `deferFree` path** (`ProtoSpace.cpp:915`, 1140-1175).  The sweepers
  push dead cells into their own vectors.  The grace period, the post-grace
  finalization and the publication follow today's order.  v1 runs the
  post-grace finalization serially on the collector thread.  It can use the
  pool when the pool is free, which is the same job shape over a vector
  instead of a list.
- **Grace periods.**  Helpers have no quiescence record
  (`multispace::ensureQuiescenceRecord` is never called on them), so
  `waitForGracePeriod` (`core/MultiSpace.cpp:383`) never waits for a helper.
  Helpers hold no cell of any space between jobs.  During a job they touch
  only candidate cells of the token holder's space, which no mutator can
  reach.

### 4.7 Threads

Helpers are `std::thread`s created and owned by protoCore.  This is the
precedent of the collector itself (`gcThread`, `ProtoSpace.cpp:1445`).

The "ProtoThreads, not native threads" rule binds runtimes: their threads run
user code and must take part in stop-the-world and grace periods.  It does
not fit collector threads, and a helper must **not** be a `ProtoThread`:
- a `ProtoThread` registers in `runningThreads` and in the quiescence
  records;
- a helper would then count in every stop-the-world quorum and every grace
  period, and the collector would wait for its own helpers.

### 4.8 Synchronisation

| Purpose | Primitive |
|---|---|
| idle sleep and job start | `std::mutex` + `std::condition_variable`; the predicate is `jobGeneration != seen || stopping`; `notify_all` after every change, made under the mutex |
| claims | the job's `std::mutex` (4.2) |
| completion | `activeSweepers` and `cursor`, changed under the pool mutex; the collector waits on a second `std::condition_variable` with predicate `cursor == nullptr && activeSweepers == 0`; a sweeper notifies when it decrements to 0 |
| publication | `ProtoSpace::globalMutex` for chunks (unchanged); CAS for the segment chains (unchanged algorithm, fewer operations) |

There is no spinning at idle, no timed wait, and no semaphore, latch or
barrier.  The happens-before edges are as follows:
- the mark bits written in Phase 4 are visible to the helpers through the job
  publication (mutex release, then acquire);
- the helpers' writes to cells and to per-sweeper state are visible to the
  collector through the completion handshake;
- the free chunks reach the mutators through `globalMutex`, as today.

### 4.9 Configuration

- `PROTOCORE_GC_SWEEP_THREADS=<K>`, read when the pool is first needed.
  - Proposed default: `min(3, hardware_concurrency / 4)`, which gives 3 on
    DEV12 (4 sweepers including the collector).
  - `0` gives today's behaviour, with the pool never created.
  - Invalid values are ignored, like the other `PROTOCORE_*` variables
    (README table).
- No API in v1.  A static `ProtoSpace::setCollectorHelperThreads(unsigned)`
  would be ABI-additive, and is an open question.
- The helpers take CPU from the mutators.  At N = 12 on 12 hardware threads
  that CPU is not free in general.  It is free exactly when mutators wait,
  which they do 17-63 % of the time at N = 12.  An option, as an open
  question: engage helpers only while a mutator waits for headroom, or while
  the backlog of unclaimed segments exceeds a threshold.

### 4.10 Lifecycle and teardown

- **Start:** lazily, at the first sweep that wants helpers.  Never in static
  initialisation, which on Windows runs under the loader lock in `DllMain`.
- **Stop:** when the live space count drops to zero.
  - `~ProtoSpace` of the last space stops the pool after its own
    `gcThread->join()`, inside the same `setQuiescenceOut(true)` bracket
    (`ProtoSpace.cpp:1503-1511`).
  - By then no collector exists, so no job is in flight.
  - The helpers wake on `stopping`, leave, and are joined.
  - A later space starts a new pool.
- **Never at static destruction.**  The pool object is allocated once and
  never destroyed by a static destructor.  A process that exits with a live
  space leaves parked helpers to the OS, as it does the collector thread
  today.  Destroying a joinable `std::thread` would call `std::terminate`.
- **Fork.**  Threads do not survive `fork`.  A `pthread_atfork` child handler
  marks the pool as having no threads.  Because the collector never waits for
  a helper to arrive (4.4), a pool that has lost its threads cannot deadlock
  a sweep.
- **The 2.10.0 teardown deadlock** (adaptive design § 6.10): a thread
  destroying a space joined its collector while that collector's grace
  period waited for the same thread.
  - The pool adds no edge of that kind.
  - Helpers never wait for mutators, grace periods, the token or the
    stop-the-world.
  - A collector waits for helpers only while they hold claimed work, which
    finishes without waiting on anything except `globalMutex` (held briefly,
    never across a wait).

### 4.11 Soundness

- The candidate set is fixed at Phase 2, as today.  Claims partition it, so
  each segment, and therefore each cell, is processed by exactly one
  sweeper.
- Mutators never touch candidate cells.  Dead cells are unreachable.
  Survivors' `next_and_flags` belong to the collector during sweep.  This is
  the argument of today's single-threaded sweep, unchanged; the helpers are
  the collector.
- Mark has finished before the job is published, so mark bits are read-only
  during sweep except for the owner's unmark.
- Phase 5b sees every finalized ref, because the merge precedes it.
- Phase 6 still precedes the token release.
- The grace period still precedes every finalization and reuse on the
  multi-space path.

### 4.12 What does not change

- Stop-the-world, roots, the mutable snapshot, mark, Phase 5b, the token, the
  grace period, the young-generation rules and the heap accounting.
- `getFreeCells` and the allocation fast path.
- Every public type and layout.
- With `PROTOCORE_GC_SWEEP_THREADS=0` the code path is the serial sweep, with
  F1-F4 applied.

## 5. Pacing

### 5.1 Today

- `PROTOCORE_HEAP_LIMIT_CELLS=<hard>` (the phase report's js40/js10 runs)
  sets `softHeapLimit = 0`.  With no soft limit, a fixed-limit space requests
  a cycle only when a refill finds the heap at `maxHeapSize` with an empty
  freelist (`getFreeCells`, `ProtoSpace.cpp:2081-2091`).  By then every
  mutator that needs cells is about to wait.
- With a soft limit, a refill at or above it waits for one cycle (the
  "soft zone").  That is a wait as well, not a trigger with runway.
- The wait is quantised by the 50 ms watchdog (section 1).

At N = 6 with a 40 M-cell limit, the collector is idle most of the run, yet
the mutators wait 10-22 %.  The single-threaded `core_fixed640_live1M` waits
1.25 s of a 3.6 s run (25 waits of 50 ms).

### 5.2 Proposal: early requests under fixed limits

At each refill, under `globalMutex` and off the per-cell fast path (the hook
`adaptive::pace` already occupies, `core/AdaptiveHeap.cpp:402`):

```
left   = freeCellsCount + max(0, maxHeapSize - heapSize)
runway = min(f_max * max(0, maxHeapSize - R),  m * r * Tc)
if left < runway and no cycle requested or running: request a cycle
```

| Symbol | Meaning | Default |
|---|---|---|
| R | cells the last cycle left unreclaimed (`heapSize - freeCellsCount` right after its sweep; the controller's `retainedLastCycle`) | |
| r | allocation rate: cells handed out by `getFreeCells` per second since the last cycle end (counted under the lock already held) | |
| Tc | duration of the last cycle (already measured for `PROTOCORE_HEAP_TRACE`) | |
| m | margin | 1.25 |
| f_max | cap, so the trigger never runs cycles back to back | 0.5 |

- Before the first cycle, `runway = 0`, which is today's behaviour.
- The function `pacing::fixedRunway(R, max, r, Tc, params)` is pure and
  unit-tested.
- State lives in the side structure keyed by space, which is extended to
  fixed-limit spaces.

This is not the "rate-aware target" the calibration rejected.  That proposal
*sized the soft limit* from rate x mark time and gave too little headroom.
Here the limit is fixed by the embedder; only the moment a cycle starts
moves.  The cost:
- cycles start with less garbage, at most 2x as many cycles when f_max = 0.5
  binds, each paying a mark of the live set;
- mutators running at the request pay the stop-the-world quorum, which is
  near zero today because waiting mutators are already out of the running
  set.  Under the controller's pacing at N = 12 the quorum was 28-270 ms per
  run and the parked time 0.4-2.7 s summed over 12 threads, below 1 % of
  thread time.

Default: on, with `PROTOCORE_GC_PACING=0` to disable, subject to the
acceptance runs.  This is an open question.

### 5.3 Waits that end when cells arrive

- **Fixed limits.**  `reclaimWaitLocked` keeps its predicate.  In addition,
  `publishFreeChunk`'s callers `notify_one` on `memoryReclaimedCV` when a
  waiter count (incremented and decremented around the wait, under
  `globalMutex`) is non-zero.  `waitForHeapHeadroom` then re-checks the
  freelist, as it does after a watchdog wake, so the notify needs no new
  predicate.  It costs nothing when no thread waits.
- **Controller.**  Early wake was rejected in the calibration: 18-25 %
  slower on the 1 M-cell single-threaded benchmark, mixed elsewhere.  It is
  re-measured at N = 12 only after C6 shows how much of the wait is
  watchdog latency, and only with the parallel sweep in place.
- The OOM strike logic is unaffected.  A wake that finds an empty freelist
  goes through the same "cycle completed?" check (line 1839).

### 5.4 Why the controller was slower at N = 12

Same hard limit (40 M cells), fixed against controller, from the phase report:

| Workload | Cycles | Mark s | Sweep s | Busy s | Wait s (sum) | Quorum ms |
|---|---|---|---|---|---|---|
| records | 10 -> 22 | 7.5 -> 8.7 | 27.0 -> 24.3 | 37.0 -> 35.1 | 231 -> 351 | 0.9 -> 57 |
| join | 8 -> 17 | 10.0 -> 12.7 | 18.3 -> 20.9 | 29.9 -> 35.5 | 178 -> 208 | 10.5 -> 116 |
| doctree | 6 -> 14 | 6.0 -> 9.2 | 13.4 -> 17.3 | 21.7 -> 29.3 | 121 -> 167 | 0.4 -> 28 |
| wordfreq | 5 -> 24 | 1.7 -> 8.2 | 6.0 -> 10.7 | 8.5 -> 20.2 | 31 -> 191 | 1.9 -> 261 |
| graph | 17 -> 21 | 15.5 -> 20.6 | 37.3 -> 35.9 | 56.3 -> 60.7 | 364 -> 396 | 2.0 -> 270 |

Reading:

1. **The controller runs more cycles on a collector that is already
   saturated** (69-95 % busy).  Its soft limit is capped at `8 L`, which is
   below H at these live sets.  Whether S actually reached the cap in these
   runs is not in the phase report; the `PROTOCORE_HEAP_TRACE` lines must
   confirm it.
2. **Each extra cycle pays a fixed cost:** the mark of the live set, and the
   sweep of the survivors re-included every cycle (survivor stagger 1).  On
   `wordfreq` the mark grows 4.8x for the same work.  On a saturated
   collector, extra collector work becomes mutator wait almost one for one.
3. **The calibration's premise does not hold here.**  The premise was that a
   larger S only makes each cycle longer, because the sweep is proportional
   to the garbage.  At N = 12 mark is 23-41 % of the controller's busy time,
   and a larger S *does* reduce total work by running fewer marks.  The cap
   trades this time for memory: peak RSS was 2-21 % lower on four of the
   five workloads.
4. **Pressure cannot see throughput.**  p is summed over threads and far
   above `p_high`, so S grows to the cap and stays there.  The control law
   has no signal that says the collector is the bottleneck.

**Consequences for this design:**
- A faster sweep shrinks reason 2's sweep half and lowers the saturation, so
  the controller's gap should shrink with it.  This is a hypothesis, measured
  in section 8.
- The mark half is untouched by a parallel sweep.  After it, mark becomes a
  larger share, and the cap trade-off sharpens.
- **Proposed now:** add collector utilisation `u = busy / T` to the
  controller's trace line (measurement only, no law change).
- **Open question:** whether the cap should rise when `u` is high and mark is
  a large share.  That is the "second signal" anticipated in the controller
  design's open point 2.

### 5.5 Controller pacing

Unchanged in v1: request at a quarter of `S - L` (`kTriggerFraction = 0.75`,
`core/AdaptiveHeap.h:51`).  Fixed-limit pacing (5.2) applies only to spaces
without the controller.  The two rules are unified (one runway function, with
`S` or `maxHeapSize` as the ceiling) only after both are measured.

## 6. Mark: not parallelised now

The data says mark is secondary:
- 1-50 % of busy time;
- every completed workload reaches its zero-wait bound with a faster sweep
  alone;
- mark throughput degrades much less under threads (11-22 M cells/s down to
  7-16 M).

Mark leads only near a full heap (`wordfreq` N = 12 and `graph` N = 1 at
10 M cells, and the runs that ended out of memory).

**Revisit when any of these holds:**
- after the parallel sweep meets its target, mark exceeds 40 % of busy time
  on a completed workload of the acceptance set;
- the collector is still more than 80 % busy at N = 12;
- the maintainer prioritises workloads whose live set is above half the
  limit;
- the controller question of 5.4 is answered with "keep the cap", which makes
  mark the dominant cost of the extra cycles.

**What a parallel mark would need**, as a sketch only:
- per-thread work lists with stealing;
- `fetch_or` returning the previous bit, so that exactly one marker claims a
  cell;
- per-thread `markedList`s;
- the young-chain and pen walks split by chain.

No barrier and no change to the snapshot discipline: the graph traversed is
immutable.

## 7. Expected gains (bounds, not predictions)

From the phase report's Amdahl table.  The wall-time bound assumes:
- the collector fully overlaps the mutators;
- the waits are spread evenly over the threads;
- the CPU a parallel sweep takes is free.

| Workload | Wall s | Bound, sweep x2 | Bound, sweep x4 | Zero-wait floor |
|---|---:|---:|---:|---:|
| js40_records_n12 | 41.6 | 23.5 (-43 %) | 22.3 (-46 %) | 22.3 |
| js40_graph_n12 | 61.2 | 37.6 (-38 %) | 30.8 (-50 %) | 30.8 |
| js10_records_n12 | 20.1 | 11.2 (-44 %) | 8.9 (-56 %) | 8.9 |
| jsad_records_n12 | 46.3 | 23.0 (-50 %) | 17.1 (-63 %) | 17.1 |
| scala_tree_t6 | 89.6 | 49.3 (-45 %) | 49.3 (-45 %) | 49.3 |
| clj_coll_t6 | 129.2 | 61.6 (-52 %) | 43.4 (-66 %) | 43.4 |
| js40_*_n6 | 8.2-19.3 | no sweep speed-up needed | | -10 % to -22 % |

- The N = 6, 40 M-cell rows need pacing (section 5), not a faster sweep.
- `core_fixed640_live1M` is single-threaded and loses 35 % of its wall time
  in waits (1.25 s of 3.6 s).  That is a pacing case too.
- CAD 20k mixes single-threaded and N = 12 phases, and the phase report
  gives no bound for it.  It is measured, not predicted.
- **Whether the sweep scales is unproven.**
  - If the per-cell cost is latency (H1/H2 dependent misses), independent
    chains on K sweepers add memory-level parallelism, much as F4 does on one
    thread, and should scale well below the bandwidth limit.  At 12-17 M
    cells/s per sweeper the sweep moves under 1.1 GB/s of 64-byte lines.
  - If it is contention on shared lines (H3), more sweepers make it worse
    until F1/F5 remove it.
  - Section 3's gate exists for this reason.

## 8. Acceptance criteria and measurement plan

**Workloads:** the phase report's, run with the same binaries method (the
installed runtimes against the development library through
`LD_LIBRARY_PATH`, checked with `ldd`):
- protoJS structures `MODE=par` at 40 M cells (REPS 3) and at 10 M cells
  (REPS 1), for N = 1, 6, 12, under fixed limits;
- the same with `PROTOCORE_ADAPTIVE_HEAP=1` at N = 12;
- protoScala `tree_alloc` (t1, t6) and protoClojure `coll_alloc` (t1, t6).
  These two scripts are not committed yet; commit them, or a protoCore-side
  equivalent, before measuring;
- protoJS CAD 20k (`NS=1,12`);
- protoCore `adaptive_heap_benchmark` with a 1 M live list (fixed 640 MB and
  controller);
- the new `sweep_contention_benchmark`.

**Run rules:**
- **Median of 3**, each in `systemd-run --user --scope -p MemoryMax=<cap>
  -p MemorySwapMax=0` under `/usr/bin/time`.
- Sequential, on a machine with no other build or test running.
- Every workload verifies itself: the `ok` lines and the checksums listed in
  the phase report.  A run that does not self-verify is a failure, never a
  data point.

| Criterion | Threshold |
|---|---|
| Sweep throughput at N = 12 / t6 (cells/s, `[GC-PHASES]`) | at least 2x baseline on js40 `records` and `graph`, `scala_tree_t6`, `clj_coll_t6` |
| Wall time at N = 12 / t6 | at least -25 % on js40 `records` and `graph`; at least -30 % on `clj_coll_t6` |
| Headroom wait share at N = 6, 40 M cells (pacing) | at most half of baseline (10-22 % today) |
| Single-thread wall time (js10 N = 1, scala t1, clj t1, `core_fixed640`, CAD N = 1) | no regression beyond the noise band of the calibration report's "Noise" section (re-measured: median of 5) |
| Peak RSS | at most +5 % on any workload |
| Stop-the-world maximum per cycle | unchanged (at most 0.25 ms) |
| `PROTOCORE_GC_SWEEP_THREADS=0` | identical cycle counts and freed cells to the F1-F4 baseline on the deterministic tests |
| Sanitizers | full ctest green under TSan and ASan, sequential, plus the new tests |
| CI | Linux and cross-platform workflows green, Windows included (helpers are `std::thread`; no POSIX-only call outside `pthread_atfork`, guarded) |

**Measurement steps:**
1. Baseline at the current master.
2. Instrumentation C1-C6 and the microbenchmark.
3. Diagnosis report.
4. F1-F5, each with its own numbers.
5. Gate decision.
6. Parallel sweep, then K = 0, 1, 2, 3, 5 on the N = 12 and t6 workloads.
   This gives the measured scaling curve, the whole curve and not one point.
7. Pacing.
8. Final matrix.

Results go in `docs/reports/`, with every unproven claim marked as such.

## 9. Test plan

All tests are deterministic: they count, and never time.  Tests that need
threads assert invariants on counts, not on interleavings.

1. **Claiming partitions the list.**  A unit test drives the claim function
   over synthetic `DirtySegment` lists of length 0, 1, R-1, R, R+1 and 10^5,
   with 1 and 8 concurrent claimers.  Every segment is claimed exactly once,
   and no claim returns an empty run while unclaimed segments remain.
2. **Same result as the serial sweep.**  A space with a root set holding a
   known structure builds a known amount of garbage.  One cycle with K = 0
   and one with K = 3 must give the same `reclaimedLastCycle`,
   `liveCellsLastCycle` and freelist count.  After the cycle no reachable
   cell carries a mark bit (the `GCMarkTests` pattern).
3. **Finalizer thread identity.**  `ProtoExternalPointer` finalizers record
   `std::this_thread::get_id()`.  The test records the ids of every mutator
   thread.  It asserts that no finalizer id is a mutator id, and that every
   external-pointer finalizer ran on the space's collector thread (the serial
   rule of 4.5).
4. **Embedder finalizers do not overlap.**  An in-flight counter inside the
   callback never exceeds 1, with K = 3 and 10^5 external pointers.
5. **Mutable refs are merged before Phase 5b.**  The
   `MutableRootReclaimTests` scenarios run with K = 3.  Dropped mutables'
   entries are released and their states freed one cycle later.
6. **Survivor pen.**  `GCSurvivorRechainTests` runs with K = 3, with
   `PROTOCORE_GC_SURVIVOR_STAGGER=1` and `=4`.
7. **Helpers never allocate.**  In debug builds, a helper calling
   `getFreeCells` aborts.  A death test confirms that the assertion fires.
8. **Teardown.**
   - Destroy a space while a K = 3 sweep is in progress (large garbage set,
     cycle requested, destructor called at once), 1,000 iterations.
   - Two spaces destroyed in both orders.
   - Destroying the last space stops the pool, and a new space restarts it.
   - A subprocess that exits with a live space exits with status 0, with no
     `std::terminate`.
9. **Several spaces.**
   - Two spaces collecting concurrently with K = 3: the pool's one-job rule
     holds, checked by a counter of concurrent jobs, at most 1.
   - The `deferFree` path frees the same cells as K = 0.
   - `MultiSpaceTeardown.DestroyingASpaceWhileItsCollectorWaitsForAGracePeriod`
     passes with K = 3.
10. **Fork.**  On POSIX only: a child forked after the pool started completes
    a cycle with K configured.
11. **Configuration.**  `PROTOCORE_GC_SWEEP_THREADS` values 0, 1, 64 and
    `abc`; invalid values fall back to the default.
12. **Pacing.**
    - Unit tests of `pacing::fixedRunway`: zero before the first cycle,
      monotone in r and Tc, capped by f_max.
    - Integration: under a fixed limit with a steady allocator, the second
      and later cycles start with `heapSize < maxHeapSize`, as shown by the
      `fixed` trace line.
    - `AllocationLimitTests` and `HeapHeadroomWaitTests` still pass, and the
      OOM tests still abort with the same message.
    - Early wake: a waiter blocked at the ceiling returns before the 50 ms
      watchdog once a chunk is published (asserted through a wake-reason
      counter, not through time).
13. **Stress.**  `GCStressTests` and `ConcurrentMarkSafetyTests` run with
    K = 3, under TSan and ASan.

## 10. Implementation steps

Each step is a separate commit and is measured on its own:
1. Instrumentation C1-C6 and the microbenchmark, then the diagnosis report.
2. F1, F2, F3, F4 (and F5, F6, F7 only if the diagnosis calls for them).
3. Gate review with the maintainer.
4. The sweep refactored into `sweepRun` with per-sweeper state, K = 0 only.
   No behaviour change; tests 2, 5 and 6 pass.
5. The pool, claiming and the join; finalizer sink and serial embedder
   finalizers; teardown and fork.  Tests 1-11.
6. Pacing under fixed limits and early wake; tests 12.  Controller trace
   gains `u`.
7. Documentation: GarbageCollector.md (§ 7, Phase 5, Synchronisation,
   Several spaces), README variable table, CHANGELOG; acceptance report.

## 11. Risks

- **The sweep does not scale** (memory-bound or contention-bound).  The
  diagnosis gate addresses this.  F4 may deliver the latency part without
  threads.
- **CPU taken from mutators at saturation.**  The default K is small.  The
  option to engage helpers only while mutators wait is an open question.
- **Finalizer concurrency.**  Built-in finalizers become concurrent with each
  other.  A future built-in finalizer must be thread-safe, and the contract
  says so.  Embedder callbacks stay serial.
- **Teardown and fork deadlocks.**  These are addressed by the "never wait
  for arrival or for the pool" rule and by tests 8 and 10.  They are the
  class of bug most likely to escape a sequential test run: run the teardown
  test 1,000 times under TSan.
- **Pacing increases cycles** under fixed limits (at most 2x with
  f_max = 0.5), and with them mark work and quorum time.  Measured; default
  on only if the acceptance holds.
- **Measurement noise on a shared machine.**  Median of 3, sequential, no
  concurrent builds.  The phase report itself used single runs.
- **Windows.**  Pool creation must not happen under the loader lock (lazy
  start).  MSVC's `std::thread` and condition variables are fine.
  `pthread_atfork` is POSIX only.

## 12. Open questions for the maintainer

1. **Finalizer contract:** may built-in finalizers run concurrently with each
   other on helper threads (4.5)?  And should embedder `ProtoExternalPointer`
   callbacks stay serial on the collector thread (proposed) or become
   concurrent after an audit of the runtimes?
2. **Helper default and engagement:** `min(3, hw/4)` always available, or
   engaged only while mutators wait for headroom or a backlog exists?  Is an
   API (`setCollectorHelperThreads`) wanted, or the environment variable
   only?
3. **Fixed-limit pacing on by default?**  It changes when cycles start under
   `setHeapLimits` and `PROTOCORE_HEAP_LIMIT_CELLS` (more, shorter cycles;
   fewer waits).
4. **Early wake under fixed limits** (5.3): accept a `notify_one` per
   published chunk while waiters exist, given that the calibration rejected
   early wake under the controller on single-threaded workloads?
5. **Controller cap and collector utilisation** (5.4): should the cap rise
   when `u` is high and mark is a large share (time over memory), or stay at
   `8 L` (memory over time)?
6. **Per-thread dirty-segment stacks** (4.2): worth pursuing if C2 shows
   contention on the single `dirtySegments` head at context destruction?
   This is mutator-side cost, outside the collector.
7. **Side mark bitmap** (3.3): out of scope unless H1/H5 dominate after F2
   and F3.  Agreed?
8. **The two new runtime workloads** (`tree_alloc.scala`, `coll_alloc.clj`):
   commit them to their runtimes' benchmark directories so that the
   acceptance runs are reproducible?
