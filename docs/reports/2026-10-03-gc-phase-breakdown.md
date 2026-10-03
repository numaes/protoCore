# Collector time, phase by phase, on runtime workloads

Date: 2026-10-03.  protoCore 2.10.2 (master `b7f6d82a`) plus the
instrumentation of branch `measure/gc-phases` (`f4e05e57`, and the
collector-CPU fields of the following commit).  Measurement only: no
behaviour was changed.  Author: Gustavo Marino, with Claude.

## Verdict

- **The pause is not the cost.**  The stop-the-world window (every mutator
  parked until the resume) totalled 0.0-14.7 ms per run, at most 0.23 ms
  per cycle, at most 0.025 % of the collector's busy time on every
  workload.  The quorum wait (raising the flag until the last mutator
  parks) is larger but still minor: at most 21 ms per protoJS run with a
  fixed limit, 43 ms (protoClojure) and 173 ms (protoScala) with 6 tasks,
  28-270 ms under the adaptive controller at N = 12, against 20-61 s of
  collector time.
- **The sweep dominates.**  It is 41-99 % of the collector's busy time on
  the 32 runs that collected and completed (median 61 %), and the largest
  phase on 30 of them; mark is 1-50 %, Phase 5b (mutables-tree release)
  and the bulk unmark together 0-14 %.  Mark leads only when most of the
  heap is live: protoJS `wordfreq` N = 12 and `graph` N = 1 at 10 M cells
  (50 % and 44 %), and the runs that ended in out-of-memory (`join` at
  10 M cells, 66-76 % mark; `graph` N = 12 at 10 M, 89 %).
- **With many allocating threads the collector is the bottleneck.**  At
  N = 12 Deferreds it is busy 56-95 % of the wall time, nearly back to back,
  and the mutators spend 17-63 % of their thread time waiting for heap
  headroom.  protoScala and protoClojure with 6 tasks: 92-94 % busy, 45-66 %
  waiting.
- **The sweep's throughput collapses under concurrent allocation.**  Per
  cell, single-threaded: 17-25 ns (40-60 M cells/s); with 6-12 allocating
  threads: 58-158 ns (6-17 M cells/s), the same cell mix (protoClojure
  t1 against t6: 45 against 143 ns per cell).  The collector thread was on
  the CPU 99-100 % of the sweep's wall time in every case, so it was not
  starved of CPU: each cell costs more.  The likely cause, not proven here,
  is cache-coherence traffic (the swept cells were last written on other
  cores; the sweep reads, finalizes and relinks each one).  Mark throughput
  degrades much less (11-22 M cells/s single-threaded, 7-16 M with threads).
- **Amdahl.**  At N = 12, removing every mutator wait would cut wall time
  by at most 17-50 % (protoJS, 40 M cells), 42-56 % (10 M cells), 40-63 %
  (adaptive controller); with 6 tasks, 45 % (protoScala) and 66 %
  (protoClojure).  On every completed workload a faster sweep alone, with
  mark unchanged, reaches that bound: 1.2-3.2 x with fixed limits, 2.0-3.9 x
  under the controller (5.9 x for its `graph`), 1.9 x protoScala, 2.9 x
  protoClojure.  A parallel mark is needed only near a full heap.  Whether a parallel
  sweep scales is open: the per-cell cost that limits it is a memory-system
  cost, which more sweeping threads share.
- **Part of the wait is timing, not throughput.**  At N = 6 with a 40 M-cell
  limit the collector is busy less than the mutators' own work (no sweep
  speed-up needed), yet the mutators wait 10-22 % of their time: a fixed
  limit requests the cycle only at the ceiling, so the mutators stop for the
  whole cycle.  Pacing (the controller's early request) addresses this; a
  faster sweep does not.
- **The adaptive controller at N = 12** (hard limit 40 M cells, the same
  ceiling as the fixed runs) was slower than the fixed limit on all five
  workloads (+2 % to +91 %; `wordfreq` 15.2 -> 29.1 s), with 1.2-4.8 x the
  cycles and 9-516 % more mutator waiting: its soft limit runs more cycles of a
  collector that is already saturated.  Peak RSS was 2-21 % lower on four
  workloads and 2 % higher on `graph`.

## Method

**Library.**  protoCore master built in Release with
`-DPROTOCORE_GC_INSTRUMENT=ON` in a separate build directory of a worktree
of `measure/gc-phases`.  `PROTOCORE_GC_PROFILE=1` prints, per cycle, the
existing cumulative `[GC-PROFILE]` line and a new cumulative `[GC-PHASES]`
line:

| Field | Meaning |
|---|---|
| `busy` | the whole cycle on the collector thread, minus the wait for the process-wide cycle token |
| `token`, `quorum` | P1 split: waiting for the cycle token; raising `stwFlag` until every mutator is parked |
| `stw`, `stw_max` | the pause itself: every mutator parked until the world resumes (sum and maximum per cycle) |
| `young`, `trace` | P4 split: young-chain and survivor-pen walk; the transitive mark loop.  "Mark" below is their sum |
| `sweep`, `swept_cells`, `freed_cells` | P5: time, cells examined (candidates, survivors included), cells returned to the freelist |
| `rel`, `unmark` | Phase 5b; the bulk unmark alone (P6 also holds Phase 7 and deferred frees) |
| `headroom_wait` | mutator time in `reclaimWaitLocked` (every soft-zone, checkpoint and hard-zone wait for a cycle), summed over threads |
| `mut_park` | mutator time parked for a stop-the-world in `multispace::parkIn`, summed over threads |
| `cpu_busy`, `cpu_mark`, `cpu_sweep` | collector-thread CPU time (`CLOCK_THREAD_CPUTIME_ID`) |

All of it is compiled out by default (`nm` of a default build shows no
`gcprof` symbol).  The branch is pushed and not merged.

**Binaries.**  The installed Release runtimes (`/usr/bin/protojs`,
`protoscala`, `protoclj`, all linked against SOVERSION 3), copied to a
scratch directory and run with `LD_LIBRARY_PATH` set to the instrumented
library.  `ldd` resolved `libprotoCore.so.3` to it for all three, and every
run that collected printed `[GC-PHASES]` lines, which only that library
emits.  The seven runs without a line ran no cycle.

**Runs.**  Sequential, each in `systemd-run --user --scope -p MemoryMax=<cap>
-p MemorySwapMax=0` under `/usr/bin/time` (wall time, peak RSS).  One run
per configuration.  Machine: AMD, 6 cores / 12 threads, 62 GB, Linux
x86-64, shared with other jobs.  Each workload verifies itself:

- **protoJS structures** (`benchmarks/structures/protojs_bench.js`,
  `MODE=par`, N Deferreds, scale 1): `ok` from the script (repetitions
  agree), and task k's checksum equal across every N and heap setting.
  - *js40*: `PROTOCORE_HEAP_LIMIT_CELLS=40000000`, REPS = 3, 6 GB cap: the
    configuration of protoJS's structure report.  At N = 1 four of five
    workloads run no cycle at all.
  - *js10*: 10,000,000 cells, REPS = 1, 4 GB cap: the collection-heavy end
    of that report's sweep.
  - *jsad*: N = 12, `PROTOCORE_ADAPTIVE_HEAP=1` with H = 40 M cells, REPS = 3.
- **protoJS CAD** (`cad/protojs_cad.js`): 20,000 parts (a tenth of the
  report's model; 7.7 M live cells after the build), `NS=1,12`, REPS = 1,
  16 M-cell limit, 3 GB cap; `ok` and per-task checksums.
- **protoScala** `tree_alloc.scala` (new, in the scratch set, not committed):
  batches of T Futures, each building five depth-12 case-class trees,
  path-copying their spine and folding both; every Future must return
  540451801.  100 batches, 2 M-cell limit (protoScala sets none).
- **protoClojure** `coll_alloc.clj` (new): per round, a 2,000-entry map
  built with `assoc` and a 2,000-element list built with `cons`, folded;
  1,500 rounds per task, tasks through `pmap`; the checksum is checked
  against the closed form.  2 M-cell limit (protoClojure sets none).
- **protoCore** `adaptive_heap_benchmark` with a 1 M live list, under the
  640 MB fixed limit and under the controller.

A first idiomatic protoScala version ran all rounds in one loop per Future;
it went out of memory at 2 M cells with the live set equal to the heap.
protoScala keeps a function's garbage reachable until the function returns,
so the rounds were split into short Futures.  That retention is a protoScala
property worth its own look; it is not measured here.

**Derived quantities.**
- Mark and sweep throughput: cells handled per second of phase wall time.
  Mark counts marked cells plus young cells walked.
- Wait share: `headroom_wait / (wall x threads)`.
- Mutator floor: `wall - headroom_wait / threads`, the wall time if no
  thread had waited, assuming the waits were spread evenly over the
  threads and the collector overlaps the mutators entirely.  Under the
  same assumption the wall time with a phase k times faster is
  `max(floor, busy - phase x (1 - 1/k))`: a collector that keeps up with
  the mutators stops costing them time, and a collector slower than they
  are paces them.  This is an upper bound on the gain: it ignores the CPU
  that a parallel collector would take from the mutators (12 Deferreds
  already use all 12 hardware threads) and the throughput loss shown
  below.

## Per-workload breakdown

`Mark` = young + trace.  `Headroom wait` is summed over the mutator threads
(the number of waits in brackets).  `STW park` is the mutators' time parked
for pauses, summed over threads.  `M` = millions of cells; `Mc/s` = millions
of cells per second.

| Workload | ok | Wall s | Cycles | GC busy s (% wall) | STW total / max ms | Quorum ms | Mark s (young+trace) | Sweep s | 5b ms | Unmark ms | Headroom wait s (sum, waits) | STW park s | Marked M | Swept M | Freed M | Mark Mc/s | Sweep Mc/s |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| core_fixed640_live1M | yes | 3.6 | 7 | 2.00 (56%) | 0.2 / 0.06 | 0.0 | 0.51 | 1.39 | 0.0 | 102 | 1.25 (25) | 0.000 | 6.7 | 72.5 | 66.1 | 14 | 52 |
| core_adaptive_live1M | yes | 4.4 | 15 | 3.17 (72%) | 0.4 / 0.09 | 50.6 | 1.01 | 1.94 | 0.0 | 166 | 2.43 (52) | 0.001 | 12.4 | 78.2 | 67.2 | 14 | 40 |
| js40_records_n1 | yes | 7.3 | 0 | - | - | - | - | - | - | - | - | - | - | - | - | - | - |
| js40_records_n6 | yes | 16.3 | 5 | 10.75 (66%) | 0.2 / 0.04 | 0.2 | 2.46 | 7.28 | 478.5 | 532 | 21.49 (428) | 0.000 | 24.9 | 199.1 | 174.4 | 10 | 27 |
| js40_records_n12 | yes | 41.6 | 10 | 36.99 (89%) | 0.5 / 0.07 | 0.9 | 7.46 | 26.97 | 923.5 | 1641 | 231.27 (4603) | 0.000 | 68.6 | 402.7 | 335.6 | 9 | 15 |
| js40_join_n1 | yes | 6.6 | 0 | - | - | - | - | - | - | - | - | - | - | - | - | - | - |
| js40_join_n6 | yes | 15.1 | 3 | 5.53 (37%) | 0.1 / 0.04 | 0.3 | 1.59 | 3.58 | 56.7 | 291 | 13.24 (264) | 0.000 | 14.4 | 109.6 | 96.5 | 15 | 31 |
| js40_join_n12 | yes | 39.1 | 8 | 29.91 (76%) | 0.4 / 0.08 | 10.5 | 10.02 | 18.27 | 236.6 | 1361 | 178.10 (3547) | 0.001 | 58.3 | 266.1 | 216.3 | 11 | 15 |
| js40_doctree_n1 | yes | 5.6 | 0 | - | - | - | - | - | - | - | - | - | - | - | - | - | - |
| js40_doctree_n6 | yes | 11.6 | 2 | 3.94 (34%) | 0.1 / 0.06 | 0.1 | 1.09 | 2.30 | 275.7 | 277 | 8.42 (168) | 0.000 | 12.0 | 79.3 | 67.4 | 11 | 35 |
| js40_doctree_n12 | yes | 28.6 | 6 | 21.68 (76%) | 0.5 / 0.15 | 0.4 | 5.97 | 13.36 | 787.4 | 1555 | 120.61 (2403) | 0.000 | 60.3 | 239.9 | 179.9 | 10 | 18 |
| js40_wordfreq_n1 | yes | 5.2 | 0 | - | - | - | - | - | - | - | - | - | - | - | - | - | - |
| js40_wordfreq_n6 | yes | 8.2 | 2 | 2.13 (26%) | 0.1 / 0.05 | 0.3 | 0.63 | 1.29 | 79.0 | 132 | 4.81 (96) | 0.000 | 7.0 | 79.4 | 72.4 | 11 | 62 |
| js40_wordfreq_n12 | yes | 15.2 | 5 | 8.52 (56%) | 0.3 / 0.08 | 1.9 | 1.65 | 6.02 | 303.3 | 536 | 31.32 (624) | 0.000 | 19.4 | 199.0 | 180.0 | 12 | 33 |
| js40_graph_n1 | yes | 9.3 | 1 | 0.69 (7%) | 0.0 / 0.03 | 0.0 | 0.22 | 0.40 | 38.0 | 40 | 0.25 (5) | 0.000 | 2.5 | 39.8 | 37.4 | 11 | 101 |
| js40_graph_n6 | yes | 19.3 | 7 | 11.79 (61%) | 0.2 / 0.06 | 0.5 | 3.19 | 7.60 | 346.3 | 645 | 23.01 (459) | 0.000 | 33.7 | 259.1 | 241.7 | 16 | 34 |
| js40_graph_n12 | yes | 61.2 | 17 | 56.26 (92%) | 0.8 / 0.09 | 2.0 | 15.47 | 37.26 | 939.2 | 2586 | 363.95 (7233) | 0.000 | 137.6 | 579.6 | 525.2 | 15 | 16 |
| js10_records_n1 | yes | 4.1 | 1 | 0.32 (8%) | 0.0 / 0.04 | 0.0 | 0.12 | 0.17 | 2.8 | 23 | 0.15 (3) | 0.000 | 1.7 | 9.9 | 8.2 | 14 | 58 |
| js10_records_n6 | yes | 8.2 | 7 | 5.93 (72%) | 0.3 / 0.08 | 0.3 | 1.43 | 4.07 | 112.9 | 322 | 18.45 (368) | 0.000 | 16.6 | 69.1 | 52.8 | 12 | 17 |
| js10_records_n12 | yes | 20.1 | 15 | 17.73 (88%) | 0.8 / 0.08 | 3.6 | 3.65 | 12.97 | 297.7 | 814 | 135.16 (2693) | 0.003 | 39.7 | 150.0 | 111.8 | 12 | 12 |
| js10_join_n1 | yes | 2.7 | 0 | - | - | - | - | - | - | - | - | - | - | - | - | - | - |
| js10_join_n6 | **no** | 68.2 | 65 | 64.65 (95%) | 2.2 / 0.08 | 2.5 | 48.84 | 13.13 | 24.3 | 2651 | 389.47 (7761) | 0.001 | 174.8 | 172.6 | 23.9 | 13 | 13 |
| js10_join_n12 | **no** | 260.2 | 188 | 253.88 (98%) | 9.4 / 0.11 | 7.6 | 167.65 | 71.44 | 64.9 | 14700 | 3071.86 (61249) | 0.010 | 931.4 | 819.8 | 39.7 | 12 | 11 |
| js10_doctree_n1 | yes | 2.4 | 0 | - | - | - | - | - | - | - | - | - | - | - | - | - | - |
| js10_doctree_n6 | yes | 5.4 | 4 | 2.88 (54%) | 0.2 / 0.06 | 4.2 | 0.92 | 1.61 | 135.4 | 204 | 7.62 (152) | 0.000 | 9.3 | 38.9 | 29.7 | 10 | 24 |
| js10_doctree_n12 | yes | 14.2 | 10 | 12.18 (86%) | 0.8 / 0.12 | 1.7 | 3.66 | 7.35 | 298.1 | 870 | 80.61 (1607) | 0.000 | 37.4 | 98.8 | 61.8 | 10 | 13 |
| js10_wordfreq_n1 | yes | 2.6 | 0 | - | - | - | - | - | - | - | - | - | - | - | - | - | - |
| js10_wordfreq_n6 | yes | 4.2 | 3 | 1.21 (29%) | 0.1 / 0.04 | 0.7 | 0.42 | 0.70 | 24.9 | 69 | 3.63 (72) | 0.000 | 4.0 | 28.4 | 25.4 | 13 | 41 |
| js10_wordfreq_n12 | yes | 9.2 | 8 | 6.51 (71%) | 0.4 / 0.11 | 20.5 | 3.26 | 2.70 | 64.0 | 464 | 46.48 (924) | 0.009 | 24.4 | 66.2 | 54.0 | 13 | 25 |
| js10_graph_n1 | yes | 3.2 | 1 | 0.23 (7%) | 0.0 / 0.04 | 0.0 | 0.10 | 0.10 | 8.3 | 21 | 0.15 (3) | 0.000 | 1.2 | 9.4 | 8.7 | 17 | 94 |
| js10_graph_n6 | yes | 10.8 | 12 | 8.47 (79%) | 0.4 / 0.10 | 1.3 | 3.20 | 4.64 | 106.9 | 517 | 27.85 (555) | 0.000 | 38.9 | 87.3 | 75.9 | 22 | 19 |
| js10_graph_n12 | **no** | 8.5 | 8 | 7.45 (88%) | 0.4 / 0.07 | 2.9 | 6.60 | 0.17 | 0.2 | 681 | 91.12 (1812) | 0.003 | 71.0 | 4.6 | 1.8 | 22 | 28 |
| jsad_records_n12 | yes | 46.3 | 22 | 35.14 (76%) | 1.7 / 0.16 | 57.1 | 8.70 | 24.27 | 669.9 | 1445 | 350.73 (7094) | 0.780 | 66.5 | 381.7 | 317.8 | 8 | 16 |
| jsad_join_n12 | yes | 39.9 | 17 | 35.54 (89%) | 1.2 / 0.13 | 116.2 | 12.72 | 20.87 | 196.8 | 1627 | 208.14 (4210) | 1.331 | 78.8 | 304.1 | 233.2 | 11 | 15 |
| jsad_doctree_n12 | yes | 34.7 | 14 | 29.34 (85%) | 1.7 / 0.23 | 27.9 | 9.19 | 17.31 | 830.6 | 1976 | 166.59 (3350) | 0.375 | 73.6 | 255.1 | 182.1 | 8 | 15 |
| jsad_wordfreq_n12 | yes | 29.1 | 24 | 20.19 (69%) | 2.3 / 0.19 | 260.5 | 8.15 | 10.75 | 208.3 | 817 | 190.98 (3887) | 2.693 | 43.4 | 203.8 | 176.6 | 8 | 19 |
| jsad_graph_n12 | yes | 63.9 | 21 | 60.74 (95%) | 1.2 / 0.12 | 269.7 | 20.62 | 35.87 | 914.6 | 3056 | 395.60 (7889) | 1.270 | 161.6 | 556.5 | 504.7 | 14 | 16 |
| scala_tree_t1 | yes | 19.0 | 28 | 4.62 (24%) | 0.7 / 0.05 | 0.1 | 0.21 | 4.39 | 0.1 | 28 | 2.10 (42) | 0.000 | 2.0 | 54.1 | 52.3 | 13 | 12 |
| scala_tree_t6 | yes | 89.6 | 199 | 82.24 (92%) | 8.3 / 0.10 | 172.7 | 11.59 | 69.23 | 0.5 | 1240 | 241.93 (4824) | 0.273 | 73.9 | 390.3 | 318.3 | 7 | 6 |
| clj_coll_t1 | yes | 14.5 | 66 | 7.12 (49%) | 1.6 / 0.05 | 0.1 | 0.05 | 7.07 | 0.2 | 5 | 3.22 (67) | 0.000 | 0.4 | 129.8 | 129.5 | 12 | 18 |
| clj_coll_t6 | yes | 129.2 | 374 | 120.92 (94%) | 14.7 / 0.20 | 43.1 | 2.01 | 118.68 | 0.9 | 176 | 514.47 (10253) | 0.065 | 12.1 | 789.8 | 779.1 | 7 | 7 |
| cad_20k | yes | 247.0 | 71 | 219.88 (89%) | 3.0 / 0.15 | 1.9 | 61.45 | 142.72 | 1732.6 | 13968 | 687.47 (13695) | 0.006 | 558.5 | 1135.2 | 581.8 | 9 | 8 |

Runs marked **no** ended in out-of-memory (exit 3, the runtime's message:
the live set fills the 10 M-cell limit and the last collections reclaimed
nothing).  `join` at N = 6 and 12 also failed at 10 M cells in protoJS's
structure report.  `graph` at N = 12 and 10 M cells completed there (63.6 s,
54 cycles, commit `decd79efe`) and fails now, three times out of three
(5.9-9.1 s, 6-9 cycles).  The live set of twelve concurrent searches is
now above 10 M cells; this report does not explain why.

The CAD row covers the whole process: the single-threaded build (25.6 s),
four sequential tasks (61.3 s, the integrity analysis 45.9 s of it), N = 1
(4.2 s) and N = 12 (155.7 s, against a sequential sum of 183.9 s).

## Why the sweep slows down with threads

A subset re-run with the collector-thread CPU time:

| Workload | Wall s | Cycles | Sweep wall s | Sweep CPU s (share) | Sweep Mc/s | ns per swept cell | Mark wall s | Mark CPU s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| core_fixed640_live1M | 3.6 | 7 | 1.38 | 1.37 (100 %) | 53 | 19 | 0.51 | 0.51 |
| js10_records_n1 | 3.3 | 1 | 0.17 | 0.17 (100 %) | 59 | 17 | 0.11 | 0.11 |
| js10_records_n6 | 7.7 | 7 | 3.98 | 3.97 (100 %) | 17 | 58 | 1.26 | 1.26 |
| js10_records_n12 | 19.9 | 15 | 13.10 | 12.95 (99 %) | 12 | 85 | 3.56 | 3.56 |
| scala_tree_t1 | 4.3 | 8 | 0.61 | 0.61 (100 %) | 25 | 40 | 0.04 | 0.04 |
| scala_tree_t6 | 24.2 | 59 | 18.27 | 18.18 (100 %) | 6 | 158 | 3.09 | 3.08 |
| clj_coll_t1 | 12.1 | 66 | 5.90 | 5.89 (100 %) | 22 | 45 | 0.04 | 0.04 |
| clj_coll_t6 | 122.9 | 357 | 112.59 | 112.12 (100 %) | 7 | 143 | 1.75 | 1.75 |

(`scala_tree` ran 30 batches here instead of 100.)

- The collector thread was on the CPU for 99-100 % of every sweep and every
  mark.  It was not descheduled in favour of the mutators: the slowdown is
  in the work per cell.
- The cell mix does not explain it.  protoClojure's t1 and t6 runs execute
  the same rounds and sweep the same 2.0-2.1 M cells per cycle; the cost per
  cell triples (45 -> 143 ns).  protoScala: 40 -> 158 ns.  protoJS
  `records`: 17 -> 58 -> 85 ns at N = 1, 6, 12.
- The sweep reads each candidate's header, finalizes the dead ones and
  relinks them; survivors get an atomic unmark.  With N allocating threads
  those cells were written last on other cores, and the freelist chunks the
  sweep publishes are consumed on other cores again.  Cache-coherence
  misses fit the numbers (a cross-core transfer costs on the order of
  100 ns) but were not measured: `perf c2c` or per-thread `perf stat` on
  the collector thread is the next step.  Contention on `globalMutex` when
  a 4,096-cell chunk is published would show up as time off the CPU, and
  there is none.

What this means for parallelising the sweep: the Amdahl bounds below
assume a phase that runs k times faster.  If the per-cell cost is
coherence traffic, k sweeping threads divide the cells but not the
traffic, and the speed-up will be below k.  A sweep that runs on the core
that allocated the cells (per-thread or per-segment-owner sweeping) attacks
the cost itself; the measurement here cannot tell the two apart.

## Amdahl estimates (completed multi-threaded runs)

`Wall bound` is the mutator floor defined under Method: no thread waits,
the collector fully overlapped.  `Sweep speed-up that reaches it` is the
factor k for which `busy - sweep x (1 - 1/k)` equals the floor; "none
needed" means the collector is already busy for less time than the
mutators' own work, so the remaining wait comes from when cycles start,
not from how long they take.

| Workload | Threads | Wall s | GC busy s | Sweep s | Mark s | Waits, share of mutator thread-time | Wall bound s (zero wait) | Sweep speed-up that reaches it (mark unchanged) | Wall bound, sweep x2 | Wall bound, sweep x4 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| js40_records_n6 | 6 | 16.3 | 10.7 | 7.3 | 2.5 | 22% | 12.7 (-22%) | none needed | 12.7 (-22%) | 12.7 (-22%) |
| js40_records_n12 | 12 | 41.6 | 37.0 | 27.0 | 7.5 | 46% | 22.3 (-46%) | x2.2 | 23.5 (-43%) | 22.3 (-46%) |
| js40_join_n6 | 6 | 15.1 | 5.5 | 3.6 | 1.6 | 15% | 12.9 (-15%) | none needed | 12.9 (-15%) | 12.9 (-15%) |
| js40_join_n12 | 12 | 39.1 | 29.9 | 18.3 | 10.0 | 38% | 24.3 (-38%) | x1.4 | 24.3 (-38%) | 24.3 (-38%) |
| js40_doctree_n6 | 6 | 11.6 | 3.9 | 2.3 | 1.1 | 12% | 10.2 (-12%) | none needed | 10.2 (-12%) | 10.2 (-12%) |
| js40_doctree_n12 | 12 | 28.6 | 21.7 | 13.4 | 6.0 | 35% | 18.5 (-35%) | x1.3 | 18.5 (-35%) | 18.5 (-35%) |
| js40_wordfreq_n6 | 6 | 8.2 | 2.1 | 1.3 | 0.6 | 10% | 7.4 (-10%) | none needed | 7.4 (-10%) | 7.4 (-10%) |
| js40_wordfreq_n12 | 12 | 15.2 | 8.5 | 6.0 | 1.7 | 17% | 12.6 (-17%) | none needed | 12.6 (-17%) | 12.6 (-17%) |
| js40_graph_n6 | 6 | 19.3 | 11.8 | 7.6 | 3.2 | 20% | 15.4 (-20%) | none needed | 15.4 (-20%) | 15.4 (-20%) |
| js40_graph_n12 | 12 | 61.2 | 56.3 | 37.3 | 15.5 | 50% | 30.8 (-50%) | x3.1 | 37.6 (-38%) | 30.8 (-50%) |
| js10_records_n6 | 6 | 8.2 | 5.9 | 4.1 | 1.4 | 37% | 5.2 (-37%) | x1.2 | 5.2 (-37%) | 5.2 (-37%) |
| js10_records_n12 | 12 | 20.1 | 17.7 | 13.0 | 3.6 | 56% | 8.9 (-56%) | x3.2 | 11.2 (-44%) | 8.9 (-56%) |
| js10_doctree_n6 | 6 | 5.4 | 2.9 | 1.6 | 0.9 | 24% | 4.1 (-24%) | none needed | 4.1 (-24%) | 4.1 (-24%) |
| js10_doctree_n12 | 12 | 14.2 | 12.2 | 7.3 | 3.7 | 47% | 7.5 (-47%) | x2.8 | 8.5 (-40%) | 7.5 (-47%) |
| js10_wordfreq_n6 | 6 | 4.2 | 1.2 | 0.7 | 0.4 | 14% | 3.6 (-14%) | none needed | 3.6 (-14%) | 3.6 (-14%) |
| js10_wordfreq_n12 | 12 | 9.2 | 6.5 | 2.7 | 3.3 | 42% | 5.3 (-42%) | x1.8 | 5.3 (-42%) | 5.3 (-42%) |
| js10_graph_n6 | 6 | 10.8 | 8.5 | 4.6 | 3.2 | 43% | 6.1 (-43%) | x2.0 | 6.1 (-43%) | 6.1 (-43%) |
| jsad_records_n12 | 12 | 46.3 | 35.1 | 24.3 | 8.7 | 63% | 17.1 (-63%) | x3.9 | 23.0 (-50%) | 17.1 (-63%) |
| jsad_join_n12 | 12 | 39.9 | 35.5 | 20.9 | 12.7 | 44% | 22.5 (-44%) | x2.7 | 25.1 (-37%) | 22.5 (-44%) |
| jsad_doctree_n12 | 12 | 34.7 | 29.3 | 17.3 | 9.2 | 40% | 20.8 (-40%) | x2.0 | 20.8 (-40%) | 20.8 (-40%) |
| jsad_wordfreq_n12 | 12 | 29.1 | 20.2 | 10.7 | 8.2 | 55% | 13.2 (-55%) | x2.9 | 14.8 (-49%) | 13.2 (-55%) |
| jsad_graph_n12 | 12 | 63.9 | 60.7 | 35.9 | 20.6 | 52% | 30.9 (-52%) | x5.9 | 42.8 (-33%) | 33.8 (-47%) |
| scala_tree_t6 | 6 | 89.6 | 82.2 | 69.2 | 11.6 | 45% | 49.3 (-45%) | x1.9 | 49.3 (-45%) | 49.3 (-45%) |
| clj_coll_t6 | 6 | 129.2 | 120.9 | 118.7 | 2.0 | 66% | 43.4 (-66%) | x2.9 | 61.6 (-52%) | 43.4 (-66%) |

Reading:
- **Mark in parallel** would gain little where the sweep does not already
  reach the bound: mark is 1.7-21 s of busy time at N = 12, 1-50 % of it, and
  every completed workload reaches its bound with the sweep alone.
- **STW, Phase 5b and unmark** are below 15 % of busy time together, the
  pause below 0.025 %: parallelising them changes nothing measurable.
- **The sweep** is where a 2-4 x speed-up would turn 17-66 % of the
  mutators' wall time at N = 12 into work, if it scales.  The CPU-time table
  says it may not scale linearly.
- **The N = 6, 40 M-cell runs** need no speed-up at all: the collector
  is idle for most of the run but each cycle starts only at the ceiling, so
  the mutators stop for its whole length (10-22 % of their time).  The
  adaptive controller's pacing exists for this case; a fixed limit with
  an early request (the soft zone) would do the same.

## Other observations

- **protoScala keeps a function's garbage until the function returns.**
  One loop of 100 rounds inside a Future went out of memory at a 2 M-cell
  limit with the live set equal to the heap; split into Futures of five
  rounds it ran (20 batches) in 155 MB peak RSS.  Worth its own investigation in protoScala.
- **protoClojure `pmap` with 6 tasks took 129 s for 6 x the work that one
  task did in 14.5 s** (1.5 x slower than sequential): 94 % of the wall
  time is collector time, 98 % of it sweep at 7 M cells/s.
- **protoJS `graph` N = 12 at 10 M cells** now fails (see above).

## Data

Raw per-run records (JSON lines, with the last `[GC-PHASES]` line parsed),
the runner, the two new workloads and the tabulator are kept outside the
repository; the branch `measure/gc-phases` holds the instrumentation.  To
reproduce a row: build that branch with `-DPROTOCORE_GC_INSTRUMENT=ON`,
run the binary with `LD_LIBRARY_PATH` pointing at the build and
`PROTOCORE_GC_PROFILE=1`, and read the last `[GC-PHASES]` line.
