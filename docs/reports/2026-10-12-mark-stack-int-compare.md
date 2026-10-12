# The mark work list and integer comparisons (2.14.6)

Date: 2026-10-12.  Author: Gustavo Marino, with Claude.

protoST's pre-talk audit (protoST
`benchmarks/reports/2026-10-12-pre-talk-063.md`, sections 2 and 3) left two
findings in protoCore:

- **(a)** under a 640 MB cell limit, protoST reached 1.19 GB resident, and
  the excess was the collector's mark work list, a `std::vector` outside
  the cell heap that doubled up to 512 MB;
- **(b)** an integer comparison allocated a temporary, which made the
  single-thread half of the talk's demo 2 depend on the glibc version.

**Everything here is synthetic and notebook-class**: DEV12 (AMD Ryzen 5
5500U, 6 cores / 12 threads, two DDR4 channels, a desktop with an editor and
a browser running).  No server was measured.

## Verdict

| | Root cause | Fix | Before -> after |
|---|---|---|---|
| (a) | Three, measured on the reproduction: the young-chain walk pushed every reference of ~10 M young cells before the first pop (35 M entries); a cell was marked when popped, so 82 % of the 51 M pops found it already marked; the main thread's young chain was walked twice | Mark-on-push; each source traced as it is visited; a fixed-capacity mark stack (1 M entries, 8 MiB) with overflow rescan; the marked list in blocks; contexts and young chains visited once | `(1 to: 500000) asArray`, fixed 640 MB limit: peak RSS **1,165 -> 690 MB**, 5.01 -> 2.53 s to the out-of-memory exit; mark stack peak 35 M -> **19 entries** |
| (b) | `Integer::compare`, `sign`, `negate`, `abs`, `divide`, `modulo` converted SmallIntegers into a `TempBignum` whose `std::vector` allocated (2 allocations per compare, about 7 per divide or modulo) | Two SmallIntegers answered from their values; a SmallInteger against a LargeInteger by the LargeInteger's sign; promotion and errors unchanged | trial-division microbench **12.28 G -> 0.59 G cycles**; demo 2 one thread 1,313 -> 879 ms |


Costs: the mark phase spends about 4 % more per marked cell on a heap with
no young-generation pressure (`adaptive_heap_benchmark`, 70.1 -> 72.9 ns);
the collector workloads are otherwise unchanged or faster, except
`scala_tree_t6`, consistently about 4 % slower (section 3).  Demo 2's
printed speedup ratio *falls* (median 2.99 -> 2.66), because its one-thread
half gains more than its twelve-actor half; both halves are faster.

## 1. Facts first

**(a) Why the work list reached 512 MB.**  A temporary instrumentation of
the mark loop on protoST 0.6.4 running `(1 to: 500000) asArray` with
`PROTOCORE_ADAPTIVE_HEAP=0` (first cycle):

| | Value |
|---|---|
| roots pushed in the pause | 313 |
| young chain heads captured | 2, **the same head twice** (9.9 M cells each) |
| young cells walked | 19.8 M (twice 9.9 M) |
| work list after the young walk, before the first pop | **35.4 M entries** (the peak) |
| pops | 51.5 M, of which **42.5 M (82 %) found the cell already marked** |
| cells marked | 9.0 M |
| capacities | work list 67 M entries (512 MB), marked list 16.8 M (128 MB) |

So it was a breadth explosion *and* duplicates, compounded by a double walk:

- the young-chain walk (and the pen walk) pushed every reference of every
  young cell **before** the mark loop popped anything; the young cells here
  are the intermediate versions of a structure built by path copying, so
  most of their references point to shared subtrees;
- marking happened on **pop**, so a cell referenced N times was pushed N
  times;
- the main thread's contexts were scanned twice: once through its
  `ProtoThread` (in `space->threads`) and again as `space->mainContext`,
  which is on that same chain once the main thread is adopted.  Their roots
  were pushed twice in the pause and their young chain walked twice.

A wide node is not the cause: protoCore's cells hold at most a handful of
references, and a depth-first traversal of a balanced tree needs a few
dozen entries (19 to 21 measured after the fix).

**(b) Where the comparison allocated.**  The profile in protoST's report
(`perf-d2h.txt`) puts `proto::toTempBignum` at 7.6 % and libc
`malloc`/`free` at 18 % of demo 2's one-thread half.  The call path is
`ProtoObject::compare` -> `partialCompare` -> `Integer::compare` ->
`toTempBignum`, which builds a `TempBignum` holding a `std::vector` for
each operand: an `operator new`, not a cell.  `test/int_alloc` counts
`operator new` per operation on two SmallIntegers against 2.14.5:

| Operation (10,000 calls) | Allocations, 2.14.5 | 2.14.6 |
|---|---:|---:|
| `compare`, `partialCompare` | 19,999 each | 0 |
| `integerSign`, `negate`, `abs` | 9,999 each | 0 |
| `divide` | 66,667 | 0 |
| `modulo` | 68,569 | 0 |
| SmallInteger vs LargeInteger `compare` | 99,998 | 0 |

The last row matters for protoST, whose `fitsDivisorWord`
(`src/primitives/IntegerDivision.h`) compares every divisor with
`fromLong(±2^62)`, two LargeIntegers, before each `//` and `\\`.  Building
those two constants still allocates on every division; that part is in
protoST and is reported, not changed (protoST 0.6.4 is the talk's release).

## 2. The fixes

**(a)** `core/MarkStack.h`, `core/ProtoSpace.cpp`; described in
docs/GarbageCollector.md, "Mark work list".

- *Mark-on-push.*  A cell is marked, recorded and pushed in one step, so the
  stack holds each cell at most once.  Only the collector thread reads or
  writes mark bits, so the marked set is the same as before.
- *Each source traced as it is visited.*  Each root is shaded, each young or
  pen cell scanned, and the stack drained right after.
- *A bounded stack.*  1 M entries (8 MiB), allocated on first use and reused
  across cycles.  A push that finds it full leaves the cell unmarked and
  raises an overflow flag; the collector then traces the sources again and
  rescans every cell marked before the pass, until a pass completes without
  overflow (each overflowing pass marks a full stack of new cells, so it
  terminates).  This is Boehm's and HotSpot's overflow recovery, with the
  marked list in place of a heap scan.
- *The marked list in 256 KiB blocks*: no copy on growth (the old vector
  held its old and new buffers at each doubling), four blocks retained.
- *Visited once*: `mainContext` is skipped when a thread's chain covered it;
  young chain heads are deduplicated.
- The unmark loop prefetches 16 entries ahead, and the references of one
  scanned cell are prefetched and shaded in batches of 8 (the miss on a
  child's header moved from the pop, where it was prefetched, to the push).

Alternatives considered: *pushing large objects as ranges* is unnecessary,
no cell is wide; *a bounded stack without mark-on-push* keeps duplicates,
so an overflowing pass need not make progress; *counting the side memory in
the controller's budget* would change H's documented meaning, while the
stack is now 8 MiB at most and the marked list an eighth of the live cells'
bytes, inside the 25 % the default budget leaves (docs/MemoryModel.md § 3).

**(b)** `core/Integer.cpp`, `core/ProtoObject.cpp`.  Two SmallIntegers are
answered from their values in `compare`, `sign`, `negate`, `abs`, `divide`
and `modulo`; results outside the range go through `fromLong`, which
promotes exactly as the general path did (`-(-2^53)`, `-2^53 / -1`); division
by zero throws the same error.  A SmallInteger against a LargeInteger is
decided by the LargeInteger's sign, which is exact because `fromTempBignum`,
the only constructor of a LargeInteger, returns a SmallInteger whenever the
value fits.  `ProtoObject::compare` and `partialCompare` test two
SmallIntegers before any type probe.  LargeInteger and double operands keep
the general path.

**Tests** (each fails against 2.14.5): `GCMarkStack.LargeYoungGenerationDoesNotFloodTheStack`
(peak 1,544,226 entries before, bound 4,096), `GCMarkStack.EachYoungChainIsWalkedOnce`
(654,510 cells walked for a 327 k-cell chain before),
`GCMarkStack.OverflowRescanMarksTheSameLiveSet` (a 16-entry stack; same
marked count as a cycle with room; 20,000 elements intact after three more
tight cycles), `integer.small_operations_do_not_allocate`.
`IntegerSmallPathTest.*` pins the results at the range's edges and passes on
both paths.  Suite: 668/668 Release, 658/658 ASan with leak detection and
658/658 TSan (the clock-dependent cases excluded as in CI).

## 3. Measurements

**Method.**  Before: master `5257b37d` (2.14.5).  After: `9304945d` (both
fixes without the mixed SmallInteger/LargeInteger compare, which was added
afterwards; it only affects mixed integer comparisons).  Both built here
with GCC 15.2, Release, with and without `PROTOCORE_GC_INSTRUMENT`.  Both
sides run the **same installed runtime binaries** (protoST 0.6.4,
protoPython 1.0.0, protoJS 0.1.0, protoScala 0.6.0, protoClojure 0.0.1) and
differ only in the `libprotoCore.so.3` that `LD_LIBRARY_PATH` selects
(checked with `ldd`).  Interleaved run by run, behind the 2026-10-04
harness's quiet gate, each run verifying its own output, each in a
`systemd-run` scope with a memory cap.  Session 2026-10-12 18:43-20:02 UTC.
Harness: [data/2026-10-12-mark-stack-int-compare/](data/2026-10-12-mark-stack-int-compare/)
(`spec.py` the specific runs, `fm.py`/`tab.py` the 2026-10-04 harness with
the sides changed, `run_all.sh` the order, `results/` the records).

### 3.1 The two findings (medians of 5; demo 2 of 10)

| Workload | Metric | 2.14.5 | 2.14.6 | Change |
|---|---|---:|---:|---:|
| K5, fixed 10 M-cell limit (exit 3, OOM) | peak RSS | 1,165 MB | 690 MB | **-41 %** |
| | wall to the OOM exit | 5.01 s | 2.53 s | -50 % |
| | mark time (instrumented, 4 cycles) | 4.16 s | 1.74 s | -58 % |
| | young cells walked (4 cycles) | 79.7 M | 39.8 M | -50 % |
| | mark stack peak / marked list | 512 MB vector | 19 entries / 69 MB | |
| K5 under the controller (completes) | peak RSS | 718 MB (one run 1,230) | 723 MB | = |
| | wall | 1.51 s | 1.06 s | -30 % |
| primes_bench 48000 x 10 (`perf stat -r 5`) | cycles | 12.28 G | 0.594 G | **-95 %** |
| | instructions | 30.86 G | 2.15 G | -93 % |
| demo 2, one thread | time printed | 1,313 ms | 879 ms | **-33 %** |
| demo 2, 12 actors | time printed | 446 ms | 329 ms | -26 % |
| demo 2 | printed ratio | 2.99 | 2.66 | -11 % |
| `adaptive_heap_benchmark` fixed, 1 M live | mark ns per marked cell | 70.1 | 72.9 | +4 % |
| | unmark (12 cycles) | 121 ms | 120 ms | = |
| | collector busy | 1.70 s | 1.67 s | = |
| | pause per cycle (mean / max) | 17 / 39 µs | 19 / 38 µs | +2 µs |

`primes_bench` (`primes_bench.cpp`) is demo 2's `isPrime` written against
protoCore's public API, so its gain is the upper bound of (b) for that
program; demo 2 itself spends most of its time in the interpreter.  Its
twelve-actor half on this desktop (a browser and an editor running) is
noisy (291-422 ms after, 385-521 before); the absolute figures are lower
than the 4.45x of protoST's own quiet-session report.  **The ratio demo 2
prints falls with 2.14.6**: a faster single thread is the denominator's
gain, not a loss of parallelism.

### 3.2 Collector workloads (the phase report's set; medians of 3)

The 2026-10-04 `gc` group with the instrumented libraries, without the
protoJS `join` runs at 10 M cells (out of memory on both sides).  Full table
with waits, sweep and CPU: [results/gc-table.md](data/2026-10-12-mark-stack-int-compare/results/gc-table.md).

| Workload | Wall s | Peak RSS MB | Mark s | Max pause ms |
|---|---|---|---|---|
| core_fixed640_live1M | 2.41 -> 2.39 | 673 -> 655 | 0.84 -> 0.87 | 0.03 -> 0.06 |
| js40_records_n12 | 18.73 -> 18.47 | 2592 -> 2527 | 6.34 -> 6.20 | 0.10 -> 0.14 |
| js40_join_n6 | 9.38 -> 8.22 | 2274 -> 2509 | 3.76 -> 3.73 | 0.06 -> 0.07 |
| js40_doctree_n12 | 16.48 -> 18.48 (spread 16.2-18.3 / 16.7-20.2) | 2658 -> 2602 | 8.75 -> 9.55 | 0.14 -> 0.17 |
| js40_wordfreq_n12 | 12.26 -> 10.68 | 2665 -> 2543 | 5.46 -> 4.02 | 0.13 -> 0.12 |
| js40_graph_n12 | 37.44 -> 33.69 | 2755 -> 2553 | 22.19 -> 17.81 | 0.10 -> 0.11 |
| js10_wordfreq_n12 | 8.00 -> 6.16 | 853 -> 755 | 4.00 -> 2.70 | 0.11 -> 0.22 |
| js10_graph_n12 (OOM 2/3 before, 1/3 after) | 45.19 -> 33.98 | 924 -> 694 | 32.57 -> 22.27 | 0.10 -> 0.13 |
| jsad_graph_n12 (controller) | 38.46 -> 33.64 | 2786 -> 2569 | 22.61 -> 17.95 | 0.13 -> 0.12 |
| scala_tree_t1 | 11.73 -> 10.37 | 161 -> 159 | 0.14 -> 0.11 | 0.04 -> 0.04 |
| scala_tree_t6 | **48.29 -> 50.35** (47.6-49.6 / 50.2-51.0) | 177 -> 176 | 11.91 -> 12.15 | 0.09 -> 0.09 |
| clj_coll_t1 | 10.89 -> 10.64 | 137 -> 136 | 0.09 -> 0.10 | 0.06 -> 0.04 |
| clj_coll_t6 | 38.78 -> 39.47 | 153 -> 154 | 2.12 -> 2.07 | 0.13 -> 0.08 |
| cad_20k (the CAD model) | 123.91 -> 118.24 | 1198 -> 1092 | 74.05 -> 67.33 | 0.11 -> 0.12 |

- Mark time falls where the young generation is large or shared
  (`graph`, `wordfreq`, the CAD model: -9 to -33 %), and is within the
  run-to-run spread elsewhere.  Peak RSS falls on 34 of the 39 rows and
  rises on 5 (`js40_graph_n1`'s is bimodal on both sides, 336-676 MB).
- The pause is unchanged in scale (all maxima below 0.25 ms); single
  maxima move by tens of microseconds both ways.
- `scala_tree_t6` is about 4 % slower in every repetition, with the extra
  time in the sweep (33.1 -> 34.1 s), which this change did not touch, and
  fewer cycles; not explained.  `js40_doctree_n12`'s median is +12 % but the
  repetitions overlap.
- Not measured to completion: the calibration group (`cal`), stopped after
  67 records to release first; the records taken are in `results/cal.jsonl`.
- CI: on the release branch the informational (never gating) clock case
  `MPSCQueueGC.LargeDrainDoesNotBlockStopTheWorld` failed once on Windows
  Release on its sampling precondition (3 of 9 cycles landed in the drain
  window); the pauses it measured were 25-28 µs median, against a 300 µs
  bound.

## Cross-runtime comparisons

What (b) does to each runtime's comparison with its usual competitor
(CPython, Node, the JVM, Babashka) is in the companion report,
[2026-10-12-cross-runtime-int-compare.md](2026-10-12-cross-runtime-int-compare.md):
only division, remainder, protoPython's unary minus and protoJS's `===`
reach the changed code; most harness loops are unchanged.

## What this does not show

- No server and no other machine: one notebook-class desktop.
- The "after" library of section 3 lacks the mixed SmallInteger/LargeInteger
  compare (commit after `9304945d`), which removes further allocations from
  protoST's `//` and `\\`; its effect on demo 2 is not in these figures.
- The marked list is still proportional to the live set (8 bytes per live
  cell) and is not counted in any budget.
- The overflow rescan is exercised by a test with a 16-entry stack, never by
  a workload: no measured cycle came close to 1 M entries (the largest peak
  was 21).
