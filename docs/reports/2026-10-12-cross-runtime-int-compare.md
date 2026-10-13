# Cross-runtime comparisons after the SmallInteger fast paths (2.14.6)

Date: 2026-10-12/13.  Author: Gustavo Marino, with Claude.

Companion to [the mark work list and integer comparisons
(2.14.6)](2026-10-12-mark-stack-int-compare.md).  2.14.6 answers `compare`,
`sign`, `negate`, `abs`, `divide` and `modulo` on two SmallIntegers without
the heap temporary of the general path, and bounds the collector's mark
work list.  Question: does that move the comparisons of the runtimes with
their usual competitors, especially the integer loops?  protoST is measured
separately, by the agent preparing its talk release, and is not in this
report.

**Everything here is synthetic and notebook-class** (DEV12, Ryzen 5 5500U,
a desktop with an editor and a browser).  Numeric loops against a JIT (Node,
the JVM) measure the JIT: they are reported, but closing that gap is not
the goal.  The CPython comparison is the fair interpreter-against-
interpreter one.

## Verdict

- **Only the operations that reach protoCore benefit, and most integer
  loops in the harnesses use none of them.**  Every runtime has its own
  SmallInteger path for comparisons and arithmetic.  What reaches protoCore
  is division and remainder (protoPython `%` `//`, protoScala `%` `/`,
  protoClojure `/`), protoPython's unary minus, and protoJS's `===`/`!==`
  between two different integers.  On loops doing exactly those operations:
  protoPython `%` -18.7 %, `//` -16.5 %, unary minus -3.8 %; protoScala `%`
  -48 %, `/` -49 %; protoClojure `/` -36 % (cycles; A/A floor <= 3.4 %).
- **In the existing harnesses**, the integer workload that moves is
  protoJS's `control_flow` (`i % 2 === 0`): -26 % wall, -27 % cycles, about
  52x -> 39x Node's time.  protoScala's `object_tree` and `list_ops` gain
  9-10 % (they compare and divide inside the program).  The loops that came
  out worst against their competitors do not reach the changed code and are
  unchanged: protoJS `numeric_loop` (67-69x Node), protoPython
  `int_sum_loop` (about 1.0x CPython 3.14t), protoClojure `tak` (1.73x
  Babashka).
- **The mark-phase change shows in protoJS's parallel structure
  benchmarks**: at 12 workers `records` -21 %, `graph` -20.5 %, `wordfreq`
  -14 %; single-thread rows within a few percent.
- **No regression confirmed.**  Every `perf stat` check with an A/A arm is
  within its floor or a gain.  Two single-process structure rows rose
  (`join` par N = 6 +13 %, N = 2 +8 %); they were measured once per side
  and are not confirmed.
- **A first session was discarded.**  From about 21:50 UTC on 2026-10-12
  the maintainer's editor ran three file listers at about 9 CPUs (load
  ~40) for almost three hours.  The first session's second harness round
  and its `perf stat` checks fell in that window; they showed four
  protoPython "regressions" that do not exist.  Everything below is from
  the re-run of 2026-10-13 00:39-01:45 UTC, started once the 1-minute load
  was below 3 (each run behind the busy-CPU gate; the probes recorded loads
  of 1.3-3.7); the discarded records are kept in
  `results/first-session-contaminated/`.

## Method

- **Builds.**  Each runtime at its current default-branch commit
  (protoPython `e1c88f73`, protoJS `a57b2c688`, protoScala `dc8d4917`,
  protoClojure `359d8f6`), built twice the same way (GCC 15.2, Release,
  `-DPROTOCORE_REQUIRE_PACKAGE=ON`): against the protoCore 2.14.5 package
  (`*/build_cmp2145`) and against 2.14.6 (`*/build_pkg_oct12`, the trees of
  the 2.14.6 installers).  Each binary reaches its own protoCore through its
  RUNPATH; `ldd` was checked for every binary of both sides.
- **Competitors.**  CPython 3.14.0 and 3.14t (free-threaded); Node 22.17.0;
  Babashka 1.4.192 (as in protoClojure's RESULTS.md); Scala 3.9.0 on
  OpenJDK 25.0.4.1 (protoScala's last JVM report used OpenJDK 21).
- **Probes** (section 1): one loop of 2,000,000 iterations per operation and
  runtime, printing a checksum that must agree between arms; `perf stat -r
  5` cycles and instructions, arms before / after / before again (A/A).
- **Harnesses** (section 2): each runtime's own comparison, unchanged except
  for the binary: protoPython `benchmarks/run_4way_interleaved.py`, protoJS
  `tests/benchmarks/run_standard_comparison.js` and
  `benchmarks/structures/run.py`, protoScala `benchmarks/run_benchmarks.py`,
  protoClojure `benchmarks/bench.sh`.  Sides alternated run by run, three
  rounds, each run behind the 2026-10-04 quiet gate (no compiler or test
  suite, at most 2.5 CPUs busy).  "Change (ratio)" is the change of the
  per-run ratio runtime / competitor, which cancels a load change because
  every harness interleaves the competitor within the run.  protoJS's
  standard suite reports whole milliseconds and Node takes 1-4 ms there, so
  for it the change of protoJS's own in-process time is the reliable one.
  The structure benchmarks ran once per side (each process reports the
  median of 3 in-process repetitions, the method of protoJS's own report).
- **Checks** (section 3): `perf stat -r 5` of single workloads, before /
  after / A/A.
- Scripts, probes and records: [data/2026-10-12-cross-runtime/](data/2026-10-12-cross-runtime/)
  (`rerun.sh` ran the session; `xrt_tab.py results/harness` prints
  section 2's tables; `results/tables.md` holds them).

## 1. Which operations reach protoCore

Cycles per probe (G), medians of 5; A/A is the second "before" arm against
the first.  `cmp` is the loop with an extra comparison; `base` is the loop
alone (its own `<` already compares).

| Runtime | Operation | 2.14.5 | 2.14.6 | Change | A/A | Path |
|---|---|---:|---:|---:|---:|---|
| protoPython | base loop | 21.34 | 20.91 | -2.0 % | -0.6 % | own SmallInt path in `COMPARE_OP` |
| | `<` | 21.74 | 21.40 | -1.6 % | -0.1 % | own path |
| | `%` | 27.02 | 21.98 | **-18.7 %** | -0.5 % | `binaryModulo` -> `modulo`, `integerSign` x3 |
| | `//` | 26.23 | 21.91 | **-16.5 %** | +0.4 % | `binaryFloorDivide` -> `divide`, `integerSign` x2 |
| | unary `-` | 22.18 | 21.34 | **-3.8 %** | +0.2 % | `UNARY_NEGATIVE` -> `negate` |
| protoJS | base loop | 4.292 | 4.285 | -0.1 % | +0.9 % | own paths |
| | `<` | 4.426 | 4.454 | +0.6 % | -1.2 % | own path |
| | `%` | 4.217 | 4.093 | -2.9 % | -3.4 % | own path |
| | `/` (+ `Math.trunc`) | 8.276 | 8.323 | +0.6 % | -0.2 % | doubles |
| | unary `-` | 4.531 | 4.597 | +1.4 % | -0.1 % | own path |
| | `===` between different integers | (section 3) | | -27 % | | `OP_strict_eq` -> `ProtoObject::compare` |
| protoScala | base loop | 0.482 | 0.468 | -2.8 % | -2.4 % | own path |
| | `<` | 0.598 | 0.606 | +1.3 % | -0.3 % | own path |
| | `%` | 4.502 | 2.331 | **-48.2 %** | +0.4 % | `arith` primitive -> `modulo` |
| | `/` | 4.463 | 2.258 | **-49.4 %** | -0.3 % | `arith` primitive -> `divide` |
| | unary `-` | 0.494 | 0.494 | +0.1 % | -0.7 % | own path |
| protoClojure | base loop | 2.198 | 2.175 | -1.0 % | -1.1 % | own path |
| | `<` | 2.381 | 2.311 | -3.0 % | -1.0 % | own path |
| | `/` (integer, truncating) | 6.011 | 3.843 | **-36.1 %** | +1.9 % | `divide`, `integerSign` |
| | `(- x)` | 3.716 | 3.620 | -2.6 % | -0.3 % | own path |

protoClojure has no `rem`, `quot` or `mod`, and its `/` on integers
truncates (Clojure answers a ratio).  The protoPython probes run at module
level and cost about 10,000 cycles per iteration; they measure the change,
not protoPython's best case.  The first session's probes (before the
editor's load) agree within 1.5 points.

## 2. The harnesses (three rounds)

Ratio = runtime / competitor (> 1: the competitor is faster).  Full tables
in `results/tables.md` and `results/harness/structures_table.md`.

### protoPython against CPython 3.14t (`run_4way_interleaved.py`)

| Workload | 2.14.5 ms | 2.14.6 ms | CPython-t ms | Ratio before | Ratio after | Change (ratio) | A/A ratio spread |
|---|---:|---:|---:|---:|---:|---:|---:|
| int_sum_loop | 461.6 | 459.8 | 436.7 | 1.07 | 1.00 | -6.0 % | 2.4 % |
| range_iterate | 491.9 | 489.6 | 273.1 | 1.83 | 1.79 | -2.3 % | 3.2 % |
| call_recursion | 171.6 | 172.5 | 30.7 | 5.53 | 5.62 | +1.5 % | 4.3 % |
| list_append_loop | 132.3 | 134.5 | 22.8 | 5.81 | 5.91 | +1.8 % | 1.0 % |
| pyperf_fib | 368.9 | 373.8 | 79.5 | 4.56 | 4.73 | +3.8 % | 6.5 % |
| pyperf_binary_trees | 482.5 | 515.5 | 31.8 | 15.21 | 16.08 | +5.7 % | 10.1 % |
| pyperf_nqueens | 108.7 | 105.8 | 45.4 | 2.35 | 2.34 | -0.3 % | 7.7 % |
| pyperf_richards_lite | 40.2 | 40.2 | 23.5 | 1.74 | 1.68 | -3.6 % | 4.2 % |
| pyperf_sieve | 87.6 | 87.5 | 24.4 | 3.60 | 3.53 | -1.9 % | 1.4 % |
| protopyc int_sum_loop | 276.2 | 278.9 | 436.7 | 0.64 | 0.61 | -3.9 % | 3.3 % |
| protopyc attr_lookup | 1001.1 | 1040.5 | 318.6 | 3.18 | 3.35 | +5.2 % | 2.5 % |

None of these workloads divides or takes a remainder.  `int_sum_loop`'s
-6 % is at about twice its spread and has no mechanism here (the loop's
comparisons and additions have protoPython's own path); `perf stat` of the
allocation-heavy rows is flat (section 3).

### protoJS against Node 22 (`run_standard_comparison.js`, in-process ms)

| Workload | 2.14.5 ms | 2.14.6 ms | Change | A/A spread | Node ms | Ratio before | Ratio after |
|---|---:|---:|---:|---:|---:|---:|---:|
| control_flow | 207.0 | 154.0 | **-25.6 %** | 1.4 % | 4.0 | ~52 | ~39 |
| numeric_loop | 67.0 | 69.0 | +3.0 % | 0.0 % | 1.0 | ~67 | ~69 |
| function_calls | 183.0 | 175.0 | -4.4 % | 1.1 % | 1.0 | ~183 | ~175 |
| array_literal | 189.0 | 188.0 | -0.5 % | 33.7 % | 2.0 | ~95 | ~94 |
| object_property | 511.0 | 522.0 | +2.2 % | 13.0 % | 33.5 | 15.0 | 15.5 |
| object_write_only | 1504.0 | 1485.0 | -1.3 % | 19.0 % | 11.5 | 125 | 134 |
| string_concat | 112.0 | 114.0 | +1.8 % | 2.7 % | 1.0 | ~112 | ~114 |

`numeric_loop` does not reach protoCore's integer operations;
`control_flow` does, through `===` (`perf record` on 2.14.5:
`toTempBignum` 4.4 % plus malloc/free, under `ProtoObject::compare`).

### protoJS structure benchmarks (`benchmarks/structures/run.py`, 40 M cells)

| Workload | N | mode | 2.14.5 ms | 2.14.6 ms | Change | Node ms | Ratio before | Ratio after |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| records | 1 | par | 1552 | 1561 | +0.6 % | 149 | 10.4 | 10.5 |
| records | 12 | par | 6886 | 5442 | **-21.0 %** | 789 | 8.7 | 6.9 |
| records | 12 | seq | 17612 | 17566 | -0.3 % | 83 | 212 | 212 |
| join | 2 | par | 1476 | 1593 | +7.9 % | 94 | 15.7 | 16.9 |
| join | 6 | par | 2239 | 2537 | +13.3 % | 196 | 11.4 | 12.9 |
| join | 12 | par | 7208 | 6912 | -4.1 % | 312 | 23.1 | 22.2 |
| doctree | 12 | par | 4998 | 4802 | -3.9 % | 176 | 28.4 | 27.3 |
| wordfreq | 12 | par | 3901 | 3359 | **-13.9 %** | 81 | 48.2 | 41.5 |
| graph | 6 | par | 4173 | 3651 | -12.5 % | (fails) | - | - |
| graph | 12 | par | 11982 | 9529 | **-20.5 %** | (fails) | - | - |
| graph | 12 | seq | 25998 | 24827 | -4.5 % | 149 | 175 | 167 |

All 50 protoJS configurations completed with checksums equal to Node's
sequential run.  Of the rows not shown, all are within -5.1 to +2.8 %
except `graph` par N = 1 (-10.1 %) and N = 4 (-6.6 %).  Node's parallel
`graph` fails in its own `postMessage` (maximum call stack size exceeded)
on both sides.  One process per configuration and side, so the two `join`
rises are not confirmed.

### protoScala against CPython 3.14 and Scala 3.9.0 on the JVM (`run_benchmarks.py`, wall, cold start included)

| Workload | 2.14.5 ms | 2.14.6 ms | CPython ms | JVM ms | Ratio CPython before / after | Change (ratio) | A/A |
|---|---:|---:|---:|---:|---:|---:|---:|
| int_sum_loop | 29.7 | 30.0 | 32.5 | 171.7 | 0.91 / 0.92 | +1.3 % | 5.6 % |
| sum_loop | 75.8 | 75.1 | 82.7 | 173.0 | 0.89 / 0.91 | +2.6 % | 6.0 % |
| fib | 62.8 | 60.8 | 39.9 | 168.3 | 1.55 / 1.50 | -3.5 % | 10.5 % |
| fib30 | 479.3 | 469.8 | 153.1 | 172.9 | 3.09 / 3.06 | -0.9 % | 5.1 % |
| tak | 33.2 | 33.4 | 31.3 | 174.5 | 1.05 / 1.05 | +0.4 % | 3.4 % |
| object_tree | 364.5 | 331.1 | 174.7 | 180.9 | 2.09 / 1.88 | **-9.9 %** | 3.7 % |
| list_ops | 489.1 | 444.6 | 1660.7 | (fails) | 0.29 / 0.27 | **-9.7 %** | 3.6 % |
| map_build | 288.8 | 291.7 | 64.2 | 295.8 | 4.51 / 4.44 | -1.7 % | 1.1 % |

The JVM column is dominated by its start-up (about 170 ms for any
program).  `list_ops`'s JVM twin fails in every run (with OpenJDK 25; the
cause was not investigated).

### protoClojure against Babashka 1.4.192 (`bench.sh`, best of 3, cold start)

| Workload | 2.14.5 ms | 2.14.6 ms | bb ms | Ratio before | Ratio after | Change (ratio) | A/A |
|---|---:|---:|---:|---:|---:|---:|---:|
| fib | 506.0 | 471.0 | 403.5 | 1.25 | 1.17 | -5.8 % | 3.4 % |
| tak | 46.0 | 45.0 | 26.5 | 1.73 | 1.73 | +0.0 % | 2.1 % |
| sum-loop | 64.0 | 64.0 | 153.0 | 0.42 | 0.41 | -1.9 % | 0.7 % |
| reduce-list | 29.0 | 29.0 | 17.5 | 1.71 | 1.61 | -5.6 % | 9.0 % |
| sum-squares | 22.0 | 22.0 | 16.0 | 1.38 | 1.38 | +0.0 % | 5.9 % |

None of the five workloads divides.  `fib`'s -7 % raw is about twice its
spread and is not explained by the integer change (its comparisons and
arithmetic have protoClojure's own path); not investigated.

## 3. Checks with `perf stat` (cycles, millions; medians of 5)

| Workload | 2.14.5 | 2.14.6 | A/A | Change | Verdict |
|---|---:|---:|---:|---:|---|
| protopy list_append_loop | 468 | 473 | 472 | +1.2 % | flat (A/A +0.9 %) |
| protopy str_concat_loop | 427 | 425 | 422 | -0.4 % | flat |
| protopy richards_lite | 137.5 | 136.4 | 137.6 | -0.8 % | flat |
| protopy call_recursion | 635 | 633 | 633 | -0.3 % | flat |
| protoScala object_tree | 1,309 | 1,220 | 1,305 | **-6.8 %** | gain (instructions -12 %) |
| protoScala list_ops | 2,014 | 1,830 | 2,011 | **-9.2 %** | gain (instructions -9 %) |
| protoScala fib | 205 | 203 | 206 | -0.9 % | flat |
| protoJS control_flow | 3,997 | 2,900 | 3,988 | **-27.4 %** | gain (instructions -21 %) |
| protoJS object_property | 10,160 | 10,191 | 10,161 | +0.3 % | flat |

## What this does not show

- No server; one machine.  protoJS's structure benchmarks once per side.
- `protoPython/benchmarks/run_4way_interleaved.py` fails while writing its
  report when `PROTOPYC_BIN` is unset (it formats a missing geomean); the
  runs here set it.  Reported, not changed.
