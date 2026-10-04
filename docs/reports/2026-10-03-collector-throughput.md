# Collector throughput: pacing, early wake and a faster sweep

Date: 2026-10-03.  protoCore 2.11.0 (master `69b56afe`) against the
implementation of
[the collector-throughput design](../specs/2026-10-03-collector-throughput-design.md)
(approved 2026-10-03; "the spec" below).  Author: Gustavo Marino, with
Claude.

This report covers milestones M1 (pacing and early wake, spec 4.3-4.4) and
M2 (diagnosis of the sweep's per-cell cost and the cheap fixes, spec 5),
released together as 2.12.0, and M3 (the control law, spec 4.5), released
as 2.13.0, and M4 (the sweep's helper threads, spec 6), released as
2.14.0.

**Every workload here is synthetic**: benchmarks written for this platform
(the phase report's set), used as evidence about mechanisms.  Nothing is
fitted to them.  Numbers are medians of 3 runs on one machine (AMD Ryzen,
Zen 2, 6 cores / 12 threads, 62 GB), which other agents shared during the
measurements: each run started only when no compiler or test suite was
running, but a job could start during a run.  Run-to-run spread is in the
raw records.

## Verdict

**Across the four milestones** (synthetic workloads, this notebook-class
machine; details in each section):

- 2.11.0 -> 2.14.0 on the aged runs (the heap cycled many times, all
  allocation from recycled memory): protoJS `records` N = 12 at 10 M cells
  71.0 -> 31.0 s, protoClojure `coll_alloc` with 6 tasks 117.1 -> 46.4 s.
  Almost all of it is 2.12.0's multi-cursor sweep (the per-cell cost was
  memory latency); the helpers of 2.14.0 add 6-18 % on the protoJS N = 12
  workloads and little on the protoClojure and protoScala t6 ones.
- Waits no longer end on a 50 ms watchdog, and fixed limits pace their
  cycles (2.12.0).  The controller minimises waits within the budget with no
  fitted constant (2.13.0): waits fall to a third, wall time does not move,
  the time goes to collector CPU.
- The hardware-sensitive parts (the walk's width and prefetch, the helper
  count and engagement) are configurable; their defaults are measured on one
  CPU only (section "Hardware class").

**M1 and M2 (2.12.0):**

- **The sweep's per-cell cost under concurrent allocation was memory
  latency, not lock or CAS contention.**  The collector thread's demand
  fills per swept cell, protoClojure `coll_alloc` with 6 tasks: 1.11 from
  DRAM and 0.29 from other cores' caches, at an IPC of 0.08 (426 cycles per
  cell).  One dependent miss per cell, one miss in flight.
- **The multi-cursor sweep (F4) removes most of it.**  Walking 8 segment
  chains in lockstep with prefetch: 139 -> 46 ns per swept cell on
  `clj_coll_t6`, 84 -> 29 on protoJS `records` N = 12 (10 M cells),
  157 -> 91 on protoScala `tree_alloc` t6.  The collector's demand DRAM
  fills fell from 1.11 to 0.12 per cell and its IPC rose from 0.08 to 0.37.
  F1-F3 (batched segment recycling, one write per survivor, test before
  unmark) together gave 139 -> 122 and 84 -> 80.
- **Wall time** (2.11.0 -> 2.12.0, fixed limits): `clj_coll_t6` 40.1 ->
  15.0 s (-63 %), `js10_records_n12` 19.5 -> 11.2 s (-43 %),
  `scala_tree_t6` 26.0 -> 19.2 s (-26 %), `js40_records_n6` 15.7 -> 12.5 s
  (-20 %), `js40_graph_n6` 18.8 -> 15.7 s (-16 %), the single-threaded
  640 MB core benchmark 3.49 -> 2.66 s (-24 %).  `js40_wordfreq_n12` did
  not improve (13.9 -> 14.5 s): its waits fell (17.6 -> 7.9 %) but it ran
  more cycles (5 -> 8) and its collector was busier.
- **Under the adaptive controller** (12 threads, H = 40 M cells):
  `jsad_records` 44.4 -> 23.6 s, `jsad_wordfreq` 24.9 -> 15.6 s; the
  single-threaded adaptive benchmark 4.24 -> 2.66 s.  Peak RSS rose on
  `jsad_wordfreq` (1.86 -> 2.58 GB) and `jsad_records` (2.11 -> 2.58 GB).
- **Waits no longer end on the watchdog.**  The mean headroom wait was
  48.6-50.2 ms on every multi-threaded baseline run (the 50 ms watchdog);
  with M1 alone it is 5-19 ms, with 2.12.0 10-46 ms (fewer, longer waits:
  the remaining ones span a mark), and with 2.12.0 the wait share fell on
  every multi-threaded workload, for example 64.6 -> 14.3 % (`clj_coll_t6`),
  21.4 -> 3.0 % (`js40_records_n6`, regime 1: it waited only because the
  cycle started at the ceiling).
- **A regression found and fixed before release**: with the early wake,
  the controller's soft-zone checkpoint returned at once while its pending
  flag stayed set, and every critical-section entry took `globalMutex`:
  about 2 million empty waits per run, the adaptive benchmark 3x slower
  (4.2 -> 14.5 s).  A publication of cells now clears the flag.
- **Single-threaded micro-benchmarks**: identical instruction counts;
  cycles within -1.2 % to +1.8 % on four of six; `hash_quality` +3.5 to
  +6.8 % on a 150 ms run whose hot code (`getAttribute`, `isInteger`) did
  not change (code layout); `immutable_sharing` +3.5 % in the first run,
  -1 % in two reruns.
- **The gate of spec 5.3** (per-cell cost at N = 12 within 1.5x of
  N = 1 after F1 + F4) is **not met**: 46 against 23 ns (`clj_coll`, 2.0x),
  91 against 30 ns (`scala_tree`, 3.0x).  The parallel sweep (M4) goes
  ahead.

## Method

**Builds.**  Release with `-DPROTOCORE_GC_INSTRUMENT=ON`, the compiled-out
instrumentation of branch `measure/gc-phases` (now merged), one library per
step:

| Label | What |
|---|---|
| base, base2 | master before the work (2.10.2 / 2.11.0 sweep, identical collector) plus the instrumentation; base2 re-ran the four N = 12 protoJS workloads whose records base had truncated (below) |
| m1 | + pacing and early wake |
| f123 | + F1, F2, F3 |
| m2 | + F4 (multi-cursor sweep) |
| m2b | + the controller fix; the 2.12.0 code.  It differs from m2 only on the controller's soft-zone path and the collector thread's name, so the fixed-limit rows of m2 stand for 2.12.0 |

**Runtimes.**  The installed Release `protojs`, `protoscala` and `protoclj`
(SOVERSION 3), run with `LD_LIBRARY_PATH` set to each library; `ldd` showed
each resolving `libprotoCore.so.3` to it, and every collecting run printed
the instrumented library's `[GC-PHASES]` line.

**Runs.**  `runner.py` (data directory), sequential, each in
`systemd-run --user --scope -p MemoryMax=<cap> -p MemorySwapMax=0` under
`/usr/bin/time`.  Every run verifies itself: protoJS prints `"ok":true` and
per-task checksums, which must be identical in every run of a workload
(they were); protoScala `checksum=540451801`; protoClojure the closed-form
checksum; the core benchmark `verified=yes`.  The first baseline pass kept
only the last 400 characters of standard output, which cut the JSON line of
the N = 12 protoJS runs, so those rows are marked unverified and were run
again (base2) with the whole line kept.

Workloads (the phase report's, scaled down to keep the matrix short):
`core_fixed640_live1M` / `core_adaptive_live1M` (`adaptive_heap_benchmark`,
1 thread); protoJS `structures` `records`, `graph`, `wordfreq` with N
Deferreds at 40 M or 10 M cells, and under the controller (`jsad`, H = 40 M);
protoScala `tree_alloc` with 30 batches; protoClojure `coll_alloc` with 500
rounds (2 M-cell limit for both).

## M1: pacing and early wake

What changed (spec 4.3-4.4): a refill requests a cycle once the cells left
before the ceiling fall below `min(headroom, r x C x 1.25)`; a wait for
headroom ends when cells are published, not on the watchdog.

| Workload | Wall s base -> m1 -> 2.12.0 | Wait share | Waits | Mean wait ms | Cycles |
|---|---|---|---|---|---|
| core_fixed640_live1M (1 thread) | 3.49 -> 3.28 -> 2.66 | 35.9 -> 28.2 -> 5.4 % | 25 -> 25 -> 11 | 50.1 -> 36.9 -> 13.3 | 7 -> 10 -> 11 |
| js40_records_n6 | 15.65 -> 13.95 -> 12.50 | 21.4 -> 13.7 -> 3.0 % | 398 -> 908 -> 48 | 50.1 -> 12.7 -> 46.2 | 5 -> 6 -> 9 |
| js40_graph_n6 | 18.81 -> 17.71 -> 15.74 | 19.4 -> 11.8 -> 3.3 % | 436 -> 737 -> 72 | 50.1 -> 16.8 -> 45.1 | 7 -> 10 -> 13 |
| js40_wordfreq_n12 | 13.90 -> 13.14 -> 14.50 | 17.6 -> 11.0 -> 7.9 % | 584 -> 921 -> 406 | 50.2 -> 18.9 -> 33.9 | 5 -> 6 -> 8 |
| js10_records_n12 | 19.46 -> 18.70 -> 11.19 | 56.6 -> 67.0 -> 30.1 % | 2697 -> 9431 -> 1014 | 50.2 -> 16.1 -> 40.0 | 15 -> 15 -> 16 |
| scala_tree_t6 | 26.01 -> 22.73 -> 19.18 | 44.6 -> 47.1 -> 35.2 % | 1379 -> 7180 -> 1210 | 50.1 -> 9.2 -> 33.5 | 59 -> 60 -> 78 |
| clj_coll_t6 | 40.11 -> 33.78 -> 14.98 | 64.6 -> 68.0 -> 14.3 % | 3104 -> 28897 -> 923 | 50.1 -> 4.8 -> 14.1 | 120 -> 115 -> 140 |

- The watchdog no longer sets the wait: every baseline mean is 50 ms; after
  M1 the mean is the time until cells arrive.
- On the regime-1 rows (N = 6, 40 M cells: the collector keeps up) the
  wait share fell from 19-21 % to 3 %: the cycle now starts before the
  ceiling.  It does not reach zero: the first cycle still starts at the
  ceiling (no measured r and C yet), and r x C is taken from the last two
  cycles.
- On the regime-2 rows (N = 12 at 10 M cells, the t6 runs) M1 alone moved
  little: the collector could not keep up, so the waits only became more
  numerous and shorter.  The sweep fix (M2) is what moved them.
- More cycles: pacing starts cycles with less garbage.  Each costs a mark
  of the live set; on `js40_wordfreq_n12` (5 -> 8 cycles) that cost ate the
  gain.

**Wake reasons**, from the deterministic tests (test/CollectorPacingTests.cpp,
Release, one thread, 600,000-cell fixed limit): with pacing, 1 wait in
6 M allocated objects, ended by a publication of cells, 20 cycles of which
20 were requested by pacing; with `PROTOCORE_GC_PACING=0`, 7 waits, all
ended by a cycle, none by cells, none paced.

## M2: why the sweep slows down, and the fixes

### Differential micro-benchmark

`performance/sweep_contention_benchmark` (new; spec 5.2): 3 M objects
(6 M cells) of garbage in segments of about 6 cells, one cycle per
scenario, the sweep's ns per swept cell from the collector's own measures.
Median of 3 (raw: `sweep-contention-m2.log`).

| Scenario | m1 | + F1-F3 | + F4 |
|---|---:|---:|---:|
| S0 nothing else running | 9.0 | 9.0 | 10.8 |
| S1 6 threads streaming private memory (loaded DRAM) | 38.3 | 37.3 | 69.0 (36.7-74.3) |
| S2 6 threads submitting and destroying contexts (segment pool) | 19.9 | 10.3 | 12.2 |
| S3b survivors in the candidate set | 22.4 | 21.6 | 18.3 |
| S3 the same, 6 threads reading the survivors | 30.6 | 31.9 | 22.9 |
| S4 garbage built by 6 other threads | 20.2 | 19.2 | 16.3 |
| S5 fresh memory | 9.2 | 9.7 | 11.1 |

- Loaded DRAM (S1) quadruples the cost; contention on the segment pool (S2)
  doubles it, and F1 removes that; other cores' caches (S4) and readers of
  the survivors (S3 - S3b) add about 10 ns.
- F4 costs about 2 ns per cell where every line is already near (S0, S5:
  freshly built garbage in a quiet machine) and helps where lines are far
  (S3, S4).  Under saturated bandwidth (S1) it is erratic: 36.7, 69.0 and
  74.3 ns in three repetitions; prefetches add traffic to a memory system
  six streaming threads already saturate.
- The micro-benchmark over-weights S2: its threads do nothing but destroy
  contexts.  The runtime workloads below say which effect matters there.

### Collector-thread counters

`perf stat -p <pid> --per-thread` on `clj_coll_t6` (500 rounds), the
collector thread only.  AMD Zen 2 `ls_refills_from_sys` counts demand fills
by source (`lcl_l2`: this core's L2; `lcl_cache`: another core's cache;
`lcl_dram`: DRAM).  Per cell visited (swept plus marked):

| Build | Cycles (G) | IPC | Cycles / cell | DRAM fills / cell | Other-core fills / cell | Sweep s |
|---|---:|---:|---:|---:|---:|---:|
| base | 113.1 | 0.08 | 426 | 1.11 | 0.29 | 33.7 |
| + F1-F3 | 106.2 | 0.09 | 401 | 1.05 | 0.23 | 32.6 |
| + F4 (2.12.0) | 37.8 | 0.37 | 141 | 0.12 | 0.03 | 12.0 |

- Before: about one DRAM miss per cell, serialized (IPC 0.08), plus a
  cross-core transfer every three or four cells.  Hypotheses H1 (coherence)
  and H2 (loaded DRAM latency) of spec 5.2 both act; both are latencies of
  dependent loads.
- After F4 the demand fills drop tenfold: the lines arrive through the
  software prefetches, several in flight.
- `perf c2c record` ran (IBS on this kernel) but recorded no sample for the
  process at `perf_event_paranoid=1`; the counters above and the
  micro-benchmark stand in for it, as the spec allows.

### Runtime workloads, per fix

| Workload | Sweep ns / cell base -> F1-F3 -> F4 | Wall s base -> F1-F3 -> F4 | Collector busy s |
|---|---|---|---|
| clj_coll_t6 | 139.0 -> 122.3 -> 46.3 | 40.1 -> 32.9 -> 15.0 | 37.1 -> 32.7 -> 13.2 |
| js10_records_n12 | 83.9 -> 80.2 -> 29.2 | 19.5 -> 18.6 -> 11.2 | 17.2 -> 16.8 -> 9.1 |

F1-F3 are measured together (one commit), not one by one.  F2 and F3 save a
locked instruction per survivor and are not separable by these numbers; F1
is what moved S2.

### Single-threaded workloads

| Workload | Wall s base -> 2.12.0 | Sweep ns / cell |
|---|---|---|
| core_fixed640_live1M | 3.49 -> 2.66 | 18.7 -> 10.4 |
| core_adaptive_live1M | 4.24 -> 2.66 | 24.2 -> 10.9 |
| scala_tree_t1 | 4.52 -> 4.31 | 47.1 -> 30.4 |
| clj_coll_t1 | 3.27 -> 3.46 | 24.7 -> 23.3 |

`clj_coll_t1` ran 30 cycles against 22 (pacing) for +6 % wall time with a
slightly cheaper sweep; its waits fell from 28.7 % to 0.1 % of the run, so
the time went to the extra cycles' marks and to the collector's CPU next to
the mutator.  It is one run of three per side on a shared machine; read it as
"no gain" rather than as a measured regression.

`perf stat -r 3`, the six single-threaded protoCore benchmarks (no heap
limit, no collection), 2.11.0 against 2.12.0 (`single-thread/`):

| Benchmark | instructions:u | cycles:u |
|---|---|---|
| microbenchmark_final | 7.97 G = | +0.0 % |
| mutable_access_benchmark | 16.2 G = | -0.3 % |
| cache_timing_benchmark | 4.31 G = | +1.8 % |
| hash_quality_benchmark | 1.60 G = | +6.8 % (reruns: +3.5 %, +6.6 %) |
| object_access_benchmark | 60.2 G = | -1.2 % |
| immutable_sharing_benchmark | 4.57 G = | +3.5 % (reruns: -0.8 %, -1.1 %) |

The instruction counts are identical: these benchmarks never allocate past
a refill that 2.12.0 changed.  `hash_quality`'s profile is
`getAttribute` (56-61 %) and `isInteger` (12-20 %), neither touched; the
difference moves between them from build to build, the signature of code
layout, not of work.  It is reported, not explained away: a 3-7 % cycle
difference on a 150 ms run.

### The controller fix

| Workload | Wall s base -> m2 -> m2b | Waits | Peak RSS MB |
|---|---|---|---|
| core_adaptive_live1M | 4.24 -> 13.60 -> 2.66 | 53 -> 1,907,169 -> 25 | 575 -> 591 -> 531 |
| jsad_wordfreq_n12 | 24.91 -> 18.95 -> 15.62 | 3,447 -> 358,881 -> 1,200 | 1860 -> 1877 -> 2580 |
| jsad_records_n12 | 44.44 -> 26.89 -> 23.60 | 6,921 -> 718,772 -> 1,526 | 2110 -> 2560 -> 2578 |

## M3: the control law (2.13.0)

What changed (spec 4.5 and decision 1): the controller minimises the
mutators' wait within the budget H, from measurements only.  While they
wait and the collector keeps up (rho = r / T < 1), S becomes
`L + rho L / (1 - rho) x 1.25`; while they wait and it does not, S
doubles as long as each doubling reduces the waits, and stops after two
that do not.  `k_live`, `k_cap` and `p_high` are gone.

**Two corrections found by measuring, before release.**
- The first cycle under the controller is not evidence about S (it starts
  with no measured runway, and its interval includes the program's
  start-up): the law starts at the second.  Without it, one early wait on a
  steady 300,000-cell workload sent S to 12.5 M cells.
- A probe across a workload change does not count.  On protoJS `records`
  N = 12 the first probes fell where the program went from its build to
  twelve parallel tasks (r x 5); the waits rose with the load, both probes
  counted as non-improving, and S sat at 8 M cells for 12 cycles with a
  third of the mutators' time waiting.  A verdict is now void when L or r
  moved by the re-arm factor (2) between probe and verdict; the same run
  then reaches H at cycle 14 instead of 23.

**Under the controller, 2.12.0 (2.10.1 law) -> 2.13.0** (median of 3):

| Workload | Wall s | Peak RSS MB | Wait share | Cycles |
|---|---|---|---|---|
| core_adaptive_live1M (1 thread) | 2.66 -> 2.67 | 531 -> 858 | 26.2 -> 4.2 % | 19 -> 15 |
| jsad_wordfreq_n12 | 15.62 -> 16.71 | 2580 -> 2761 | 24.3 -> 8.6 % | 22 -> 18 |
| jsad_records_n12 | 23.60 -> 24.17 | 2578 -> 2567 | 21.7 -> 8.5 % | 23 -> 22 |

The new law does what its objective says -- the waits fall to a third --
and does not make the programs faster: the wall time is within +0.4 % to
+7 %.  The time the mutators no longer wait went to the collector's own
CPU next to them, and to more memory (the single-threaded benchmark keeps
858 MB instead of 531).  The objective counts waits only (decision 3); this
is the measurement that question asked for.

### The memory-as-only-variable experiment

Spec section 9: each workload under fixed limits of 10, 20, 40, 100 and
200 M cells, and under the controller with H equal to that limit; same
binaries, same library (2.13.0 code; fixed limits do not reach the law).
**One run per point** (the spec asks for a median of 3; the matrix had to
fit beside other agents' jobs), so differences under about 10 % are noise.
The 400 M-cell column was not run: it needs a 26 GB cap and the shared
machine had 24 GB available.  Workloads as above, scaled: protoJS N = 12
(REPS = 1), protoScala 30 batches, protoClojure 500 rounds, the core
benchmark with 1 M live cells.

| Workload | Limit / H (M cells) | Fixed limit: wall s, peak RSS MB, cycles, wait share | Controller 2.13.0 (H = the limit): wall s, peak RSS MB, cycles, wait share |
|---|---:|---|---|
| core | 10 | 2.84, 642, 12, 6.9 % | 2.56, 634, 17, 6.8 % |
| core | 20 | 3.02, 1242, 6, 3.0 % | 2.54, 907, 15, 4.6 % |
| core | 40 | 3.76, 2464, 2, 2.2 % | 2.34, 906, 14, 4.2 % |
| core | 100 | 4.40, 4458, 0, 0.0 % | 2.65, 859, 16, 5.3 % |
| core | 200 | 5.32, 4458, 0, 0.0 % | 2.69, 891, 14, 3.4 % |
| js_records | 10 | 12.32, 716, 16, 30.2 % | 11.39, 736, 23, 33.7 % |
| js_records | 20 | 9.16, 1362, 8, 15.1 % | 9.01, 1315, 16, 17.7 % |
| js_records | 40 | 8.54, 2578, 4, 5.7 % | 7.82, 2561, 13, 7.8 % |
| js_records | 100 | 8.65, 6284, 1, 10.1 % | 8.55, 3251, 13, 3.4 % |
| js_records | 200 | 8.80, 7886, 0, 0.0 % | 9.36, 3252, 13, 4.6 % |
| js_wordfreq | 10 | 7.12, 924, 10, 30.2 % | 7.99, 874, 15, 36.8 % |
| js_wordfreq | 20 | 5.49, 1413, 5, 2.5 % | 5.46, 1233, 12, 6.9 % |
| js_wordfreq | 40 | 5.19, 2621, 2, 4.1 % | 6.71, 1302, 13, 2.9 % |
| js_wordfreq | 100 | 4.75, 4112, 0, 0.0 % | 5.63, 1372, 12, 4.1 % |
| js_wordfreq | 200 | 4.59, 4148, 0, 0.0 % | 6.85, 1286, 13, 6.9 % |
| scala | 10 | 11.82, 718, 19, 0.1 % | 10.58, 552, 30, 0.4 % |
| scala | 20 | 10.56, 1420, 7, 0.2 % | 10.65, 943, 25, 1.4 % |
| scala | 40 | 9.48, 2797, 3, 0.4 % | 12.33, 566, 27, 1.5 % |
| scala | 100 | 8.60, 6693, 0, 0.0 % | 11.72, 862, 26, 1.1 % |
| scala | 200 | 8.59, 6693, 0, 0.0 % | 12.10, 587, 26, 0.7 % |
| clj | 10 | 14.86, 658, 41, 1.2 % | 12.48, 663, 49, 1.4 % |
| clj | 20 | 11.89, 1305, 23, 0.1 % | 12.58, 1107, 44, 1.4 % |
| clj | 40 | 10.79, 2600, 12, 0.1 % | 10.99, 1395, 40, 0.8 % |
| clj | 100 | 11.48, 6473, 4, 0.2 % | 11.31, 916, 45, 0.1 % |
| clj | 200 | 11.06, 12935, 1, 0.5 % | 12.76, 1433, 44, 0.1 % |

Reading, per workload (synthetic evidence, not acceptance):

- **Regimes under fixed limits.**  `js_records` and `js_wordfreq` wait
  30 % at 10 M cells and 0-10 % from 40 M: regime 1 above the knee, and at
  100-200 M the run's whole garbage nearly fits (0-1 cycles).  `scala` and
  `clj` wait at most 1.5 % at every limit since 2.12.0's sweep: regime 1
  everywhere at this scale (they were regime 2 in the phase report, before
  the multi-cursor sweep).
- **Memory.**  The controller holds 0.55-1.4 GB where the large fixed
  limits take 4-13 GB.  At the knee it is at or below the fixed limit's
  memory (`js_records` 40 M: 2.56 against 2.58 GB; `js_wordfreq` 20 M:
  1.23 against 1.41 GB).
- **Time.**  The controller is within noise of the fixed limit at and
  below the knee, and faster than every fixed limit on the core benchmark
  (2.3-2.7 s against 2.8-5.3 s: a large fixed heap is touched for the first
  time once per cell, which costs page faults the controller's reused heap
  does not pay).  It is **slower than the large fixed limits where those
  never collect**: `scala` 10.6-12.3 s against 8.6 s, `js_wordfreq`
  5.5-6.9 s against 4.6 s.  There the controller collects 12-30 times with
  waits of 1-7 %, because without waits it does not grow (decision 2: no
  speculative growth to the budget); the time is the collector's CPU beside
  six or twelve mutator threads on six cores.
- **P1** (no wait with spare capacity) is met to 1.5 % for `scala` and
  `clj` and to 3-7 % for the protoJS workloads, not to zero.
- **P6** (regime 2 does not spend memory for nothing): the probes grew S to
  H on `js_records` at 10 M, where the fixed curve says memory helps; no
  workload here showed two non-improving probes holding S below H after the
  fix above.
- An embedder whose run fits starts at the budget instead
  (`PROTOCORE_ADAPTIVE_HEAP_START=budget` or `initialSoftCells >= H`):
  that is the large-fixed-limit column, by construction.

## Hardware class

**Every figure in this report comes from one notebook-class CPU**: an AMD
Ryzen 5 5500U (Zen 2, "Lucienne"): 6 cores / 12 threads in two core
complexes, 4 MB of L3 per complex (8 MB in all), two DDR4 channels, one
NUMA node, 62 GB, a laptop power envelope.  A server differs in the
directions that matter here: many more cores, much more L3, six to twelve
memory channels (far more bandwidth and memory-level parallelism), and
several NUMA nodes, where a cache line written on one socket is expensive
to read on another.  The changes of this work fall into two classes:

| Change | Class | Why | Expected on a large server (hypotheses, not results) |
|---|---|---|---|
| Early wake (M1) | hardware-robust | removes a fixed 50 ms polling latency | same benefit |
| Pacing (M1) | hardware-robust | starts the cycle on time; its inputs (r, C) are measured on the machine | same benefit |
| F1 batched segment recycling | hardware-robust | removes compare-and-swaps on a line every mutator writes | larger benefit: more cores contend for that line, and across sockets each transfer costs more |
| F2 one store per survivor, F3 test before unmark | hardware-robust | remove locked writes to live lines | larger benefit across sockets (fewer invalidations of readers' copies) |
| The control law (M3) | hardware-robust in form | its inputs are measured; no constant fitted to this machine | same behaviour; the regime boundary moves with T |
| F4 multi-cursor walk (8 chains) | **hardware-sensitive** | trades bandwidth for latency: several misses in flight | more channels and MLP: likely more useful, and more chains may pay; across NUMA nodes the misses are slower, so overlap matters more |
| F4 prefetch | **hardware-sensitive** | on a saturated two-channel memory (S1 above) it added traffic and was erratic | more bandwidth: less risk of saturation |
| Helper count (M4) | **hardware-sensitive** | sweepers share one memory system | more channels: more helpers may scale; across sockets a helper sweeping another node's cells pays remote latency; the default keeps helpers to half of one node's cores |
| Helper engagement (M4) | adapts by measurement | helpers are kept only while they shorten the sweep | the same rule decides there |

So every hardware-sensitive parameter is configurable (since 2.14.0;
environment and `ProtoSpace`'s static API): chains walked in lockstep,
prefetch, helper count and helper engagement; and the defaults are derived
conservatively from what is cheap to detect: physical cores and NUMA nodes
(helpers = half the cores of one node).  The L3 size is detected and
reported, not used: one machine does not show how it should be used.

**A second data point, from shared CI virtual machines** (workflow
`sweep-hardware.yml`; noisy, read as a direction only: the same cell moved by
up to 50 % between run 37143120002 and run 37156579508), 2.14.0 code (run
37156579508), `sweep_contention_benchmark 1000000 3 3`, ns per swept cell,
median of 3, helpers engaged on every sweep:

| Runner | Scenario | 1 chain, no prefetch | 8 chains + prefetch (K = 0) | + 1 helper | + 2 | + 3 |
|---|---|---:|---:|---:|---:|---:|
| Linux arm64 (Neoverse-N2, 4 vCPU) | S0 quiet | 8.6 | 7.0 | 4.7 | 3.4 | 2.7 |
| | S4 built by other threads | 23.6 | 16.2 | 9.1 | 6.3 | 5.1 |
| | S1 loaded memory | 10.1 | 7.8 | 6.9 | 6.8 | 5.8 |
| Linux x64 (EPYC 7763, 4 vCPU) | S0 | 6.2 | 6.4 | 4.9 | 4.6 | 4.0 |
| | S4 | 25.6 | 11.2 | 7.9 | 7.1 | 6.4 |
| | S1 | 18.2 | 18.6 | 15.8 | 11.9 | 11.7 |
| macOS arm64 (3 vCPU) | S0 | 5.7 | 8.3 | 5.8 | 6.5 | 4.1 |
| | S4 | 25.9 | 12.4 | 11.8 | 8.4 | 10.2 |
| | S1 | 12.8 | 12.1 | 11.9 | 19.4 | 16.4 |

- The multi-cursor walk helps wherever lines are far (S4: 1.5-2.3x on every
  runner; S3 similarly) and is neutral or negative where they are near
  (S0: +45 % on the macOS VM in this run, -19 % on arm64 Linux).
- Helpers scale on the arm64 Linux VM (S0 7.0 -> 2.7 with three) and the
  x64 VM, and are erratic on the 3-vCPU macOS VM, where three helpers plus
  three mutator threads oversubscribe the machine (S1 12.1 -> 16.4 with
  three, worse with two).  The default engagement (measured) exists for this
  case; the micro-benchmark engages helpers on every sweep, so it shows the
  raw effect.
- The deterministic tests (sweep, pacing, the law) passed on all three.

## M4: the sweep's helper threads (2.14.0)

What changed (spec 6 and decisions 5-6): while mutators wait for headroom,
up to K helper threads (default: half the physical cores of one NUMA node,
3 here) sweep beside the collector, claiming runs of 128 segments; embedder
finalizers stay on the collector thread; helpers are kept only while they
shorten the sweep (measured engagement); K = 0 is the serial sweep.

**Two regressions found and fixed before release, both with K = 0**, by
measuring in the same session against 2.12.0's sweep: (1) the collector
called the multi-cursor walk once per claimed run of 128 segments, so its 8
chains drained to one at the end of every run (`clj_coll_t6` 79 ns per cell
against 46); the cursors now refill across runs.  (2) A claim walks 128
segment links in a row under the claim lock, a dependent miss each (4.5 % of
a protoClojure run); until a helper joins, the collector now takes segments
straight from the list.  ThreadSanitizer then found a read of the list's
head for a prefetch after helpers had joined (harmless on the hardware, a
data race in the C++ model): fixed.

### Runtime workloads, K = 0 against K = 3

One session, interleaved run by run (2.11.0's collector, 2.12.0's sweep,
2.14.0 with K = 0 and with K = 3), median of 3, synthetic.  The machine was
shared and loaded (load average 3-6 from other work during these runs); each
run started below a load of 6.

| Workload | Wall s: 2.11.0 / 2.12.0 / 2.14.0 K=0 / K=3 | Sweep ns per cell | Wait share | Process CPU s (user+sys), K=0 -> K=3 |
|---|---|---|---|---|
| js10_records_n12 | - / 10.93 / 10.96 / 10.23 | - / 29.0 / 29.2 / 12.5 | 31 / 32 / 19 % | 57.2 -> 60.2 |
| js40_wordfreq_n12 | - / 14.57 / 15.15 / 12.39 | - / 20.7 / 20.7 / 15.9 | 8.5 / 7.4 / 8.1 % | 95.5 -> 93.7 |
| jsad_records_n12 (controller) | - / 22.99 / 22.43 / 20.91 | - / 28.6 / 27.0 / 19.4 | 22 / 15 / 8 % | 156.5 -> 157.2 |
| jsad_wordfreq_n12 (controller) | - / 15.68 / 13.95 / 12.80 | - / 27.8 / 22.0 / 18.6 | 21 / 10 / 7 % | 91.8 -> 91.5 |
| clj_coll_t6 | - / 14.74 / 15.19 / 14.60 | - / 45.5 / 47.0 / 43.2 | 15 / 18 / 10 % | 84.4 -> 93.6 |
| scala_tree_t6 | - / - / 18.65 / 19.76 | - / - / 93.7 / 95.9 | 35 / 37 % | 80.5 -> 85.6 |
| aged_js10_records_n12 | 71.02 / 33.50 / 32.80 / 30.99 | 118.3 / 36.4 / 36.7 / 14.1 | 69 / 35 / 34 / 22 % | 189.8 -> 202.2 |
| aged_clj_coll_t6 | 117.13 / 46.04 / 47.13 / 46.38 | 135.7 / 47.4 / 48.7 / 46.2 | 67 / 17 / 18 / 11 % | 263.5 -> 291.1 |
| single-threaded (core, scala t1, clj t1) | - / - / 2.37-3.91 / 2.40-3.91 | about 10-31, unchanged | < 5 % | unchanged |

- **K = 0 is 2.12.0's sweep** within noise (-2 to +4 % wall; the
  controller rows differ because 2.13.0's law is in both 2.14.0 columns).
- **Helpers cut the sweep's cost per cell 2.3-2.6x where the sweep is the
  bottleneck and lines are far** (`js10_records`, aged or not), and the
  wall time 6-18 % on the protoJS N = 12 workloads.  The wall gain is far
  smaller than the sweep gain because the collector is no longer the only
  limit: mark, Phase 5b and the mutators' own work remain.
- **They hardly help the protoClojure and protoScala t6 workloads**
  (-4 % and +6 % wall; sweep per cell -8 % and +2 %).  These run 6 mutator
  threads on 6 cores with many short cycles of 2 M cells; the helpers took
  CPU (process CPU +10 % on `clj_coll_t6`, +6 % on `scala_tree_t6`) and the
  measured engagement kept them (helper runs: 309 in 155 cycles for clj).
  Whether the engagement rule should weigh the mutators' CPU is the
  question of decision 3, now with data: helpers cost -2 to +11 % process
  CPU for -18 to +6 % wall time.
- Peak RSS is unchanged.

### Micro-benchmark: K and the walk (one session)

`sweep_contention_benchmark 3000000 6 3`, release builds, ns per swept cell:

| Build | S0 | S1 | S2 | S3b | S3 | S4 | S5 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 2.12.0 (serial) | 10.9 | 66.7 | 12.4 | 17.9 | 23.6 | 15.7 | 11.1 |
| 2.14.0, K = 0 | 11.6 | 45.6 | 12.7 | 19.4 | 21.7 | 17.9 | 11.9 |
| 2.14.0, K = 3 (every sweep) | 6.5 | 25.2 | 10.1 | 11.3 | 15.5 | 10.4 | 6.1 |

Scaling over K (an earlier session of the same day, K = 0, 1, 2, 3, 5,
helpers on every sweep), S0: 13.4, 9.6, 8.2, 7.0, 6.1; S4: 20.5, 14.7,
12.6, 10.8, 9.2; S1: 45.2, 39.4, 27.3, 23.5, 24.3.  **P10 holds on this
machine**: no scenario got slower with helpers, up to K = 5.  (On the 3-vCPU
macOS VM above it did not hold for S1.)

Chains and prefetch (K = 0, 2.14.0, one session whose load rose from 2.6
to 7 midway; `micro-final-cursors.log`):

| Chains, prefetch | S0 | S1 | S3 | S4 |
|---|---:|---:|---:|---:|
| 1, off | 10.1 | 34.4 | 32.8 | 27.4 |
| 1, on | 13.5 | 43.3 | 37.9 | 29.2 |
| 2, off | 18.6 | 173.5 | 32.1 | 30.2 |
| 2, on | 11.2 | 32.6 | 23.5 | 21.1 |
| 4, off | 20.0 | 95.7 | 34.0 | 22.1 |
| 4, on | 13.0 | 35.7 | 21.5 | 17.3 |
| 8, off | 18.9 | 77.1 | 33.2 | 24.3 |
| 8, on (default) | 11.5 | 47.0 | 24.6 | 17.9 |
| 16, on | 13.5 | 53.7 | 36.1 | 23.7 |

Several chains without prefetch are worse than one chain (the bookkeeping
without the overlap); with prefetch, 2-8 chains are within the noise of
each other and 30-40 % below one chain where lines are far (S3, S4); 16 is
worse.  A single chain is best only where lines are near (S0) or memory is
saturated (S1 at 1 chain without prefetch: 34 ns).  8 is kept; on this CPU
4 would do as well.  The same scan on the pre-fix M4 code
(`micro-m4-cursors-prefix.log`) gave the same ordering.

### Fresh against recycled memory, and the aged heap

Cells handed out by origin (instrumented builds count them): from a fresh OS
block (contiguous) or from chunks the sweep published (recycled, in the
order the sweep met them).

| Workload | Fresh share of cells handed out (whole run) | Sweep ns per cell, whole run | Same, aged window | Fresh share in the aged window |
|---|---:|---:|---:|---:|
| js40_records_n6 (K = 0) | 22 % | 20.9 | 26.1 | 0 % |
| js10_records_n12 (K = 0 / 3) | 9 % | 29.2 / 12.5 | 32.1 / 13.5 | 0 % |
| jsad_records_n12 (K = 0 / 3) | 11 % | 27.0 / 19.4 | 31.0 / 22.2 | 0 % |
| scala_tree_t6 (K = 0) | 2 % | 93.7 | 97.2 | 0 % |
| clj_coll_t6 (K = 0 / 3) | 1 % | 47.0 / 43.2 | 47.7 / 43.8 | 0 % |
| aged_js10_records_n12 (K = 0 / 3) | 3 % | 36.7 / 14.1 | 38.2 / 14.5 | 0 % |
| aged_clj_coll_t6 (K = 0 / 3) | 0 % | 48.7 / 46.2 | 48.9 / 46.4 | 0 % |
| core_fixed640 (1 thread) | 14 % | 10.5 | n/a | n/a |

The aged window is the part of the run after the collector had freed three
times the heap limit (the heap cycled three times): the last `[GC-PHASES]`
counters minus those at that point.  The aged runs are the same workloads,
longer (protoJS REPS = 3, protoClojure 1,500 rounds).

What holds, and what is not established:

- **Established on recycled memory (aged heaps)**: every multi-threaded
  workload here allocates almost only from recycled chunks after its first
  heap turn (0 % fresh in the aged windows).  The per-cell sweep cost there
  is 2-25 % above the whole-run average (the young part of a run is
  cheaper), and the gains measured hold in the aged window: the multi-cursor
  sweep (2.11.0 -> 2.12.0: 118 -> 36 and 136 -> 47 ns per cell on the aged
  runs) and the helpers (36.7 -> 14.1 on aged protoJS records).
- **Established on mostly fresh memory only by the micro-benchmark**: S5
  (fresh) and S0 (recycled) cost the same there, but S0's recycled cells
  come from one quiet sweep of contiguously built garbage, which is close to
  fresh.  No runtime workload here ran mostly on fresh memory long enough
  to measure it (the highest fresh share is 22 %).
- **Not established**: how the cost evolves on a heap that has aged for
  hours (fragmentation of the recycled chunks across many cycles and
  threads); the longest aged window here is about 400 cycles of a 2 M-cell
  heap (protoClojure) and 40 of a 10 M-cell heap (protoJS).  Short runs on a
  young heap over-represent fresh memory, and the phase report's and this
  report's short runs should be read with that bias.

### The memory matrix with helpers

The fixed-limit columns of the memory-only experiment again with K = 3 (one
run per point, a different session from the K = 0 matrix, so compare only
large differences): at 10 M cells, `js_records` 12.3 -> 10.8 s (waits
30 -> 18 %); elsewhere within the noise of one run.
`matrix-helpers-2.14.0.jsonl`.

## Collector CPU (decision 3: measure first)

User plus system CPU of the whole process, N = 12 / t6, base -> 2.12.0:
`clj_coll_t6` 112.8 -> 86.5 s; `js10_records_n12` 65.3 -> 59.7 s;
`scala_tree_t6` 98.0 -> 83.5 s; `jsad_records_n12` 152.8 -> 153.6 s;
`js40_wordfreq_n12` 91.1 -> 98.1 s.  The collector thread's own CPU
(`cpu_busy`) fell on all but `js40_wordfreq_n12` (7.5 -> 10.4 s, the extra
cycles).  No decision is taken here; M4's helpers are where the question
becomes sharp.

## What this does not show

- Any machine but this notebook-class CPU, except the noisy CI VM runs of
  "Hardware class".
- Heaps aged for hours (see "Fresh against recycled memory").
- Two informational, clock-dependent cases failed once each during this
  work on CI: `MPSCQueueGC.LargeDrainDoesNotBlockStopTheWorld` (macOS, run
  37137054874, a pause ratio; Windows, run 37158683120: no cycle landed
  during a drain) and, in a runtime's CI that runs this
  suite, `AdaptiveHeapStorm.SoftLimitSettlesWithinBoundedCycles` (2.12.0).
  Not investigated beyond their timing nature; the latter's replacement
  (2.13.0) is still clock-dependent and listed as such, and the gating
  fast-allocator case now asserts invariants only (2.14.0).
- A ThreadSanitizer report in `GCRootScope.CandidateReachableOnlyFromAYoungCellSurvivesACycleForcedAtOnce`,
  intermittent (3 in 40) and present on 2.11.0 at the same rate: issue #3,
  fixed in 2.14.1 (an exiting thread left the stop-the-world quorum before it
  left the threads list; see docs/GarbageCollector.md, Known issues).

- No workload in the paradigm's own production style: these are the phase
  report's synthetic benchmarks.
- Peak RSS under the controller rose on two of three workloads; pacing
  changes when cycles start, and the 2.10.1 control law is unchanged until
  M3.
- The F4 slowdown on a quiet machine with near lines (S0, +20 % on the
  micro-benchmark) is real and small in absolute terms (about 2 ns per
  cell); no runtime workload measured here is in that regime long enough to
  show it.

## Data

[data/2026-10-03-collector-throughput/](data/2026-10-03-collector-throughput/):
`m1-m2-runs.jsonl` (every run: label, workload, verification, wall time, RSS,
CPU, the last `[GC-PHASES]` line), `runner.py` and `tab.py` (the runner
reads `PROTO_BIN`, `PROTO_JS_BENCH` and the workloads in `workloads/`),
`sweep-contention-m2.log`, `pmu/` (per-thread counters and their script),
`single-thread/` (perf stat output and `st6.sh`); M3: `m3-runs.jsonl`,
`matrix-fixed-and-first-law.jsonl` (the fixed-limit columns; its controller
rows are the law before the two corrections), `matrix-controller-2.13.0.jsonl`,
`matrix.py` (the experiment) and `mxjoin.py` (the table).
