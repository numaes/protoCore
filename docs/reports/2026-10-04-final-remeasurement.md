# Final re-measurement after the October 2026 work

Date: 2026-10-04.  Author: Gustavo Marino, with Claude.

**Before**: the runtime packages built on 2026-10-03 (05:20-05:30, the
binaries installed on the measuring machine and used by the baselines) on
protoCore 2.10.2.  **After**: every runtime at its default branch with write
coalescing and the CI pin on protoCore 2.14.1, packaged on 2026-10-04, on
protoCore 2.14.1 (master `94951559`).  Both sides ran in **one session,
interleaved run by run** (before, after, before, after, ...), median of 3.

**Everything here is synthetic and notebook-class.**  The workloads are the
baselines' benchmarks written for this platform, used as evidence about
mechanisms, not as a prediction of any application.  The machine is DEV12,
an AMD Ryzen 5 5500U (Zen 2 "Lucienne"): 6 cores / 12 threads in two core
complexes, 4 MB of L3 per complex (8 MB in all), two DDR4 channels, one NUMA
node, 62 GB, a laptop power envelope.  **No server-class hardware was
measured**; the section "Hardware class" says which conclusions are expected
to carry over and which are not.

The baselines are the 2026-10-03 reports:
[collector time, phase by phase](2026-10-03-gc-phase-breakdown.md) (2.10.2),
[adaptive heap calibration](2026-10-03-adaptive-heap-calibration.md),
protoJS's structure benchmarks (protoJS
`benchmarks/reports/2026-10-03-structure-benchmarks.md`) and memory per
object (protoJS `benchmarks/reports/2026-10-02-memory-per-object.md`).  What
happened between them is in
[collector throughput](2026-10-03-collector-throughput.md) (2.12.0-2.14.0)
and in each runtime's CHANGELOG (write coalescing on `setAttributes`,
protoCore 2.11.0).

## Verdict

Synthetic workloads, notebook-class machine (Ryzen 5 5500U), before = the
2026-10-03 binaries on protoCore 2.10.2, after = the 2026-10-04 binaries on
2.14.1, medians of 3 in one interleaved session:

| Workload (threads) | Wall s | Mutator wait share | Sweep ns per cell | Peak RSS MB |
|---|---|---|---|---|
| protoClojure `coll_alloc` (6), 2 M-cell limit | 117.8 -> 45.1 (-62 %) | 66.6 -> 10.0 % | 136 -> 45 | 158 -> 155 |
| protoScala `tree_alloc` (6), 2 M-cell limit | 88.9 -> 57.7 (-35 %) | 46.3 -> 35.8 % | 162 -> 122 | 182 -> 188 |
| protoJS `records` (12 Deferreds), 40 M cells | 37.4 -> 21.5 (-43 %) | 47.2 -> 5.6 % | 59 -> 20 | 2688 -> 2598 |
| protoJS `records` (12), 10 M cells | 19.0 -> 10.3 (-46 %) | 60.5 -> 19.5 % | 83 -> 12 | 746 -> 726 |
| protoJS `records` (12), controller, H = 40 M | 46.4 -> 21.7 (-53 %) | 61.6 -> 7.7 % | 66 -> 21 | 2110 -> 2588 (+23 %) |
| protoJS CAD model, 20 k parts (1 + 12) | 234 -> 145 (-38 %) | 23.6 -> 14.6 % | 121 -> 21 | 1250 -> 1218 |
| protoJS `graph` (12), 10 M cells | out of memory -> 48.1 | - -> 74 % | - -> 23 | - -> 951 |
| core benchmark (1), 640 MB fixed | 3.60 -> 2.73 (-24 %) | 34.8 -> 4.2 % | 18.7 -> 10.0 | 672 -> 664 |
| **protoClojure `coll_alloc` (1)**, 2 M-cell limit | **11.4 -> 14.4 (+26 %)** | 27.5 -> 0.0 % | 40 -> 33 | 135 -> 135 |
| **protoJS `doctree` (6), 40 M cells** | **9.50 -> 10.06 (+6 %)** | 13.7 -> 4.2 % | 24 -> 15 | 2578 -> 2582 |

| Other measure | Before -> after |
|---|---|
| Cells per construction, write coalescing (whole loop) | protoJS 125.5 -> 39.0, protoST 88.6 -> 32.1, protoScala 32.0 -> 11.0, protoPython 252 -> 188; protoClojure (no groups) 81.6 -> 81.6 |
| protoJS Deferreds at N = 12, structure benchmarks | 14-51 % faster (`records` 12.4 -> 6.1 s); sequential runs within -8 to +4 % except two rows |
| Single-threaded instruction counts (36 programs) | equal, within ±1.4 %, except `py/richards_lite` -19 % |
| Single-threaded cycles | -9.5 to +4.3 %, except **protoCore `immutable_sharing_benchmark` +8.5 %** |
| Stop-the-world, longest in any run | 0.16 ms |

- **Most of the gain is the sweep**: its cost per cell fell 2-7 times at
  6-12 threads, also on aged heaps where nearly every refill is recycled
  memory, and the mutators' waits fell with it; 27 of the 32 collecting
  workloads got faster (median about 20 %).
- **Write coalescing removes 25-69 % of the allocation it targets** and
  changes nothing else (with it switched off the after binaries allocate
  exactly what the before binaries did).
- **Regressions** (section 6): a single-mutator workload is 26 % slower
  because the 8-chain sweep competes with the mutator (one chain restores
  the old time; hardware-sensitive); 6 threads on a 40 M-cell limit, where
  the collector already kept up, are up to 6 % slower from pacing's extra
  cycles; the controller holds more memory than 2.10.2's (+23 to +64 %) for
  much shorter runs; one protoCore micro-benchmark has 8.5 % more cycles,
  unexplained.
- **The adaptive controller** (not adopted by any runtime): recommended for
  protoPython, protoScala and protoClojure, whose default never collects;
  for protoJS's parallel programs; not yet for protoST (section 7).

## What was compared

| | Before | After |
|---|---|---|
| protoCore | 2.10.2 (`b7f6d82a`) | 2.14.1 (`94951559`) |
| protoJS | `4b02a9565` (no write groups) | `a9db7f8e3` (write groups, strict-mode fixes) |
| protoPython | `284df3e9` | `090c9c0a` (attribute write groups) |
| protoST | `5badd33` | `0d9ed15` (instance-variable write groups) |
| protoScala | `04f82c3` | `ecb09aa` (constructor field groups) |
| protoClojure | `1e038da` | `df9f7ca` (no source change: coalescing does not apply) |

Between the two protoCore versions: 2.11.0 `setAttributes`; 2.12.0 pacing,
early wake, batched segment recycling, one store per survivor,
test-before-unmark, the multi-cursor sweep with prefetch; 2.13.0 the
controller's control law; 2.14.0 the sweep's helper threads; 2.14.1 the
thread-exit quorum fix (correctness only).  Each runtime's default heap
policy is unchanged: protoJS 75 % of the memory available, protoST a fixed
10 M-cell (640 MB) limit, protoPython, protoScala and protoClojure no limit.
No runtime enables the adaptive controller; where it appears below it was
switched on with the diagnostic `PROTOCORE_ADAPTIVE_HEAP=1`.

## Method

- **Binaries.**  Each side's five runtime `.deb` packages were extracted
  with `dpkg-deb -x` into a private tree (protoCore's package was not
  extracted there), so each binary finds its own standard library next to
  it and, through its RUNPATH, its own `libprotoPython` / `libprotoScala`.
  `libprotoCore.so.3` came from `LD_LIBRARY_PATH`; `ldd` confirmed the
  library of each side, and the instrumented runs printed the
  `[GC-PHASES]` lines only an instrumented library emits.
- **Libraries.**  Collector figures (waits, sweep ns per cell, collector
  CPU, fresh and recycled refills) come from Release builds with
  `-DPROTOCORE_GC_INSTRUMENT=ON`: for 2.10.2 the very library of the phase
  report (master `b7f6d82a` plus the instrumentation commits `f4e05e57`
  and `2013b02d`, its scratch build, kept), for 2.14.1 the same option on
  master.  Everything else (section 2, the single-threaded `perf stat`
  runs, protoJS's structure benchmarks) used the packaged Release
  libraries, without instrumentation.
- **Runs.**  Sequential.  Each in `systemd-run --user --scope -p
  MemoryMax=<cap> -p MemorySwapMax=0` under `/usr/bin/time` (wall, user +
  system CPU, peak RSS).  Every run verifies itself and a run that does not
  is never a data point: protoJS prints `"ok":true` and per-task checksums
  (identical in every run of a workload, before and after, and equal to
  Node.js's in the structure benchmarks); protoScala `checksum=540451801`;
  protoClojure the closed-form checksum; the core benchmark `verified=yes`;
  section 2's benchmarks their own `VERIFIED` / `BENCH_RESULT` /
  `// EXPECT:` line; the `perf stat` programs must print the same result on
  both sides.
- **Quiet machine.**  No other agent, build or test suite ran during the
  session (2026-10-04, 02:55-07:20 UTC).  The machine is the maintainer's desktop: an
  editor's GPU process took one CPU continuously and a browser about half
  of one (1.9-2.0 CPUs busy before each run, recorded per run).  Each run
  started only when no compiler, linker or test suite was running and,
  from the collector group on, when fewer than 2.5 CPUs were busy over
  3 s; the calibration matrix (section 2's first table), which ran first,
  used the 1-minute load average (below 4.5) instead.  The background load is the same for both sides because they
  are interleaved, but it takes CPU from the 12-thread runs, so absolute
  figures at N = 12 are pessimistic.
- **Reproducibility of the baseline.**  The before side re-measured in this
  session is 1-10 % faster than the 2026-10-03 phase report's single runs
  (protoJS `records` N = 12 at 40 M cells 37.4 against 41.6 s,
  `clj_coll_t6` 117.8 against 129.2 s, `scala_tree_t6` 88.9 against
  89.6 s, the CAD model 234 against 247 s).
- **Medians of 3**, except: three protoJS workloads at 10 M cells ran
  once on each side (`join` N = 6 and 12, out of memory in both builds;
  `graph` N = 12, out of memory before and completed after), because a run
  that ends in out-of-memory gives the same verdict each time and costs up
  to 9 minutes; protoJS's structure benchmarks are one process per
  configuration and side, each reporting the median of 3 repetitions
  inside the process (the method of protoJS's report).  The cells-per-
  operation runs (section 5) and the diagnostic of section 6 are medians of
  3 as well.

Harness, workloads and raw records:
[data/2026-10-04-final-remeasurement/](data/2026-10-04-final-remeasurement/),
machine paths scrubbed.  `fm.py` runs the groups (`gc`: section 1; `cal`
and `policy`: section 2; `rt6`: section 4; `cells`: section 5; `diag`:
section 6), `st6.sh` protoCore's six benchmarks, `struct.sh` the structure
benchmarks, `run_all.sh` the session's order; `tab.py <group> <records>`
prints every table of this report from `results/`.  The session ran
`run_all.sh` (cells, rt6, cal, the first repetition of gc, struct), then
the second and third repetitions of `gc` (without the three
out-of-memory workloads), `policy` and `diag`.

## 1. Collector workloads (the phase report's set)

Wait share = mutator time waiting for heap headroom / (wall x threads).
Sweep ns/cell = sweep phase time / cells examined.  Collector CPU = the
collector thread's CPU time (helpers' time is in process CPU).

| Workload | Wall s | Peak RSS MB | Cycles | Wait share | Sweep ns/cell | Collector CPU s | Process CPU s | runs ok (b/a) |
|---|---|---|---|---|---|---|---|---|
| core_fixed640_live1M | 3.60 -> 2.73 (-24.2 %) | 672 -> 664 (-1.1 %) | 7 -> 12 | 34.8 -> 4.2 % | 18.7 -> 10.0 (-46.7 %) | 2.0 -> 1.9 (-1.7 %) | 4.3 -> 4.6 (+7.0 %) | 3/3 |
| core_adaptive_live1M | 4.37 -> 2.36 (-46.0 %) | 544 -> 891 (+63.7 %) | 16 -> 14 | 57.6 -> 3.9 % | 24.7 -> 10.0 (-59.5 %) | 3.2 -> 1.8 (-43.1 %) | 5.0 -> 4.0 (-20.4 %) | 3/3 |
| js40_records_n1 | 6.98 -> 7.11 (+1.9 %) | 2245 -> 2248 (+0.2 %) | 0 -> 0 | 0.0 -> 0.0 % | - -> - | 0.0 -> 0.0 (-) | 7.0 -> 7.1 (+1.9 %) | 3/3 |
| js40_records_n6 | 15.34 -> 12.60 (-17.9 %) | 2574 -> 2548 (-1.0 %) | 5 -> 8 | 21.7 -> 3.0 % | 34.0 -> 18.8 (-44.9 %) | 10.0 -> 8.0 (-20.4 %) | 60.2 -> 62.4 (+3.6 %) | 3/3 |
| js40_records_n12 | 37.39 -> 21.47 (-42.6 %) | 2688 -> 2598 (-3.4 %) | 10 -> 14 | 47.2 -> 5.6 % | 59.3 -> 20.2 (-66.0 %) | 32.7 -> 16.7 (-49.0 %) | 167.5 -> 164.7 (-1.7 %) | 3/3 |
| js40_join_n1 | 6.13 -> 5.93 (-3.3 %) | 1496 -> 1493 (-0.3 %) | 0 -> 0 | 0.0 -> 0.0 % | - -> - | 0.0 -> 0.0 (-) | 6.2 -> 6.0 (-3.2 %) | 3/3 |
| js40_join_n6 | 12.43 -> 11.90 (-4.3 %) | 2658 -> 2705 (+1.8 %) | 3 -> 6 | 15.2 -> 3.1 % | 29.4 -> 20.1 (-31.5 %) | 4.9 -> 6.9 (+40.3 %) | 56.2 -> 58.8 (+4.6 %) | 3/3 |
| js40_join_n12 | 33.30 -> 24.82 (-25.5 %) | 2901 -> 2808 (-3.2 %) | 8 -> 13 | 39.9 -> 15.0 % | 61.3 -> 25.7 (-58.1 %) | 25.7 -> 20.6 (-19.7 %) | 169.7 -> 167.0 (-1.6 %) | 3/3 |
| js40_doctree_n1 | 5.20 -> 5.20 (+0.0 %) | 1096 -> 1097 (+0.0 %) | 0 -> 0 | 0.0 -> 0.0 % | - -> - | 0.0 -> 0.0 (-) | 5.2 -> 5.2 (+0.0 %) | 3/3 |
| js40_doctree_n6 | 9.50 -> 10.06 (+5.9 %) | 2578 -> 2582 (+0.1 %) | 2 -> 4 | 13.7 -> 4.2 % | 23.9 -> 15.3 (-35.9 %) | 3.3 -> 5.0 (+50.0 %) | 43.1 -> 48.1 (+11.5 %) | 3/3 |
| js40_doctree_n12 | 27.03 -> 21.63 (-20.0 %) | 2672 -> 2656 (-0.6 %) | 6 -> 10 | 37.5 -> 14.7 % | 54.4 -> 16.5 (-69.6 %) | 20.3 -> 17.1 (-15.7 %) | 140.2 -> 144.9 (+3.3 %) | 3/3 |
| js40_wordfreq_n1 | 4.85 -> 4.76 (-1.9 %) | 1133 -> 1145 (+1.1 %) | 0 -> 0 | 0.0 -> 0.0 % | - -> - | 0.0 -> 0.0 (-) | 4.9 -> 4.8 (-1.8 %) | 3/3 |
| js40_wordfreq_n6 | 7.57 -> 7.67 (+1.3 %) | 2537 -> 2607 (+2.8 %) | 2 -> 4 | 9.3 -> 3.0 % | 15.9 -> 15.2 (-4.5 %) | 2.0 -> 3.0 (+50.4 %) | 33.1 -> 36.4 (+10.2 %) | 3/3 |
| js40_wordfreq_n12 | 13.76 -> 13.42 (-2.5 %) | 2580 -> 2692 (+4.4 %) | 5 -> 9 | 17.7 -> 3.1 % | 27.8 -> 18.1 (-35.0 %) | 7.5 -> 9.1 (+22.7 %) | 90.9 -> 95.7 (+5.3 %) | 3/3 |
| js40_graph_n1 | 8.29 -> 8.27 (-0.2 %) | 2488 -> 2488 (+0.0 %) | 1 -> 1 | 2.4 -> 2.5 % | 9.4 -> 9.5 (+0.9 %) | 0.6 -> 0.7 (+1.5 %) | 8.8 -> 8.8 (-0.6 %) | 3/3 |
| js40_graph_n6 | 18.06 -> 15.71 (-13.0 %) | 2622 -> 2623 (+0.0 %) | 7 -> 12 | 20.1 -> 3.5 % | 28.6 -> 18.1 (-36.5 %) | 11.3 -> 11.2 (-0.8 %) | 81.1 -> 84.0 (+3.6 %) | 3/3 |
| js40_graph_n12 | 58.71 -> 47.46 (-19.2 %) | 2776 -> 2824 (+1.7 %) | 17 -> 25 | 50.6 -> 31.5 % | 61.4 -> 17.8 (-71.0 %) | 52.8 -> 40.1 (-24.1 %) | 253.8 -> 262.8 (+3.6 %) | 3/3 |
| js10_records_n1 | 3.40 -> 3.43 (+0.9 %) | 650 -> 650 (+0.0 %) | 1 -> 1 | 4.4 -> 3.3 % | 16.1 -> 7.3 (-54.6 %) | 0.3 -> 0.2 (-30.3 %) | 3.6 -> 3.7 (+5.1 %) | 3/3 |
| js10_records_n6 | 7.47 -> 5.54 (-25.8 %) | 694 -> 702 (+1.2 %) | 7 -> 11 | 38.0 -> 8.4 % | 53.9 -> 11.5 (-78.6 %) | 5.4 -> 3.5 (-34.7 %) | 23.7 -> 24.3 (+2.6 %) | 3/3 |
| js10_records_n12 | 18.98 -> 10.27 (-45.9 %) | 746 -> 726 (-2.7 %) | 15 -> 21 | 60.5 -> 19.5 % | 82.6 -> 12.4 (-85.0 %) | 16.7 -> 7.8 (-53.5 %) | 63.4 -> 60.4 (-4.7 %) | 3/3 |
| js10_join_n1 | 2.40 -> 2.35 (-2.1 %) | 611 -> 607 (-0.6 %) | 0 -> 0 | 0.0 -> 0.0 % | - -> - | 0.0 -> 0.0 (-) | 2.4 -> 2.4 (-1.2 %) | 3/3 |
| js10_join_n6 | out of memory in both (one run each; time to the verdict 140.9 -> 30.2 s, 147 -> 40 cycles) | | | | | | | 0/0 |
| js10_join_n12 | out of memory in both (one run each; time to the verdict 510.8 -> 515.8 s, 386 -> 518 cycles) | | | | | | | 0/0 |
| js10_doctree_n1 | 2.18 -> 2.21 (+1.4 %) | 450 -> 451 (+0.1 %) | 0 -> 0 | 0.0 -> 0.0 % | - -> - | 0.0 -> 0.0 (-) | 2.2 -> 2.2 (+0.9 %) | 3/3 |
| js10_doctree_n6 | 4.64 -> 3.91 (-15.7 %) | 686 -> 685 (-0.2 %) | 4 -> 6 | 24.8 -> 5.6 % | 36.2 -> 17.3 (-52.3 %) | 2.5 -> 2.4 (-2.6 %) | 16.4 -> 17.7 (+8.0 %) | 3/3 |
| js10_doctree_n12 | 12.81 -> 9.25 (-27.8 %) | 742 -> 734 (-1.0 %) | 10 -> 13 | 48.7 -> 33.0 % | 68.4 -> 11.7 (-82.8 %) | 11.2 -> 7.2 (-35.5 %) | 50.1 -> 50.5 (+1.0 %) | 3/3 |
| js10_wordfreq_n1 | 2.30 -> 2.24 (-2.6 %) | 514 -> 490 (-4.6 %) | 0 -> 0 | 0.0 -> 0.0 % | - -> - | 0.0 -> 0.0 (-) | 2.3 -> 2.2 (-2.6 %) | 3/3 |
| js10_wordfreq_n6 | 3.34 -> 3.26 (-2.4 %) | 687 -> 707 (+2.8 %) | 3 -> 5 | 15.3 -> 2.3 % | 20.2 -> 17.0 (-15.7 %) | 1.0 -> 1.5 (+44.5 %) | 11.5 -> 12.4 (+7.4 %) | 3/3 |
| js10_wordfreq_n12 | 8.29 -> 6.61 (-20.3 %) | 917 -> 907 (-1.0 %) | 8 -> 10 | 40.3 -> 27.2 % | 39.0 -> 18.0 (-53.7 %) | 5.8 -> 4.4 (-24.2 %) | 32.5 -> 32.8 (+0.9 %) | 3/3 |
| js10_graph_n1 | 2.79 -> 2.71 (-2.9 %) | 653 -> 653 (+0.0 %) | 1 -> 1 | 3.6 -> 3.3 % | 9.6 -> 9.3 (-3.5 %) | 0.2 -> 0.2 (-3.3 %) | 2.9 -> 2.8 (-2.8 %) | 3/3 |
| js10_graph_n6 | 9.92 -> 8.06 (-18.7 %) | 745 -> 740 (-0.8 %) | 12 -> 18 | 42.3 -> 27.1 % | 50.9 -> 20.6 (-59.5 %) | 7.9 -> 6.4 (-19.3 %) | 32.4 -> 33.5 (+3.5 %) | 3/3 |
| js10_graph_n12 | out of memory (7.8 s) -> **48.05** (completes) | - -> 951 | - -> 62 | - -> 74.0 % | - -> 22.6 | - -> 42.0 | - -> 120.9 | 0/1 (before: out of memory, one run) |
| jsad_records_n12 | 46.37 -> 21.67 (-53.3 %) | 2110 -> 2588 (+22.7 %) | 24 -> 24 | 61.6 -> 7.7 % | 66.3 -> 20.6 (-68.9 %) | 36.4 -> 18.8 (-48.3 %) | 154.8 -> 160.5 (+3.7 %) | 3/3 |
| jsad_join_n12 | 36.95 -> 23.70 (-35.9 %) | 2875 -> 2817 (-2.0 %) | 17 -> 19 | 42.7 -> 16.7 % | 64.2 -> 25.9 (-59.6 %) | 32.1 -> 19.9 (-38.1 %) | 168.3 -> 160.9 (-4.4 %) | 3/3 |
| jsad_doctree_n12 | 30.12 -> 19.30 (-35.9 %) | 2667 -> 2658 (-0.3 %) | 14 -> 15 | 45.4 -> 16.6 % | 57.1 -> 17.0 (-70.2 %) | 25.0 -> 16.4 (-34.5 %) | 138.0 -> 137.7 (-0.2 %) | 3/3 |
| jsad_wordfreq_n12 | 24.02 -> 13.57 (-43.5 %) | 1805 -> 2762 (+53.0 %) | 24 -> 18 | 57.7 -> 11.2 % | 49.9 -> 18.7 (-62.5 %) | 16.3 -> 10.1 (-37.8 %) | 94.4 -> 94.8 (+0.4 %) | 3/3 |
| jsad_graph_n12 | 59.66 -> 43.74 (-26.7 %) | 2768 -> 2756 (-0.4 %) | 21 -> 30 | 52.1 -> 31.9 % | 61.5 -> 17.6 (-71.3 %) | 55.5 -> 38.7 (-30.3 %) | 254.1 -> 260.1 (+2.3 %) | 3/3 |
| scala_tree_t1 | 16.76 -> 13.59 (-18.9 %) | 153 -> 158 (+3.5 %) | 28 -> 24 | 11.2 -> 0.0 % | 70.1 -> 46.8 (-33.2 %) | 4.0 -> 2.0 (-49.5 %) | 18.9 -> 15.7 (-17.2 %) | 3/3 |
| scala_tree_t6 | 88.88 -> 57.66 (-35.1 %) | 182 -> 188 (+3.3 %) | 199 -> 172 | 46.3 -> 35.8 % | 162.3 -> 121.8 (-24.9 %) | 83.1 -> 49.6 (-40.4 %) | 326.3 -> 258.7 (-20.7 %) | 3/3 |
| clj_coll_t1 | 11.37 -> 14.38 (+26.5 %) | 135 -> 135 (+0.0 %) | 66 -> 92 | 27.5 -> 0.0 % | 40.3 -> 32.7 (-18.8 %) | 5.3 -> 4.3 (-18.1 %) | 13.5 -> 18.7 (+38.8 %) | 3/3 |
| clj_coll_t6 | 117.82 -> 45.06 (-61.8 %) | 158 -> 155 (-1.4 %) | 354 -> 460 | 66.6 -> 10.0 % | 136.3 -> 45.0 (-67.0 %) | 109.2 -> 30.6 (-72.0 %) | 332.1 -> 291.3 (-12.3 %) | 3/3 |
| cad_20k | 234.40 -> 144.53 (-38.3 %) | 1250 -> 1218 (-2.5 %) | 69 -> 104 | 23.6 -> 14.6 % | 120.7 -> 21.3 (-82.4 %) | 209.0 -> 130.0 (-37.8 %) | 417.4 -> 388.1 (-7.0 %) | 3/3 |

Reading (synthetic, this machine):

- **27 of the 32 workloads that collected and completed in both builds got
  faster**, by 2 to 62 % (median about 20 %).  The largest gains are where
  the single collector was the bottleneck in the phase report: protoClojure
  `coll_alloc` with 6 tasks 117.8 -> 45.1 s (-62 %), protoJS `records` with
  12 Deferreds and the controller 46.4 -> 21.7 s (-53 %), `records` N = 12
  at 10 M cells 19.0 -> 10.3 s (-46 %) and at 40 M cells 37.4 -> 21.5 s
  (-43 %), the CAD model 234 -> 145 s (-38 %), protoScala `tree_alloc`
  with 6 tasks 88.9 -> 57.7 s (-35 %).
- **The mutators wait much less**: 66.6 -> 10.0 % (`clj_coll_t6`), 60.5 ->
  19.5 % (`js10_records_n12`), 47.2 -> 5.6 % (`js40_records_n12`), and in
  regime 1 (6 threads, 40 M cells: the collector keeps up) 15-22 % -> 3 %.
  The waits that remain are at 12 threads on a 10 M-cell heap (19-33 %),
  `graph` N = 12 (32 %), and protoScala with 6 tasks (36 %).
- **The sweep costs 2-7 times less per cell at 6-12 threads**: 136 -> 45 ns
  (`clj_coll_t6`), 82.6 -> 12.4 (`js10_records_n12`), 121 -> 21 (CAD),
  59 -> 20 (`js40_records_n12`).  protoScala's 6-task sweep improved least,
  162 -> 122 ns per cell: it is still the slowest per cell of the set.
- **More cycles, more marking.**  Pacing starts cycles before the ceiling,
  so the after side runs more of them (`js40_records_n12` 10 -> 14,
  `clj_coll_t6` 354 -> 460, CAD 69 -> 104) and marks more in total (CAD
  59 -> 84 s, `js40_graph_n12` 15 -> 28 s).  Where the sweep was the
  bottleneck this is repaid many times over; where it was not, it is the
  cost (section 6).
- **Process CPU** is within about ±5 % at 12 threads, -12 to -21 % for the
  protoScala and protoClojure 6-task workloads, and +4 to +12 % at 6
  threads with a 40 M-cell limit (more cycles, and the collector and
  helpers beside six mutators on six cores).
- **Peak RSS is unchanged under fixed limits** (within -5 to +5 %).  Under
  the controller it rose on two protoJS workloads (`jsad_records` +23 %,
  `jsad_wordfreq` +53 %) and on the core benchmark (+64 %): 2.13.0's law
  lets the soft limit grow toward the budget while that reduces waits, by
  design (the collector-throughput report, M3).
- **The pause stays negligible**: the longest stop-the-world in any run was
  0.16 ms.
- **protoJS `graph` with 12 Deferreds at 10 M cells now completes** (48 s,
  one run, 74 % of mutator time waiting, a live set near the limit).  Before,
  it ran out of memory in 7.8 s, as it did three times out of three in the
  phase report.  Why it fits now is not established here.  `join` at
  N = 6 and 12 still runs out of memory at 10 M cells in both builds (its
  live set is above the limit); at N = 6 the after build reaches that
  verdict in 30 s instead of 141 s.

### Fresh against recycled memory, and the aged heap

2.14.1's instrumentation counts refills from fresh OS blocks against refills
from chunks recycled by earlier sweeps (2.10.2's does not, so the split
exists for the after side only).  The aged window is the part of a run after
the collector had freed three times the heap limit.

| Workload | Fresh refills % (whole run) | Aged window: sweep ns/cell b -> a | fresh % | Mark s b -> a | Sweep s b -> a | STW max ms b -> a |
|---|---|---|---|---|---|---|
| core_fixed640_live1M | 14.4 | - -> - | - | 0.50 -> 0.98 | 1.35 -> 0.81 | 0.05 -> 0.04 |
| core_adaptive_live1M | 19.3 | - -> - | - | 1.05 -> 0.91 | 1.97 -> 0.77 | 0.07 -> 0.05 |
| js40_records_n6 | 21.9 | 50.4 -> 25.2 | 0.0 | 2.21 -> 3.27 | 6.78 -> 3.93 | 0.05 -> 0.08 |
| js40_records_n12 | 11.3 | 77.1 -> 22.0 | 0.0 | 6.71 -> 7.02 | 23.79 -> 7.83 | 0.06 -> 0.10 |
| js40_join_n6 | 31.2 | - -> - | - | 1.39 -> 3.63 | 3.23 -> 2.78 | 0.06 -> 0.06 |
| js40_join_n12 | 16.0 | 89.0 -> 31.4 | 0.0 | 8.38 -> 12.58 | 16.53 -> 7.49 | 0.07 -> 0.09 |
| js40_doctree_n6 | 39.3 | - -> - | - | 1.03 -> 2.58 | 1.90 -> 1.70 | 0.07 -> 0.07 |
| js40_doctree_n12 | 19.2 | 83.2 -> 18.9 | 0.0 | 5.58 -> 10.15 | 13.02 -> 4.65 | 0.10 -> 0.12 |
| js40_wordfreq_n6 | 39.8 | - -> - | - | 0.54 -> 1.22 | 1.27 -> 1.52 | 0.04 -> 0.06 |
| js40_wordfreq_n12 | 19.8 | 41.9 -> 24.6 | 0.0 | 1.52 -> 5.06 | 5.52 -> 3.86 | 0.05 -> 0.13 |
| js40_graph_n1 | 95.5 | - -> - | - | 0.19 -> 0.21 | 0.38 -> 0.38 | 0.04 -> 0.05 |
| js40_graph_n6 | 15.2 | 40.0 -> 21.3 | 0.0 | 3.02 -> 5.10 | 7.36 -> 5.00 | 0.05 -> 0.07 |
| js40_graph_n12 | 7.4 | 73.7 -> 19.9 | 0.0 | 15.12 -> 27.71 | 35.69 -> 10.36 | 0.07 -> 0.16 |
| js10_records_n1 | 96.2 | - -> - | - | 0.11 -> 0.11 | 0.16 -> 0.07 | 0.04 -> 0.05 |
| js10_records_n6 | 16.5 | 72.4 -> 14.4 | 0.3 | 1.27 -> 2.11 | 3.77 -> 0.91 | 0.04 -> 0.09 |
| js10_records_n12 | 9.0 | 96.7 -> 13.3 | 0.4 | 3.43 -> 4.96 | 12.34 -> 1.92 | 0.07 -> 0.11 |
| js10_doctree_n6 | 29.4 | - -> - | - | 0.80 -> 1.38 | 1.40 -> 0.73 | 0.06 -> 0.07 |
| js10_doctree_n12 | 15.0 | 91.8 -> 14.0 | 0.0 | 3.48 -> 5.01 | 6.84 -> 1.25 | 0.11 -> 0.10 |
| js10_wordfreq_n6 | 30.6 | - -> - | - | 0.34 -> 0.74 | 0.58 -> 0.56 | 0.04 -> 0.05 |
| js10_wordfreq_n12 | 18.1 | 56.8 -> 17.0 | 4.8 | 2.79 -> 2.93 | 2.68 -> 1.23 | 0.06 -> 0.11 |
| js10_graph_n1 | 94.5 | - -> - | - | 0.09 -> 0.09 | 0.09 -> 0.09 | 0.05 -> 0.05 |
| js10_graph_n6 | 12.3 | 66.3 -> 23.0 | 0.0 | 2.92 -> 4.20 | 4.49 -> 1.94 | 0.06 -> 0.06 |
| js10_graph_n12 | 6.5 | - -> 24.8 | 0.2 | - -> 34.63 | - -> 4.77 | - -> 0.10 |
| jsad_records_n12 | 11.3 | 83.0 -> 22.5 | 0.0 | 8.20 -> 8.39 | 26.47 -> 8.52 | 0.12 -> 0.14 |
| jsad_join_n12 | 16.2 | 87.4 -> 29.5 | 0.0 | 11.61 -> 12.35 | 19.71 -> 7.63 | 0.13 -> 0.11 |
| jsad_doctree_n12 | 19.3 | 75.9 -> 14.8 | 0.0 | 8.18 -> 10.48 | 14.84 -> 4.48 | 0.14 -> 0.16 |
| jsad_wordfreq_n12 | 20.2 | 52.6 -> 25.3 | 0.0 | 6.64 -> 6.27 | 9.89 -> 3.98 | 0.16 -> 0.14 |
| jsad_graph_n12 | 7.5 | 72.5 -> 18.8 | 0.0 | 18.12 -> 26.49 | 35.10 -> 10.17 | 0.10 -> 0.13 |
| scala_tree_t1 | 5.2 | 76.3 -> 51.0 | 0.0 | 0.19 -> 0.15 | 3.79 -> 1.85 | 0.05 -> 0.04 |
| scala_tree_t6 | 0.9 | 164.0 -> 123.6 | 0.0 | 14.18 -> 12.80 | 67.67 -> 38.57 | 0.09 -> 0.10 |
| clj_coll_t1 | 1.5 | 42.1 -> 33.7 | 0.0 | 0.03 -> 0.07 | 5.24 -> 4.24 | 0.06 -> 0.05 |
| clj_coll_t6 | 0.3 | 137.3 -> 45.2 | 0.0 | 1.64 -> 2.41 | 107.44 -> 35.65 | 0.08 -> 0.12 |
| cad_20k | 2.8 | 125.8 -> 21.4 | 0.1 | 59.04 -> 84.21 | 135.85 -> 28.17 | 0.12 -> 0.14 |

- Over whole runs, multi-threaded workloads took 0.3-40 % of their refills
  from fresh OS blocks (most of it during the build phase that precedes the
  parallel tasks); single-task protoJS runs with one cycle took 94-96 %
  from fresh blocks, because they barely recycle.
- **In every aged window the share of fresh refills is 0-5 %**: after the
  heap has cycled a few times, allocation comes almost entirely from
  recycled chunks.  The after side's sweep per cell in those windows
  (13-31 ns for protoJS at 12 threads, 45 for protoClojure with 6 tasks,
  124 for protoScala) is within a few nanoseconds of its whole-run figure,
  and 2-7 times below the before side's aged figure (42-97, 137, 164).  So
  the gains hold on recycled memory, the state a long-running program
  lives in.  Heaps aged for hours were not measured.

## 2. Each runtime's default policy, and the controller

The calibration report's 43 workloads (its `js640` probe dropped: that
binary no longer exists), the release libraries, a 10 GB cap.  Four
variants interleaved: the default policy before and after, the controller
(`PROTOCORE_ADAPTIVE_HEAP=1`, H = 75 % of the cap) after, and for reference
the 2.10.2 controller before.  Medians of 3; 516 runs, none failed.  Times
under 0.1 s are at `/usr/bin/time`'s resolution and are not read.

| Workload | Default policy, before: s / MB / cycles | Default, after: s / MB / cycles | After vs before (time, memory) | Controller, after: s / MB / cycles | Controller vs default, after (time, memory) | Controller, before (2.10.2): s / MB |
|---|---|---|---|---|---|---|
| core/ahb-0k | 1.98 / 645 / 4 | 1.79 / 645 / 6 | -9.6 %, +0.1 % | 1.12 / 132 / 36 | -37.4 %, -79.6 % | 1.15 / 116 |
| core/ahb-100k | 2.21 / 647 / 5 | 1.88 / 646 / 6 | -14.9 %, -0.0 % | 1.26 / 141 / 47 | -33.0 %, -78.2 % | 2.07 / 152 |
| core/ahb-1000k | 3.68 / 672 / 7 | 2.69 / 664 / 12 | -26.9 %, -1.1 % | 2.44 / 891 / 14 | -9.3 %, +34.2 % | 4.45 / 544 |
| core/ahb-5000k | 24.36 / 742 / 25 | 17.02 / 743 / 34 | -30.1 %, +0.1 % | 7.09 / 2826 / 18 | -58.3 %, +280.4 % | 11.04 / 2613 |
| st/saturation_big | 1.74 / 666 / 4 | 1.68 / 669 / 6 | -3.4 %, +0.5 % | 1.86 / 190 / 39 | +10.7 %, -71.6 % | 1.60 / 194 |
| st/parallel_speedup | 0.42 / 608 / 0 | 0.42 / 605 / 0 | +0.0 %, -0.6 % | 0.42 / 185 / 7 | +0.0 %, -69.3 % | 0.35 / 173 |
| st/list_append | 0.07 / 60 / 0 | 0.06 / 48 / 0 | -14.3 %, -19.7 % | 0.06 / 48 / 0 | +0.0 %, -0.0 % | 0.06 / 60 |
| st/str_concat | 0.03 / 32 / 0 | 0.04 / 32 / 0 | +33.3 %, +0.8 % | 0.04 / 32 / 0 | +0.0 %, -0.4 % | 0.04 / 32 |
| st/fib | 0.47 / 300 / 0 | 0.47 / 289 / 0 | +0.0 %, -3.9 % | 1.61 / 564 / 3 | +242.6 %, +95.5 % | 0.76 / 329 |
| st/message_throughput | 0.10 / 80 / 0 | 0.09 / 80 / 0 | -10.0 %, +0.1 % | 0.09 / 76 / 0 | +0.0 %, -4.9 % | 0.10 / 80 |
| py/int_sum_loop | 0.21 / 57 / 0 | 0.19 / 57 / 0 | -9.5 %, -0.1 % | 0.19 / 57 / 0 | +0.0 %, +0.0 % | 0.21 / 57 |
| py/list_append_loop | 0.55 / 393 / 0 | 0.55 / 393 / 0 | +0.0 %, +0.0 % | 0.41 / 147 / 4 | -25.5 %, -62.7 % | 0.51 / 146 |
| py/str_concat_loop | 0.21 / 105 / 0 | 0.22 / 105 / 0 | +4.8 %, -0.1 % | 0.21 / 105 / 0 | -4.5 %, +0.0 % | 0.21 / 105 |
| py/range_iterate | 0.22 / 57 / 0 | 0.20 / 57 / 0 | -9.1 %, +0.1 % | 0.20 / 57 / 0 | +0.0 %, -0.3 % | 0.22 / 57 |
| py/multithread_cpu | 0.17 / 51 / 0 | 0.13 / 47 / 0 | -23.5 %, -7.8 % | 0.12 / 43 / 0 | -7.7 %, -8.8 % | 0.14 / 51 |
| py/attr_lookup | 2.30 / 57 / 0 | 2.40 / 57 / 0 | +4.3 %, -0.3 % | 2.31 / 57 / 0 | -3.7 %, +0.2 % | 2.31 / 57 |
| py/call_recursion | 0.31 / 57 / 0 | 0.30 / 57 / 0 | -3.2 %, -0.1 % | 0.30 / 57 / 0 | +0.0 %, -0.1 % | 0.31 / 57 |
| py/fib | 3.07 / 23 / 0 | 2.85 / 23 / 0 | -7.2 %, +0.4 % | 2.81 / 23 / 0 | -1.4 %, +0.0 % | 3.10 / 22 |
| py/binary_trees | 4.09 / 1852 / 0 | 3.68 / 1645 / 0 | -10.0 %, -11.2 % | 3.61 / 1001 / 7 | -1.9 %, -39.2 % | 4.14 / 734 |
| py/nqueens | 0.66 / 39 / 0 | 0.54 / 39 / 0 | -18.2 %, +0.4 % | 0.54 / 39 / 0 | +0.0 %, -0.2 % | 0.59 / 39 |
| py/richards_lite | 0.05 / 23 / 0 | 0.04 / 23 / 0 | -20.0 %, -1.3 % | 0.04 / 23 / 0 | +0.0 %, -0.0 % | 0.05 / 23 |
| py/sieve | 1.30 / 951 / 0 | 1.31 / 951 / 0 | +0.8 %, -0.0 % | 1.02 / 261 / 10 | -22.1 %, -72.6 % | 1.09 / 231 |
| clj/fib | 0.61 / 22 / 0 | 0.60 / 22 / 0 | -1.6 %, +0.7 % | 0.60 / 22 / 0 | +0.0 %, +0.7 % | 0.59 / 22 |
| clj/tak | 0.04 / 24 / 0 | 0.04 / 24 / 0 | +0.0 %, -0.2 % | 0.05 / 24 / 0 | +25.0 %, -0.0 % | 0.05 / 24 |
| clj/sum-loop | 0.07 / 22 / 0 | 0.07 / 22 / 0 | +0.0 %, +0.5 % | 0.07 / 22 / 0 | +0.0 %, -0.1 % | 0.07 / 22 |
| clj/reduce-list | 0.02 / 22 / 0 | 0.02 / 23 / 0 | +0.0 %, +0.5 % | 0.02 / 22 / 0 | +0.0 %, -0.3 % | 0.02 / 23 |
| clj/sum-squares | 0.02 / 22 / 0 | 0.01 / 22 / 0 | -50.0 %, +0.4 % | 0.02 / 22 / 0 | +100.0 %, +0.5 % | 0.01 / 22 |
| clj/actor-saturation-32 | 2.39 / 23 / 0 | 2.42 / 23 / 0 | +1.3 %, +0.9 % | 2.36 / 23 / 0 | -2.5 %, -0.3 % | 2.41 / 23 |
| scala/fib30 | 0.47 / 27 / 0 | 0.45 / 27 / 0 | -4.3 %, +0.2 % | 0.46 / 27 / 0 | +2.2 %, -0.5 % | 0.45 / 27 |
| scala/list_ops | 0.51 / 321 / 0 | 0.51 / 321 / 0 | +0.0 %, -0.0 % | 0.57 / 205 / 3 | +11.8 %, -36.1 % | 0.58 / 235 |
| scala/map_build | 0.44 / 184 / 0 | 0.40 / 184 / 0 | -9.1 %, +0.1 % | 0.38 / 150 / 1 | -5.0 %, -18.4 % | 0.38 / 133 |
| scala/object_tree | 0.46 / 137 / 0 | 0.41 / 105 / 0 | -10.9 %, -23.5 % | 0.42 / 105 / 0 | +2.4 %, +0.1 % | 0.45 / 141 |
| scala/str_concat | 0.03 / 27 / 0 | 0.03 / 27 / 0 | +0.0 %, -0.1 % | 0.03 / 27 / 0 | +0.0 %, +0.0 % | 0.03 / 27 |
| scala/tak | 0.03 / 27 / 0 | 0.03 / 27 / 0 | +0.0 %, -0.0 % | 0.03 / 27 / 0 | +0.0 %, +0.2 % | 0.04 / 27 |
| scala/sum_loop | 0.08 / 27 / 0 | 0.07 / 27 / 0 | -12.5 %, -0.3 % | 0.08 / 27 / 0 | +14.3 %, +0.7 % | 0.08 / 27 |
| scala/actor-saturation-32 | 2.06 / 44 / 0 | 1.93 / 44 / 0 | -6.3 %, +0.1 % | 2.00 / 44 / 0 | +3.6 %, -0.3 % | 2.10 / 44 |
| js/records-seq | 33.40 / 7855 / 1 | 33.23 / 7855 / 1 | -0.5 %, -0.0 % | 45.16 / 301 / 118 | +35.9 %, -96.2 % | 40.88 / 672 |
| js/doctree-seq | 26.57 / 6111 / 0 | 27.05 / 6127 / 0 | +1.8 %, +0.3 % | 27.13 / 1385 / 22 | +0.3 %, -77.4 % | 30.25 / 1157 |
| js/graph-seq | 45.78 / 7815 / 2 | 47.31 / 7810 / 2 | +3.3 %, -0.1 % | 56.63 / 775 / 66 | +19.7 %, -90.1 % | 53.15 / 749 |
| js/join-seq | 32.63 / 7969 / 1 | 32.51 / 7969 / 1 | -0.4 %, +0.0 % | 38.28 / 571 / 50 | +17.7 %, -92.8 % | 36.57 / 614 |
| js/wordfreq-seq | 22.28 / 6184 / 0 | 22.36 / 6200 / 0 | +0.4 %, +0.3 % | 25.18 / 229 / 50 | +12.6 %, -96.3 % | 25.37 / 197 |
| js/records-par | 12.30 / 7871 / 1 | 13.33 / 7871 / 2 | +8.4 %, +0.0 % | 10.90 / 2332 / 16 | -18.2 %, -70.4 % | 19.93 / 1822 |
| js/probe-100k | 1.04 / 472 / 0 | 1.03 / 472 / 0 | -1.0 %, +0.1 % | 0.94 / 322 / 2 | -8.7 %, -31.9 % | 1.00 / 216 |

516 runs, 0 failed verification

The phase report's allocating workloads with **no heap limit given**, so
that each runtime's own default decides (protoScala and protoClojure: none;
protoJS: 75 % of the 16 GB cap), against the controller; after side,
release library, medians of 3:

| Workload | Default policy: s / peak RSS MB / cycles | Controller: s / peak RSS MB (range) / cycles | Controller vs default (time, memory) |
|---|---|---|---|
| scala_tree_t1 | 13.09 / 2856 / 0 | 13.56 / 166 (165-166) / 23 | +3.6 %, -94.2 % |
| scala_tree_t6 | killed by the 16 GB cap (3 of 3 runs) | 35.04 / 449 (415-454) / 79 | completes where the default does not |
| clj_coll_t1 | 13.53 / 8404 / 0 | 13.57 / 142 (141-142) / 85 | +0.3 %, -98.3 % |
| clj_coll_t6 | killed by the 16 GB cap (3 of 3 runs) | 38.36 / 1700 (337-1722) / 72 | completes where the default does not |
| js_records_n12 | 22.52 / 12618 / 2 | 20.34 / 6871 (4764-7924) / 17 | -9.7 %, -45.5 % |
| js_wordfreq_n12 | 14.34 / 12867 / 1 | 12.81 / 3507 (2585-3804) / 16 | -10.7 %, -72.7 % |
| js_graph_n12 | 27.74 / 12814 / 5 | 28.06 / 11762 (10138-12410) / 13 | +1.2 %, -8.2 % |

Reading:

- **protoScala and protoClojure, whose default is no limit, never collect
  and grow without bound.**  With one task they finish in the same time as
  under the controller but hold 2.9 GB and 8.4 GB of garbage; with six
  tasks they were killed by the 16 GB cap in every run.  The controller
  runs both in 140-450 MB with one task at +0 to +4 % time, and completes
  the six-task runs (protoScala 35.0 s, protoClojure 38.4 s, faster than
  the same programs under the phase report's fixed 2 M-cell limit, 57.7
  and 45.1 s in section 1).  protoClojure's six-task peak RSS under the
  controller varied from 0.34 to 1.7 GB between runs.
- **protoJS with 12 Deferreds**: its default (75 % of memory, 12 GB here)
  collects 1-5 times and holds 12.6-12.9 GB; the controller is 10 % faster
  on `records` and `wordfreq` with 46-73 % less memory, and equal on
  `graph` (+1 %, -8 % memory: its live set is large).  In the policy matrix
  above, sequential protoJS is the opposite: the controller costs 13-36 %
  time on four of five structure workloads (0 % on `doctree`) for 77-96 %
  less memory; `records` with 6 Deferreds is 18 % faster.
- **protoPython** (no limit): the controller changes nothing on programs
  that do not allocate much (no cycle) and bounds the ones that do at
  39-73 % less memory and 2-25 % less time (`list_append_loop`, `sieve`,
  `binary_trees`).
- **protoST** (fixed 640 MB): on the actor benchmarks the controller uses
  69-72 % less memory at equal or up to 11 % more time
  (`saturation_big`).  On `fib.st` it is 3.4 times slower (0.47 -> 1.61 s)
  and uses twice the memory; 2.10.2's law took 0.76 s.  The trace shows
  why: protoST's live set right after start-up is about 1.7 M cells, just
  below the controller's initial soft limit (2 M cells, 128 MiB), so the
  first cycle starts at once, finds almost no garbage, and the law doubles
  S; a third cycle, 0.73 s long, runs after the program's output and the
  process waits for it before exiting.
- **The core benchmark**: the controller is 9-58 % faster than the 640 MB
  fixed limit; with a 1 M or 5 M live set it uses more memory (+34 %,
  +280 %): with a 5 M live set the fixed limit thrashes (24.4 s and 25
  cycles before, 17.0 s and 34 cycles after) and the controller grows to
  fit (7.1 s).

## 3. protoJS structure benchmarks

`benchmarks/structures/run.py` in the configuration of protoJS's
2026-10-03 report (scale 1, N = 1, 2, 4, 6, 12, three repetitions per
process, 40 M-cell limit, 4 GB cap), before and after binaries, release
libraries.  Every protoJS checksum equals Node.js's sequential run's.  One
process per row and side: differences under about 10 % are within the
run-to-run spread the report measured.

| Workload | N | seq ms before -> after | Deferreds ms before -> after | speed-up (seq/par) before -> after | GC cycles (par) b -> a | peak RSS MB (par) b -> a |
|---|--:|---|---|---|---|---|
| records | 1 | 1739 -> 1737 (-0.1 %) | 1858 -> 1842 (-0.9 %) | 0.94 -> 0.94 | 0 -> 0 | 2244 -> 2244 |
| records | 2 | 3375 -> 3319 (-1.7 %) | 1962 -> 1936 (-1.3 %) | 1.72 -> 1.71 | 1 -> 1 | 2536 -> 2536 |
| records | 4 | 6956 -> 6083 (-12.6 %) | 2857 -> 2451 (-14.2 %) | 2.43 -> 2.48 | 3 -> 5 | 2571 -> 2575 |
| records | 6 | 10581 -> 10000 (-5.5 %) | 4286 -> 3059 (-28.6 %) | 2.47 -> 3.27 | 5 -> 8 | 2576 -> 2582 |
| records | 12 | 23135 -> 22361 (-3.3 %) | 12363 -> 6098 (-50.7 %) | 1.87 -> 3.67 | 10 -> 14 | 2674 -> 2596 |
| join | 1 | 1663 -> 1653 (-0.6 %) | 1707 -> 1680 (-1.6 %) | 0.97 -> 0.98 | 0 -> 0 | 1492 -> 1492 |
| join | 2 | 3349 -> 3284 (-1.9 %) | 2100 -> 1968 (-6.3 %) | 1.59 -> 1.67 | 1 -> 1 | 2573 -> 2576 |
| join | 4 | 6529 -> 6398 (-2.0 %) | 2614 -> 2455 (-6.1 %) | 2.50 -> 2.61 | 2 -> 3 | 2637 -> 2572 |
| join | 6 | 10157 -> 9980 (-1.7 %) | 4348 -> 3229 (-25.7 %) | 2.34 -> 3.09 | 3 -> 5 | 2658 -> 2716 |
| join | 12 | 21089 -> 20892 (-0.9 %) | 10408 -> 7014 (-32.6 %) | 2.03 -> 2.98 | 8 -> 14 | 2874 -> 2825 |
| doctree | 1 | 1396 -> 1337 (-4.2 %) | 1366 -> 1390 (+1.8 %) | 1.02 -> 0.96 | 0 -> 0 | 1092 -> 1092 |
| doctree | 2 | 2706 -> 2706 (+0.0 %) | 1479 -> 1471 (-0.5 %) | 1.83 -> 1.84 | 0 -> 0 | 2105 -> 2101 |
| doctree | 4 | 6132 -> 5633 (-8.1 %) | 2214 -> 1811 (-18.2 %) | 2.77 -> 3.11 | 1 -> 2 | 2541 -> 2578 |
| doctree | 6 | 8454 -> 8299 (-1.8 %) | 2909 -> 3302 (+13.5 %) | 2.91 -> 2.51 | 2 -> 4 | 2582 -> 2582 |
| doctree | 12 | 17702 -> 17435 (-1.5 %) | 7958 -> 5223 (-34.4 %) | 2.22 -> 3.34 | 6 -> 9 | 2676 -> 2655 |
| wordfreq | 1 | 1126 -> 1116 (-0.9 %) | 1252 -> 1122 (-10.4 %) | 0.90 -> 0.99 | 0 -> 0 | 1129 -> 1128 |
| wordfreq | 2 | 2273 -> 2363 (+4.0 %) | 1233 -> 1246 (+1.1 %) | 1.84 -> 1.90 | 0 -> 0 | 2130 -> 2130 |
| wordfreq | 4 | 4575 -> 4459 (-2.5 %) | 1392 -> 1371 (-1.5 %) | 3.29 -> 3.25 | 1 -> 2 | 2498 -> 2505 |
| wordfreq | 6 | 7913 -> 6351 (-19.7 %) | 1979 -> 1958 (-1.1 %) | 4.00 -> 3.24 | 2 -> 4 | 2567 -> 2625 |
| wordfreq | 12 | 13490 -> 13240 (-1.9 %) | 4142 -> 3556 (-14.1 %) | 3.26 -> 3.72 | 5 -> 9 | 2584 -> 2699 |
| graph | 1 | 2525 -> 2342 (-7.2 %) | 2377 -> 2333 (-1.9 %) | 1.06 -> 1.00 | 1 -> 1 | 2488 -> 2488 |
| graph | 2 | 4865 -> 4895 (+0.6 %) | 3036 -> 2958 (-2.6 %) | 1.60 -> 1.65 | 2 -> 2 | 2535 -> 2544 |
| graph | 4 | 9587 -> 9199 (-4.0 %) | 4110 -> 3728 (-9.3 %) | 2.33 -> 2.47 | 4 -> 8 | 2601 -> 2631 |
| graph | 6 | 14394 -> 14017 (-2.6 %) | 5502 -> 4956 (-9.9 %) | 2.62 -> 2.83 | 7 -> 13 | 2615 -> 2638 |
| graph | 12 | 34078 -> 34393 (+0.9 %) | 18092 -> 14242 (-21.3 %) | 1.88 -> 2.41 | 17 -> 25 | 2764 -> 2842 |

- **Deferreds at N = 12 are 14-51 % faster** (`records` 12.4 -> 6.1 s,
  `doctree` 8.0 -> 5.2 s, `join` 10.4 -> 7.0 s, `graph` 18.1 -> 14.2 s,
  `wordfreq` 4.1 -> 3.6 s), and the speed-up over protoJS's own sequential
  run at N = 12 rose from 1.9-3.3 to 2.4-3.7.  At N = 4 and 6 most rows
  improve by 6-29 %.
- **Sequential runs hardly move** (within -8 to +4 %, except `records`
  N = 4, -13 %, and `wordfreq` N = 6, -20 %, single processes): they
  collect rarely, and these workloads build objects as literals, which were
  already published once, so write coalescing does not apply to them (the
  instruction counts of section 4 are identical).
- **`doctree` with 6 Deferreds is 13.5 % slower** (2.9 -> 3.3 s), the one
  row that got clearly worse.  Section 1's `js40_doctree_n6` (+6 %) shows
  the same: twice the cycles (2 -> 4) with the collector keeping up.
- protoJS is still 60-140 times slower per task than Node.js in this
  mutable-style code (Node's sequential `records` N = 1: 12 ms in this
  session, protoJS 1.7 s); nothing in this work was aimed at that.

The CAD model was measured at 20,000 parts (section 1, `cad_20k`: build plus
N = 1 and 12, 16 M-cell limit), a tenth of the report's model; the full
200,000-part model (80 minutes per protoJS run) was not re-run.

## 4. Single-threaded programs

`perf stat -r 3 -e instructions:u,cycles:u,task-clock`, release libraries,
before and after interleaved, three rounds; medians of the rounds.

protoCore's six benchmarks (no heap limit, no collection), 2.10.2's build
against 2.14.1's:

| Benchmark | instructions:u 2.10.2 -> 2.14.1 | cycles:u 2.10.2 -> 2.14.1 | task-clock ms | rounds |
|---|---|---|---|---|
| microbenchmark_final | 7.974 -> 7.974 (+0.0 %) G | 1.713 -> 1.741 (+1.6 %) G | 578 -> 579 (+0.1 %) | 3/3 |
| mutable_access_benchmark | 16.206 -> 16.206 (+0.0 %) G | 4.008 -> 4.133 (+3.1 %) G | 1307 -> 1336 (+2.2 %) | 3/3 |
| cache_timing_benchmark | 4.310 -> 4.310 (+0.0 %) G | 0.989 -> 0.985 (-0.4 %) G | 336 -> 335 (-0.3 %) | 3/3 |
| hash_quality_benchmark | 1.600 -> 1.600 (+0.0 %) G | 0.400 -> 0.400 (+0.0 %) G | 136 -> 137 (+1.0 %) | 3/3 |
| object_access_benchmark | 60.201 -> 60.201 (+0.0 %) G | 22.107 -> 22.351 (+1.1 %) G | 6750 -> 6927 (+2.6 %) | 3/3 |
| immutable_sharing_benchmark | 4.574 -> 4.574 (+0.0 %) G | 2.985 -> 3.239 (+8.5 %) G | 1211 -> 1288 (+6.4 %) | 3/3 |

Six programs per runtime (the calibration set's single-threaded ones;
protoJS: each structure workload with one task, and the 100 k-object
probe).  Each pair printed the same result (`py/richards_lite`: the same
`total=57900`; only its printed timings differ):

| Program | instructions:u before -> after | cycles:u before -> after | task-clock ms before -> after | outputs agree | runs ok (b/a) |
|---|---|---|---|---|---|
| st/fib | 1.871 -> 1.863 (-0.4 %) G | 0.845 -> 0.836 (-1.1 %) G | 441 -> 433 (-1.8 %) | yes | 3/3 |
| st/list_append | 0.206 -> 0.205 (-0.7 %) G | 0.128 -> 0.132 (+3.5 %) G | 98 -> 89 (-8.8 %) | yes | 3/3 |
| st/str_concat | 0.101 -> 0.101 (+0.2 %) G | 0.065 -> 0.063 (-3.5 %) G | 39 -> 39 (-1.2 %) | yes | 3/3 |
| st/attr_lookup | 0.804 -> 0.803 (-0.1 %) G | 0.357 -> 0.372 (+4.2 %) G | 210 -> 207 (-1.4 %) | yes | 3/3 |
| st/int_sum_loop | 0.268 -> 0.269 (+0.5 %) G | 0.118 -> 0.122 (+3.8 %) G | 56 -> 58 (+2.7 %) | yes | 3/3 |
| st/range_iterate | 0.359 -> 0.357 (-0.3 %) G | 0.187 -> 0.190 (+1.7 %) G | 124 -> 117 (-5.7 %) | yes | 3/3 |
| py/int_sum_loop | 1.284 -> 1.275 (-0.7 %) G | 0.543 -> 0.491 (-9.5 %) G | 198 -> 185 (-6.5 %) | yes | 3/3 |
| py/list_append_loop | 2.329 -> 2.331 (+0.1 %) G | 0.940 -> 0.942 (+0.2 %) G | 498 -> 503 (+1.0 %) | yes | 3/3 |
| py/str_concat_loop | 1.005 -> 1.006 (+0.1 %) G | 0.444 -> 0.447 (+0.6 %) G | 197 -> 197 (-0.0 %) | yes | 3/3 |
| py/attr_lookup | 18.151 -> 18.203 (+0.3 %) G | 7.218 -> 7.234 (+0.2 %) G | 2262 -> 2272 (+0.5 %) | yes | 3/3 |
| py/call_recursion | 1.952 -> 1.951 (-0.0 %) G | 0.882 -> 0.828 (-6.1 %) G | 305 -> 290 (-5.0 %) | yes | 3/3 |
| py/richards_lite | 0.266 -> 0.216 (-19.0 %) G | 0.124 -> 0.099 (-20.8 %) G | 49 -> 40 (-18.7 %) | yes (total=57900) | 3/3 |
| clj/fib | 4.814 -> 4.814 (+0.0 %) G | 1.941 -> 1.950 (+0.5 %) G | 629 -> 638 (+1.4 %) | yes | 3/3 |
| clj/tak | 0.227 -> 0.227 (+0.0 %) G | 0.097 -> 0.098 (+1.6 %) G | 42 -> 44 (+4.0 %) | yes | 3/3 |
| clj/sum-loop | 0.664 -> 0.665 (+0.0 %) G | 0.183 -> 0.173 (-5.6 %) G | 71 -> 71 (+0.4 %) | yes | 3/3 |
| clj/reduce-list | 0.078 -> 0.078 (+0.0 %) G | 0.037 -> 0.036 (-2.1 %) G | 23 -> 23 (-0.6 %) | yes | 3/3 |
| clj/sum-squares | 0.017 -> 0.017 (+0.0 %) G | 0.011 -> 0.012 (+2.2 %) G | 15 -> 15 (-1.7 %) | yes | 3/3 |
| clj/factorial-100 | 0.007 -> 0.007 (+0.1 %) G | 0.008 -> 0.008 (-2.6 %) G | 13 -> 13 (-0.7 %) | yes | 3/3 |
| scala/fib30 | 3.261 -> 3.289 (+0.9 %) G | 1.377 -> 1.363 (-1.0 %) G | 441 -> 444 (+0.7 %) | yes | 3/3 |
| scala/list_ops | 1.990 -> 1.993 (+0.1 %) G | 0.894 -> 0.890 (-0.4 %) G | 453 -> 450 (-0.6 %) | yes | 3/3 |
| scala/map_build | 1.591 -> 1.594 (+0.2 %) G | 0.761 -> 0.753 (-1.0 %) G | 336 -> 338 (+0.6 %) | yes | 3/3 |
| scala/object_tree | 2.287 -> 2.255 (-1.4 %) G | 1.174 -> 1.120 (-4.6 %) G | 440 -> 407 (-7.5 %) | yes | 3/3 |
| scala/str_concat | 0.059 -> 0.059 (+0.1 %) G | 0.036 -> 0.038 (+4.3 %) G | 27 -> 27 (-0.3 %) | yes | 3/3 |
| scala/tak | 0.115 -> 0.116 (+0.7 %) G | 0.060 -> 0.062 (+3.8 %) G | 33 -> 35 (+5.8 %) | yes | 3/3 |
| js/records-seq1 | 14.558 -> 14.560 (+0.0 %) G | 8.013 -> 8.155 (+1.8 %) G | 3041 -> 3078 (+1.2 %) | yes | 3/3 |
| js/doctree-seq1 | 10.780 -> 10.782 (+0.0 %) G | 5.914 -> 5.787 (-2.2 %) G | 2095 -> 2058 (-1.8 %) | yes | 3/3 |
| js/graph-seq1 | 10.866 -> 10.867 (+0.0 %) G | 6.849 -> 6.800 (-0.7 %) G | 2575 -> 2572 (-0.1 %) | yes | 3/3 |
| js/join-seq1 | 10.238 -> 10.239 (+0.0 %) G | 5.858 -> 5.932 (+1.3 %) G | 2131 -> 2172 (+1.9 %) | yes | 3/3 |
| js/wordfreq-seq1 | 10.860 -> 10.859 (-0.0 %) G | 5.866 -> 5.898 (+0.5 %) G | 2099 -> 2082 (-0.8 %) | yes | 3/3 |
| js/probe-100k | 4.540 -> 4.542 (+0.0 %) G | 2.294 -> 2.277 (-0.7 %) G | 967 -> 960 (-0.7 %) | yes | 3/3 |

- **Instruction counts did not grow** anywhere: identical for protoCore's
  six, within -1.4 to +0.9 % for 29 of the 30 runtime programs.
  `py/richards_lite` executes 19 % fewer instructions (and 21 % fewer
  cycles): its objects set several attributes in a row in `__init__`,
  which protoPython now publishes as one write group.
- **Cycles moved within -9.5 to +4.3 %** with the instruction counts
  unchanged, the signature of code layout and of the shared machine
  (`py/int_sum_loop` -9.5 %, `scala/tak` +3.8 % on a 35 ms run).
- **One reproducible single-threaded regression:
  `immutable_sharing_benchmark` +8.5 % cycles** (+6.4 % task-clock), the
  same in all three rounds (2.96-2.99 G against 3.16-3.28 G cycles), with
  identical instructions.  A second `perf stat` with cache counters gave
  +1.4 % last-level misses, +1.6 % L1 data misses, +8 % data-TLB misses
  (±5 %) and the same page faults: no change in memory traffic large enough
  to explain it.  The cause is not established (layout or frequency are
  the candidates); it is reported, not explained away.
  `mutable_access_benchmark` +3.1 % and `object_access_benchmark` +1.1 %
  cycles are at the size the collector-throughput report attributed to
  layout.

## 5. Cells per operation (write coalescing)

Cells allocated per operation, from the peak resident set of two runs of
the same loop (100,000 and 400,000 operations) with no collection (the
instrumented library prints a line per cycle; a run with a cycle is
rejected): (RSS(400 k) - RSS(100 k)) / 300,000 / 64 bytes.  This counts
everything the loop allocates (the call's context, the loop), not only the
object; the runtimes' own unit tests count the object alone, so their
figures are smaller.  The "groups off" column is the after binary with its
runtime's switch (`PROTOJS_PUTFIELD_GROUPS`, `PROTOPY_ATTR_GROUPS`,
`PROTOST_IVAR_GROUPS`, `PROTOSCALA_FIELD_GROUPS` = `off`).  Medians of 3.

| Case | Cells per operation, before | after | change | after, write groups off | runs ok (b/a/off) |
|---|---|---|---|---|---|
| js/ctor5 | 125.5 | 39.0 | -69.0 % | 125.5 | 3/3/3 |
| js/move2 | 13.6 | 6.6 | -51.4 % | 13.6 | 3/3/3 |
| py/init5 | 252.2 | 188.4 | -25.3 % | 252.2 | 3/3/3 |
| st/setId5 | 88.6 | 32.1 | -63.7 % | 94.7 | 3/3/3 |
| scala/case5 | 32.0 | 11.0 | -65.6 % | 32.0 | 3/3/3 |
| clj/assoc5 | 81.6 | 81.6 | +0.0 % | - | 3/3/0 |

- **Write coalescing cuts the allocation of the cases it targets by
  25-69 %**: a five-field JavaScript constructor 125.5 -> 39.0 cells per
  construction, a two-field update method 13.6 -> 6.6, a five-attribute
  Python `__init__` 252 -> 188, a five-variable Smalltalk initializer
  88.6 -> 32.1, a five-field Scala case class 32.0 -> 11.0.
- **With the groups switched off, the after build allocates exactly what
  the before build did** (protoJS, protoPython, protoScala to the tenth of
  a cell; protoST within its run-to-run spread, 84-97 per write on either
  build, a figure that moves with the heap's growth pattern).  So protoCore
  2.11-2.14 changed nothing on these paths; the whole difference is the
  write groups.
- protoClojure is the control: no write groups (its state changes go
  through atoms, vars and persistent collections), 81.6 cells per
  five-key `assoc` on both builds.
- These counts are hardware-independent (they count cells, not time).

## 6. Regressions

Reported plainly; none of them made a verified run fail.

1. **`clj_coll_t1` (protoClojure, one task, 2 M-cell limit): +25-27 % wall
   time**, +38 % process CPU, 66 -> 92 cycles, although its waits fell from
   27.5 % to 0.  A diagnostic run with 2.14.1's mechanisms switched off one
   at a time (instrumented library, interleaved, medians of 3):

   | Variant | Wall s | Cycles | Sweep ns/cell | Process CPU s | runs |
   |---|---|---|---|---|---|
   | before | 11.07 | 66 | 40.6 | 13.2 | 3 |
   | after | 13.89 | 92 | 32.5 | 18.2 | 6 |
   | after-no-pacing | 18.78 | 66 | 21.0 | 22.7 | 3 |
   | after-no-helpers | 13.95 | 93 | 32.1 | 18.2 | 3 |
   | after-1-chain-no-prefetch | 11.21 | 132 | 66.2 | 20.0 | 3 |
   | after-8-chains-no-prefetch | 13.45 | 104 | 44.6 | 19.3 | 3 |
   | after-1-chain-prefetch | 11.01 | 131 | 63.0 | 19.4 | 3 |

   The cause is the **multi-cursor sweep (8 chains) running beside a single
   mutator**: with one chain the run is as fast as before (11.0-11.2 s),
   with or without prefetch, although each swept cell costs twice as much
   (63-66 against 32 ns) and there are more cycles; with 8 chains it is
   13.5-13.9 s with or without prefetch.  The helpers are not involved
   (they engage only while mutators wait), and pacing helps (without it,
   18.8 s).  The likely mechanism is interference in the memory system (8
   concurrent miss streams against the one mutator thread, on a 4 MB L3
   per core complex and two memory channels); it was not measured with
   counters.  This is a hardware-sensitive effect and the chain count is
   already configurable (`PROTOCORE_GC_SWEEP_CURSORS`); the default is not
   changed here.  Open item: engage the multi-cursor walk only when it
   pays (as the helpers are engaged), and re-measure on other hardware.
2. **6 threads with a 40 M-cell limit, where the collector already kept up:
   +1 to +6 % wall** (`js40_doctree_n6` +5.9 %, `js40_wordfreq_n6` +1.3 %;
   protoJS's structure benchmark `doctree` with 6 Deferreds +13.5 %, one
   process).  Pacing doubles the cycles (2 -> 4), collector CPU rises 40-50 %
   and process CPU 10-12 %, on six cores already running six mutators and a
   desktop.
3. **Peak RSS under the controller**: `jsad_wordfreq_n12` +53 %,
   `jsad_records_n12` +23 %, the core benchmark +64 % (2.13.0's law trades
   memory within the budget for fewer waits; the wall time fell 43-53 % on
   the same rows).
4. **`immutable_sharing_benchmark`: +8.5 % cycles single-threaded**, with
   identical instructions and nearly identical miss counts; not explained
   (section 4).
5. **protoST `fib.st` under the controller: 1.61 s against 0.47 s with
   protoST's fixed limit and 0.76 s with 2.10.2's law** (section 2).  The
   controller is not protoST's default, so no shipped behaviour regressed;
   it matters for the adoption decision.
6. **More marking**: pacing's extra cycles add mark time (CAD 59 -> 84 s,
   `js40_graph_n12` 15 -> 28 s); on every workload where the sweep was the
   bottleneck the net effect is still a large gain.

## 7. The controller: recommendation per runtime

The controller (`PROTOCORE_ADAPTIVE_HEAP=1`, default budget 75 % of memory)
against each runtime's current default, from sections 1 and 2.  The
decision is the maintainer's; this is the evidence, measured on one
notebook.

| Runtime | Default today | Evidence | Recommendation |
|---|---|---|---|
| protoScala | no limit: never collects | one task: +3.6 % time, -94 % memory; six tasks: the default was killed at 16 GB in 3 of 3 runs, the controller finished in 35 s and 0.45 GB; small benchmarks unchanged except `list_ops` +12 % (0.51 -> 0.57 s) | **adopt** |
| protoClojure | no limit: never collects | one task: +0.3 % time, -98 % memory; six tasks: default killed at 16 GB 3 of 3, controller 38 s and 0.34-1.7 GB; small benchmarks run no cycle | **adopt** |
| protoPython | no limit: never collects | no cycle and no change on non-allocating programs; 2-25 % faster and 39-73 % less memory on the allocating ones; no multi-threaded allocating Python workload was measured | **adopt** |
| protoJS | 75 % of memory | 12 Deferreds: -10 % time and -46 to -73 % memory on `records` and `wordfreq`, equal on `graph`; at a 40 M-cell budget equal to or faster than the same fixed limit (section 1, `jsad_*` against `js40_*` after); 6 Deferreds `records`: -18 %; **sequential: +13 to +36 % time** on four of five structure workloads, for 77-96 % less memory | **adopt for programs that use Deferreds and for long-running processes**; for sequential batch scripts it is a trade of 13-36 % time for most of the memory, the maintainer's call |
| protoST | fixed 640 MB | actor benchmarks: -69 to -72 % memory at 0 to +11 % time; `fib.st` 3.4 times slower (the initial soft limit, 2 M cells, sits just above protoST's start-up live set of about 1.7 M cells, and the process waits for a cycle at exit) | **not yet**: first measure it with an initial soft limit above the start-up live set (`initialSoftCells`, for example 4 M cells); keep the fixed limit until then |

The cost that remains under the controller is collector CPU beside the
mutators, mostly on sequential programs that the default policy never
collects; the 2.12-2.14 collector work made the parallel case, which the
calibration report found 75 % slower, faster than the default.

## 8. Hardware class

As in the collector-throughput report, each change is classed by whether its
effect should carry over to other hardware.  The "measured here" column is
this report's evidence on DEV12; the last column is a hypothesis, not a
result.

| Change | Class | Measured here (synthetic, notebook) | Expected on a large server (not measured) |
|---|---|---|---|
| Write coalescing (`setAttributes`, 2.11.0; protoJS, protoPython, protoST, protoScala) | hardware-robust: removes allocation and instructions | cells per construction -25 to -69 %; `py/richards_lite` -19 % instructions | the same counts |
| Early wake (2.12.0) | hardware-robust: removes a 50 ms polling latency | regime-1 waits 15-22 % -> 3 % | the same benefit |
| Pacing (2.12.0) | robust in mechanism (r and C are measured), sensitive in cost | more cycles and mark time everywhere; repaid where the sweep was the bottleneck, a net cost at 6 threads on 6 cores (+1 to +6 %) and on `clj_coll_t1` (+26 %) | extra cycles cost less where cores are idle |
| F1-F3: batched segment recycling, one store per survivor, test before unmark (2.12.0) | hardware-robust: fewer locked writes to shared lines | not separable in this session | larger benefit across sockets |
| Multi-cursor sweep with prefetch (2.12.0) | **hardware-sensitive**: trades bandwidth for latency | sweep per cell 2-7x lower at 6-12 threads, also on aged (recycled) heaps | more memory channels: likely more useful; across NUMA nodes overlap matters more |
| Sweep helper threads, K = 3 here (2.14.0) | **hardware-sensitive**: helpers share one memory system | included in the after column; not isolated in this session (the collector-throughput report isolated it: sweep per cell 2.3-2.6x lower, wall -6 to -18 % at N = 12) | more channels: more helpers may scale; the default keeps them within half of one node |
| Control law (2.13.0) | robust in form: no fitted constant | under the controller at N = 12: waits 1.6-8x lower than 2.10.2's, wall -27 to -53 %, peak RSS up to +53 % | the same behaviour; the regime boundary moves with the collector's throughput |
| Thread-exit quorum fix (2.14.1) | correctness | no effect expected or seen | none |

Which figures in this report are hardware-independent: cells per
operation, instruction counts, checksums, numbers of cycles to a first
approximation.  Which are not: every wall time and wait share at 6-12
threads (cores, memory channels, the desktop's background load), sweep
nanoseconds per cell (memory latency), and single-threaded cycle counts
(layout, frequency).

## What this does not show

- Server-class hardware (many cores, large L3, six to twelve memory
  channels, several NUMA nodes).  Nothing here was measured there.
- Heaps aged for hours: the aged windows above are minutes long.
- Real applications.  The workloads are synthetic, chosen to exercise the
  collector and the write path.
- The full 200,000-part CAD model, and the Node.js side of the structure
  benchmarks (it was run only as the checksum reference).
- protoPython and protoST under the collector-heavy workloads of section 1:
  the phase report had none for them; their evidence is section 2.
- The helper threads (2.14.0) in isolation: the after column includes them
  with the default K = 3; the collector-throughput report measured them
  alone.
