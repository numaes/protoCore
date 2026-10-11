# Collector adjustments after the final re-measurement (2.14.2)

Date: 2026-10-04.  Author: Gustavo Marino, with Claude.

The [final re-measurement](2026-10-04-final-remeasurement.md) listed four
regressions of 2.14.1 against 2.10.2 (its section 6).  The maintainer
approved four adjustments:

- **A1** engage the multi-cursor sweep only when it pays (protoClojure
  `coll_alloc` with one task, +26 %);
- **A2** request paced cycles early only when the runway would otherwise
  run out (six threads at 40 M cells, +1 to +6 %, cycles doubled);
- **A3** start the adaptive controller where it does not loop on futile
  cycles (protoST `fib.st` 3.4 times slower under the controller);
- **A4** explain `immutable_sharing_benchmark`'s +8.5 % cycles.

**Everything here is synthetic and notebook-class**: the re-measurement's
workloads, on DEV12 (AMD Ryzen 5 5500U, 6 cores / 12 threads in two core
complexes of 4 MB L3, two DDR4 channels, a desktop with an editor and a
browser running).  No server was measured.

## Verdict

| | Cause | Change | Before -> after (medians of 3) |
|---|---|---|---|
| A1 | The 8-chain sweep ran far ahead of a lone mutator, which then wrote cells gone cold or modified in the collector's cache | One chain for a single allocating thread that holds pacing's runway; wide otherwise | `clj_coll_t1`: 2.10.2 11.25 s, 2.14.1 14.90 s, 2.14.2 **11.82 s**; multi-threaded rows unchanged (-6.3 to +4.8 %, within the run-to-run spread) |
| A2 | Not reproduced: 2.14.1 was not slower than 2.10.2 at six threads in this session, and pacing's early starts paid for themselves there | None (a shorter runway was implemented, measured and removed) | `js40_doctree_n6` 9.85 / 9.69 / 9.10 s, `js40_wordfreq_n6` 7.60 / 7.29 / 7.49 s (2.10.2 / 2.14.1 / 2.14.2) |
| A3 | A start-up live set just below S0: a futile first cycle the law skipped, S kept at S0 | After a cycle that reclaimed less than its live set, S >= min(H, 2 L + runway), by at least a doubling | `st/fib` under the controller 1.56 -> **1.29 s** (3 -> 2 cycles); protoST's fixed limit: 0.42 s |
| A4 | Code layout: identical instructions, hot functions moved across 64-byte lines, 41x more op-cache/decoder switches | None in code; with every function 64-byte aligned the cycles return (-0.9 % against 2.10.2) | 2.10.2 3.023 G, 2.14.1 3.165 G (+4.7 %) cycles |

Remaining: `clj_coll_t1` is still 5-7 % slower than on 2.10.2, from
pacing's extra cycles beside a single mutator (A2's data shows it; no rule
was adopted that helps it without letting waits back in elsewhere); protoST
`fib.st` remains slower under the controller than with protoST's fixed
limit (the start-up cycle and a cycle waited for at exit); the layout
lottery of A4 is unaddressed.

## Method

- **Libraries.**  "2.14.1" is master `1365c3af` rebuilt in this session,
  "2.14.2" the branch at `2618fc0f` (the release adds `054d39a9`, which
  only fixes the walk's width at sweep start when several threads
  allocate; one A/B on `js40_records_n12` showed no difference beyond the
  spread), except the three-way table of A2, measured at `d5fca7b0` (which
  differs from the release only in the helpers' measured engagement), both
  Release and, for the collector figures, Release with
  `-DPROTOCORE_GC_INSTRUMENT=ON`.  "2.10.2" is the re-measurement's before
  side (its 2026-10-03 runtime binaries and instrumented library).  The
  runtime binaries of 2.14.1 and 2.14.2 rows are the re-measurement's after
  side (the 2026-10-04 packages); only `libprotoCore.so.3` differs, loaded
  through `LD_LIBRARY_PATH` (the instrumented runs print the `[GC-PHASES]`
  line only the instrumented library emits).
- **Harness.**  The re-measurement's `fm.py`, extended with the two sides
  (`ref`, `adj`) and a stricter quiet gate: besides compilers and test
  suites, no runtime process (`protojs*`, `protopy*`, `protost*`,
  `protoclj*`, `protoscala*`) and no test262 driver may run, and fewer than
  2.5 CPUs may be busy over 3 s.  Another agent ran protoJS test262 suites
  on the machine during the session; runs waited for them.
- **Runs.**  Interleaved run by run, medians of 3 unless stated; every run
  verifies its own output (checksums, `verified=yes`, `VERIFIED`), and a
  run that does not is never a data point.  Exploratory matrices (A1, A2)
  are single runs or medians of 3 as stated beside them.
- Data: [data/2026-10-04-collector-adjustments/](data/2026-10-04-collector-adjustments/).

## A1. The multi-cursor sweep beside a single mutator

**Regression** (re-measurement): protoClojure `coll_alloc` with one task,
2 M-cell limit, 11.4 -> 14.4 s (+26 %) from 2.10.2 to 2.14.1, and back to
11.0-11.2 s with one chain (`PROTOCORE_GC_SWEEP_CURSORS=1`).  This session:
11.25 -> 14.52 s.

**Cause.**  2.14.1, eight chains against one, the process's threads
separated (`perf record`, one run each, wall under `perf`):

| `clj_coll_t1` | 8 chains | 1 chain |
|---|---|---|
| Wall s | 15.8 | 12.6 |
| Mutator thread: task-clock s | 15.4 | 12.2 |
| Mutator: cycles / instructions (G) | 47.1 / 46.7 | 35.8 / 41.5 |
| Mutator: demand fills from DRAM (M) | 73.8 | 40.8 |
| Mutator: fills from another cache of the complex (M) | 30.8 | 33.0 |
| Collector thread: task-clock s | 3.8 | 8.4 |

- **The mutator got slower, not the collector**: 11 G more cycles and 33 M
  more fills from DRAM, while the collector spent half the CPU time.
- **Where.**  AMD IBS op sampling (precise addresses): 112,081 against
  90,608 samples on the mutator thread, two thirds of the difference in
  the code that initialises newly allocated cells:
  `ProtoListImplementation`'s constructor +7.0 k, `ProtoContext::allocCell`
  +3.6 k, `Cell::Cell` +2.5 k, `ProtoMapImplementation`'s constructor
  +1.4 k.  The mutator writes cells the sweep freed; with eight chains they
  had gone cold, or sat modified in the collector's cache, before reuse.
- **Not a scheduling artefact.**  The process pinned to two CPUs
  (`taskset`, one run each), eight against one chain: same core (SMT
  siblings) 16.35 against 12.82 s (+28 %), same core complex 13.74 against
  10.91 (+26 %), the other complex 18.34 against 11.89 (+54 %), unpinned
  13.82 against 11.30 (+22 %).  The other complex is worst: a line modified
  in the collector's cache then crosses the complexes.
- **Why the speed of the walk matters.**  One chain frees about 15 M cells
  per second here (66 ns per swept cell), about what the mutator takes
  (12 M per second); the mutator takes the chunk published last first, so
  it reuses cells the sweep has just touched.  Eight chains (32 ns per
  cell) finish far ahead and leave a backlog.  The prefetch is not the
  cause: eight chains without prefetch were as slow (the re-measurement's
  diagnostic, 13.5 s).
- Not established: the mutator also retired 5.2 G more instructions with
  eight chains (counted exactly; sampling put them at the stall in
  `implAllocCell`, where they cannot be).  Their source was not found.

**Change** (`sweep::cursorsFor`, `sweep::mutatorsShort`).  The sweep
walks all its chains while

- a thread waits for heap headroom; or
- more than one thread of the space allocated since its last cycle end
  (`getFreeCells` notes the thread of each refill; the collector reads it
  at sweep start); or
- the cells left before the ceiling (the freelist plus the room below the
  soft limit, or the hard limit without one) are below pacing's runway or
  two free chunks per running thread; or
- `PROTOCORE_GC_SWEEP_ENGAGE=always`;

and one chain otherwise; the width is re-read at every segment a cursor
ends.  A cycle whose sweep walked one chain for most of its segments
measured the mutator's pace, not the collector's capacity, so the adaptive
controller keeps the throughput T of the last wide sweep; the helpers'
measured engagement compares against a wide sweep without helpers.

**Rules measured and rejected** (they are the reason for each condition):

| Rule (all with "wide while a thread waits") | What went wrong |
|---|---|
| One chain otherwise | protoJS `records` / `doctree` with six threads 4-16 % slower: the one-chain walk takes about twice the CPU per cell, which six mutators on six cores pay for |
| Wide when the running threads leave no physical core | `records` with six threads still 9.6 % slower (its threads are not all running at every moment), and protoClojure with one task registers several running threads, so its single mutator was not found |
| One chain for a single allocating thread, wide once fewer than two chunks are left | under the controller the single mutator waited in steady state (`AdaptiveHeapSteady`: S 2.1 M -> 3.5-16 M cells): a walk paced to the mutator ends with its cells taken, so the next cycle starts with no runway; and its slower sweep lowered T, pushing rho towards 1 |

**Result** (2.14.2 `2618fc0f` against 2.14.1, interleaved, medians of 3):
`clj_coll_t1` 14.90 -> 11.82 s (-20.7 %), 87 % of its segments walked as
one chain; every multi-threaded row walked wide throughout.  The largest
increases, `scala_tree_t6` +4.8 % (runs 54.9-59.2 against 58.1-60.4 s)
and `js40_records_n12` +3.6 %, are within the spread: a separate A/B of
`js40_records_n12` (three rounds) gave 2.14.1 21.1 / 24.1 / 24.6 s and the
branch 22.9 / 21.5 / 20.5 s.  `clj_coll_t1` remains 5-7 % slower than on
2.10.2 (A2).  The detail columns: `a` is 2.14.2.

| Workload | Wall s | Peak RSS MB | Cycles | Wait share | Sweep ns/cell | Collector CPU s | Process CPU s | runs ok (b/a) |
|---|---|---|---|---|---|---|---|---|
| core_fixed640_live1M | 2.67 -> 2.64 (-1.1 %) | 664 -> 672 (+1.2 %) | 12 -> 12 | 5.0 -> 4.6 % | 9.6 -> 10.7 (+10.8 %) | 1.9 -> 1.9 (+2.5 %) | 4.6 -> 4.6 (-0.4 %) | 3/3 |
| core_adaptive_live1M | 2.39 -> 2.38 (-0.4 %) | 923 -> 923 (+0.0 %) | 14 -> 14 | 3.8 -> 4.0 % | 10.0 -> 11.2 (+12.4 %) | 1.9 -> 1.9 (-0.1 %) | 4.0 -> 4.1 (+1.0 %) | 3/3 |
| js40_records_n6 | 12.51 -> 11.96 (-4.4 %) | 2547 -> 2544 (-0.1 %) | 9 -> 8 | 2.9 -> 3.2 % | 19.5 -> 18.7 (-4.3 %) | 8.4 -> 7.6 (-10.2 %) | 62.1 -> 61.2 (-1.4 %) | 3/3 |
| js40_records_n12 | 21.57 -> 22.34 (+3.6 %) | 2594 -> 2607 (+0.5 %) | 14 -> 14 | 6.5 -> 5.1 % | 20.2 -> 21.1 (+4.1 %) | 16.8 -> 17.5 (+4.1 %) | 162.2 -> 165.8 (+2.2 %) | 3/3 |
| js40_join_n6 | 11.62 -> 11.42 (-1.7 %) | 2691 -> 2691 (+0.0 %) | 6 -> 6 | 3.8 -> 4.5 % | 20.7 -> 19.6 (-5.4 %) | 6.9 -> 6.8 (-0.9 %) | 59.0 -> 59.9 (+1.5 %) | 3/3 |
| js40_doctree_n6 | 9.91 -> 9.85 (-0.6 %) | 2579 -> 2582 (+0.1 %) | 4 -> 4 | 4.3 -> 4.3 % | 15.1 -> 16.1 (+7.0 %) | 5.0 -> 5.1 (+3.4 %) | 48.3 -> 47.7 (-1.2 %) | 3/3 |
| js40_doctree_n12 | 21.52 -> 20.18 (-6.2 %) | 2657 -> 2654 (-0.1 %) | 10 -> 9 | 14.0 -> 15.8 % | 17.5 -> 16.6 (-4.8 %) | 17.2 -> 15.4 (-10.4 %) | 144.8 -> 143.0 (-1.3 %) | 3/3 |
| js40_wordfreq_n6 | 7.56 -> 7.36 (-2.6 %) | 2605 -> 2600 (-0.2 %) | 4 -> 4 | 3.0 -> 2.9 % | 14.9 -> 15.3 (+2.7 %) | 3.4 -> 3.0 (-11.2 %) | 35.4 -> 35.5 (+0.3 %) | 3/3 |
| js40_graph_n6 | 16.02 -> 15.86 (-1.0 %) | 2736 -> 2626 (-4.0 %) | 13 -> 13 | 5.9 -> 2.8 % | 18.1 -> 18.7 (+3.4 %) | 11.7 -> 11.1 (-5.2 %) | 85.7 -> 85.0 (-0.8 %) | 3/3 |
| js40_graph_n12 | 42.67 -> 43.38 (+1.7 %) | 2776 -> 2820 (+1.6 %) | 25 -> 25 | 28.8 -> 29.4 % | 16.5 -> 17.0 (+3.2 %) | 36.9 -> 37.1 (+0.3 %) | 259.5 -> 263.5 (+1.5 %) | 3/3 |
| js10_records_n12 | 10.50 -> 10.56 (+0.6 %) | 724 -> 720 (-0.5 %) | 21 -> 21 | 18.9 -> 19.4 % | 12.6 -> 12.4 (-1.3 %) | 8.1 -> 7.8 (-3.5 %) | 62.3 -> 61.5 (-1.2 %) | 3/3 |
| js10_doctree_n12 | 9.64 -> 9.25 (-4.0 %) | 731 -> 738 (+1.0 %) | 14 -> 13 | 32.8 -> 33.6 % | 13.0 -> 11.8 (-9.5 %) | 7.9 -> 7.1 (-9.6 %) | 50.3 -> 50.6 (+0.6 %) | 3/3 |
| js10_graph_n6 | 8.45 -> 8.17 (-3.3 %) | 778 -> 732 (-5.8 %) | 17 -> 17 | 26.6 -> 27.8 % | 20.6 -> 22.4 (+8.7 %) | 6.8 -> 6.8 (-0.6 %) | 33.9 -> 33.0 (-2.7 %) | 3/3 |
| jsad_records_n12 | 20.97 -> 21.49 (+2.5 %) | 2571 -> 2579 (+0.3 %) | 24 -> 21 | 7.0 -> 7.0 % | 21.3 -> 22.2 (+4.1 %) | 18.1 -> 18.7 (+3.5 %) | 159.7 -> 162.4 (+1.7 %) | 3/3 |
| jsad_wordfreq_n12 | 13.19 -> 12.36 (-6.3 %) | 2748 -> 2609 (-5.1 %) | 18 -> 18 | 6.6 -> 7.0 % | 19.6 -> 19.8 (+1.2 %) | 10.0 -> 8.9 (-10.4 %) | 96.4 -> 92.2 (-4.4 %) | 3/3 |
| scala_tree_t1 | 13.34 -> 13.15 (-1.4 %) | 159 -> 158 (-0.7 %) | 24 -> 24 | 0.0 -> 0.0 % | 46.8 -> 48.3 (+3.0 %) | 2.0 -> 2.0 (+1.5 %) | 15.4 -> 15.2 (-1.0 %) | 3/3 |
| scala_tree_t6 | 56.55 -> 59.24 (+4.8 %) | 192 -> 182 (-5.0 %) | 163 -> 182 | 35.0 -> 36.3 % | 120.4 -> 124.9 (+3.7 %) | 50.0 -> 45.5 (-8.9 %) | 254.3 -> 265.7 (+4.5 %) | 3/3 |
| clj_coll_t1 | 14.90 -> 11.82 (-20.7 %) | 136 -> 135 (-0.1 %) | 92 -> 131 | 0.0 -> 0.0 % | 33.8 -> 67.9 (+100.8 %) | 4.5 -> 9.0 (+100.4 %) | 19.4 -> 20.7 (+6.8 %) | 3/3 |
| clj_coll_t6 | 44.47 -> 44.81 (+0.8 %) | 152 -> 157 (+3.7 %) | 459 -> 450 | 10.6 -> 10.6 % | 44.3 -> 44.1 (-0.4 %) | 30.0 -> 29.4 (-2.0 %) | 286.4 -> 289.5 (+1.1 %) | 3/3 |


## A2. Pacing when the collector already keeps up

**Regression** (re-measurement): six threads at 40 M cells, where the
collector kept up, +1 to +6 % (`js40_doctree_n6` +5.9 %), attributed to
pacing's early starts doubling the cycles (2 -> 4).

**In this session it does not reproduce** (medians of 3, the same binaries
as the re-measurement's sides):

| Workload | 2.10.2 s / cycles | 2.14.1 s / cycles | 2.14.2 (`d5fca7b0`) s / cycles |
|---|---|---|---|
| js40_doctree_n6 | 9.85 / 2 | 9.69 / 4 | 9.10 / 4 |
| js40_wordfreq_n6 | 7.60 / 2 | 7.29 / 4 | 7.49 / 4 |
| js40_records_n6 | 15.36 / 5 | 11.63 / 8 | 11.89 / 9 |
| clj_coll_t1 | 11.25 / 66 | 14.52 / 92 | 11.99 / 130 |

The +5.9 % of the re-measurement (`doctree`, 9.50 -> 10.06 s) is not seen
here (9.85 -> 9.69 s); the cycles do double (2 -> 4).

**And the early starts pay for themselves there.**  One library (the
branch at the time, 1-chain rule then in force for one task), three pacing
variants, medians of 3:

| Workload | 2.14.1's runway `r C 1.25` | No early start (runway 0, early wake kept) | Runway until the first cells are back, `r (C_b + 0.25 C)` |
|---|---|---|---|
| protoJS `doctree` (6), 40 M | 9.50 s, 4 cycles | 9.67 s, 2 | 10.72 s, 3 |
| protoJS `wordfreq` (6), 40 M | 7.45 s, 4 | 7.82 s, 2 | 8.39 s, 3 |
| protoJS `records` (6), 40 M | 12.91 s, 9 | 14.42 s, 5 | 13.08 s, 7 |
| protoClojure `coll_alloc` (6), 2 M | 39.41 s, 446 | 43.08 s, 362 | 39.36 s, 443 |
| protoClojure `coll_alloc` (1), 2 M | 11.08 s, 133 | 8.74 s, 66 | 9.01 s, 81 |

- With six threads, no early start was 2-12 % slower: the extra cycles
  cost less than the waits they remove.
- The shorter runway -- the time before the sweep plus the sweep's deficit
  when it frees more slowly than the mutators allocate (`C_b`), plus a
  quarter of the cycle -- ran 25-40 % fewer cycles but was up to 13 %
  slower from the waits it let back in, and the steady-state test
  `AdaptiveHeapRate.SteadyAllocatorBelowCapacityStopsWaiting` waited again
  on a CI runner (run 37196232550).  It was implemented with its tests,
  measured, and removed (commit `3ca0ba32`).
- **With a single mutator the early starts do cost**: `coll_alloc` with one
  task ran 133 cycles and 11.08 s against 66 and 8.74 s with no early
  start.  That is the part of A1's regression that remains (11.99 against
  2.10.2's 11.25 s).  No rule was adopted for it: the variant that helps it
  is the one that let waits back in elsewhere.

**Change: none.**  The runway is 2.14.1's.

## A3. The controller's start for protoST

**Cause** (the re-measurement's trace, reproduced): `fib.st`'s live set
after start-up is 1.68 M cells, S0 2 M.  The first cycle starts at once and
reclaims 24 k cells; the law skips the first cycle, so S stays at S0, and
pacing's runway (r C 1.25, 1.9 M cells) exceeds the 0.1 M of headroom: the
next cycle starts at once and reclaims 3 cells.  Then a probe doubles S,
and a third cycle, started near the end, is waited for at exit.

**Change** (`adaptive::liveSetFloor`).  After a cycle that reclaimed fewer
cells than its live set -- the first included, which the law otherwise
skips -- S >= min(H, 2 L + runway), raised by at least a doubling: a cycle
paced to start `runway` cells before S then finds at least L of garbage.
Only after such a cycle: a first version that applied the floor whenever
`S < 2 L + runway` raised the memory of the core benchmark with a 100 k
element live set under the controller from 141 to 328 MB and made it 10 %
slower, although its cycles reclaimed plenty.  The doubling bounds the
changes of S (the runway is measured and noisy: a floor that crept with it
changed S at every cycle).

**Result** (calibration rows, the release library, H = 75 % of a 10 GB
cap, interleaved, medians of 3; "Default" is each runtime's own policy,
"Controller" `PROTOCORE_ADAPTIVE_HEAP=1`):

| Workload | Default ref: s / MB / cycles | Default adj | adj vs ref (time, memory) | Controller ref | Controller adj | adj vs ref (time, memory) | runs ok |
|---|---|---|---|---|---|---|---|
| core/ahb-0k | 1.73 / 645 / 6 | 1.67 / 645 / 7 | -3.5 %, -0.0 % | 1.15 / 132 / 36 | 1.11 / 132 / 47 | -3.5 %, -0.2 % | 3/3/3/3 |
| core/ahb-100k | 1.78 / 647 / 6 | 1.76 / 647 / 8 | -1.1 %, -0.0 % | 1.21 / 141 / 45 | 1.20 / 141 / 37 | -0.8 %, +0.0 % | 3/3/3/3 |
| core/ahb-1000k | 2.63 / 664 / 12 | 2.64 / 672 / 12 | +0.4 %, +1.2 % | 2.34 / 891 / 14 | 2.42 / 923 / 14 | +3.4 %, +3.6 % | 3/3/3/3 |
| core/ahb-5000k | 16.29 / 743 / 33 | 15.84 / 743 / 31 | -2.8 %, -0.0 % | 6.59 / 3234 / 16 | 6.94 / 3211 / 16 | +5.3 %, -0.7 % | 3/3/3/3 |
| st/saturation_big | 1.54 / 667 / 6 | 1.58 / 663 / 6 | +2.6 %, -0.6 % | 1.78 / 183 / 47 | 1.42 / 190 / 37 | -20.2 %, +4.3 % | 3/3/3/3 |
| st/parallel_speedup | - | - | -, - | 0.37 / 186 / 7 | 0.38 / 185 / 6 | +2.7 %, -0.1 % | 0/0/3/3 |
| st/list_append | - | - | -, - | 0.05 / 48 / 0 | 0.05 / 48 / 0 | +0.0 %, -0.3 % | 0/0/3/3 |
| st/str_concat | - | - | -, - | 0.03 / 32 / 0 | 0.03 / 32 / 0 | +0.0 %, -0.6 % | 0/0/3/3 |
| st/fib | 0.43 / 289 / 0 | 0.42 / 289 / 0 | -2.3 %, -0.0 % | 1.56 / 569 / 3 | 1.29 / 576 / 2 | -17.3 %, +1.4 % | 3/3/3/3 |
| st/message_throughput | - | - | -, - | 0.09 / 80 / 0 | 0.09 / 80 / 0 | +0.0 %, -0.1 % | 0/0/3/3 |
| py/list_append_loop | - | - | -, - | 0.43 / 147 / 5 | 0.38 / 147 / 4 | -11.6 %, +0.1 % | 0/0/3/3 |
| py/binary_trees | - | - | -, - | 3.71 / 340 / 17 | 3.58 / 553 / 11 | -3.5 %, +62.5 % | 0/0/3/3 |
| py/sieve | - | - | -, - | 1.03 / 261 / 10 | 1.02 / 261 / 10 | -1.0 %, +0.0 % | 0/0/3/3 |
| scala/list_ops | - | - | -, - | 0.53 / 205 / 3 | 0.56 / 237 / 2 | +5.7 %, +15.7 % | 0/0/3/3 |
| scala/map_build | - | - | -, - | 0.32 / 150 / 1 | 0.32 / 150 / 1 | +0.0 %, -0.0 % | 0/0/3/3 |
| scala/object_tree | - | - | -, - | 0.40 / 105 / 0 | 0.40 / 105 / 0 | +0.0 %, -0.1 % | 0/0/3/3 |
| js/probe-100k | - | - | -, - | 0.87 / 233 / 4 | 0.87 / 233 / 5 | +0.0 %, -0.1 % | 0/0/3/3 |

138 runs, 0 failed verification

- `st/fib` under the controller: 1.56 -> 1.29 s (-17 %), 3 -> 2 cycles.
  Still 3 times protoST's fixed limit (0.42 s): the start-up cycle
  (about 0.3 s: the live set is the start-up's) and a cycle started near
  the end and waited for at exit remain.  The recommendation for protoST
  stays "not yet" (or `initialSoftCells` above its start-up live set).
- Where cycles reclaimed less than their live set the floor bought memory:
  `py/binary_trees` 340 -> 553 MB (-3.5 % time), `scala/list_ops` 205 ->
  237 MB (+5.7 % time), core benchmark with a 5 M live set +5.3 % time.
  `st/saturation_big` -20 %, `py/list_append_loop` -12 %.
- Default policies: within -3.5 to +2.6 %.

## A4. `immutable_sharing_benchmark`: +8.5 % cycles single-threaded

2.10.2 and 2.14.1 rebuilt from their commits (`b7f6d82a`, `1365c3af`; both
libraries byte-identical to the packaged ones), each also with every
function aligned to 64 bytes (`-falign-functions=64`), and the branch;
`perf stat -r 5`, three rounds interleaved, medians:

| Counter | 2.10.2 | 2.14.1 | 2.10.2, aligned | 2.14.1, aligned | 2.14.2 |
|---|---|---|---|---|---|
| instructions:u (G) | 4.574 | 4.574 | 4.574 | 4.574 | 4.574 |
| cycles:u (G) | 3.023 | 3.165 (+4.7 %) | 3.024 | 2.996 (-0.9 %) | 3.160 |
| task-clock ms | 1230 | 1259 | 1226 | 1214 | 1248 |
| op-cache <-> decoder mode switches (M) | 0.50 | 20.65 | 1.71 | 2.80 | 18.24 |
| 32-byte fetch windows decoded (G) | 0.42 | 1.06 | 0.45 | 0.48 | 0.99 |
| fetch stalls on back-pressure (G) | 0.94 | 1.28 | 0.94 | 1.06 | 1.30 |
| L1 data misses (M) | 41.5 | 41.4 | 40.7 | 40.5 | 41.5 |
| DRAM fills (M) | 7.2 | 7.6 | 7.2 | 7.1 | 7.2 |
| branch misses (M) | 28.1 | 28.5 | 28.1 | 29.6 | 28.4 |

- **It is code layout.**  The same instructions retire; the hot functions
  have the same sizes in both libraries (`implGetAt`, the list iterator,
  `allocCell`, `implAllocCell`, `Cell::Cell` ...) and moved relative to
  64-byte boundaries.  2.14.1 switches between the op cache and the legacy
  decoder 41 times as often and decodes 2.5 times as many 32-byte fetch
  windows; the data-side counters are flat.  Aligning every function to
  64 bytes removes the difference.
- IBS (precise addresses), 2.10.2 against 2.14.1: +10.3 % samples, in
  `ProtoListImplementation::implGetAt` (+1,158), the list iterator's
  constructor (+864), the benchmark's loop (+434), `allocCell` (+252).
- This session measured +4.7 % (the re-measurement +8.5 %); 2.14.2 keeps
  2.14.1's layout for these functions (+4.5 %).
- **Not fixed in code.**  `-falign-functions=64` makes the library 3.6 %
  larger and was measured on this benchmark only; it is an option for
  builders (`CMAKE_CXX_FLAGS`), not a default changed here.

## Hardware class

| Change | Class | Expected elsewhere (not measured) |
|---|---|---|
| A1 width rule | hardware-sensitive in effect (cache sizes, complexes), robust in form: the conditions are counts and the runway, no fitted constant | larger caches shrink the cold-reuse cost; more cores make "several allocators" the common case |
| A3 floor | hardware-robust: counts cells | the same |
| A2, A4 | no change | A4's layout effect is specific to this core's op cache; another layout or CPU may show none or the opposite |

## What this does not show

- Server-class hardware; heaps aged for hours; real applications.
- Under the controller only the calibration rows listed above were re-run;
  protoJS's sequential structure benchmarks, the no-limit policy rows and
  the CAD model were not (another agent's test262 runs kept the machine
  busy for hours).
- The extra 5.2 G instructions of A1's mutator with eight chains.
