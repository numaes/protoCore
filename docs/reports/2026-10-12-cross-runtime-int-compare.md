# Cross-runtime comparisons after the SmallInteger fast paths (2.14.6)

Date: 2026-10-12.  Author: Gustavo Marino, with Claude.

Companion to [the mark work list and integer comparisons
(2.14.6)](2026-10-12-mark-stack-int-compare.md).  2.14.6 answers `compare`,
`sign`, `negate`, `abs`, `divide` and `modulo` on two SmallIntegers without
the heap temporary of the general path.  Question: does that move the
comparisons of the runtimes with their usual competitors, especially the
integer loops?  protoST is measured separately, by the agent preparing its
talk release, and is not in this report.

**Everything here is synthetic and notebook-class** (DEV12, Ryzen 5 5500U,
a desktop with an editor and a browser).  Numeric loops against a JIT (Node,
the JVM) measure the JIT: they are reported, but closing that gap is not
the goal.  The CPython comparison is the fair interpreter-against-
interpreter one.

## Verdict

- **Only the operations that reach protoCore benefit, and most integer
  loops in the harnesses use none of them.**  Each runtime has its own
  SmallInteger path for `<` and friends; what reaches protoCore is
  division and remainder (protoPython `%` `//`, protoScala `%` `/`,
  protoClojure `/`), protoPython's unary minus and protoJS's `===`/`!==`
  between two different integers.  On loops that do exactly those
  operations: protoPython `%` -18 %, `//` -16 %; protoScala `%` and `/`
  -47 %; protoClojure `/` -38 % (cycles, A/A floor <= 3.4 %).
- **In the existing harnesses**, the one integer workload that moves is
  protoJS's `control_flow` (`i % 2 === 0`), -28 % wall and -26 % cycles:
  its ratio to Node goes from about 60x to about 43x.  protoScala's
  `object_tree` (-10 % cycles) and `list_ops` (-8 % wall) gain as well.
  Everything else, including every loop that came out badly against Node
  or CPython (protoJS `numeric_loop` ~75x Node, protoPython `int_sum_loop`
  ~1.0x CPython 3.14t, protoClojure `fib`/`tak` against Babashka), is
  unchanged within the noise: those loops never reached the changed code.
- **No regression confirmed.**  The protoPython harness showed +5 to +9 %
  on four allocation-heavy rows; with `perf stat` and an A/A arm all four
  are within the floor (section 3).  protoJS `object_property` is +2.6 %
  cycles with 1 % fewer instructions (inconclusive).
- **Not run:** protoJS's structure benchmarks and a third clean round: from
  22:00 UTC the maintainer's editor ran three file listers at about 9 CPUs
  for over an hour, and no quiet window came back within this task.

## Method

- **Builds.**  Each runtime at its current default-branch commit
  (protoPython `e1c88f73`, protoJS `a57b2c688`, protoScala `dc8d4917`,
  protoClojure `359d8f6`), built twice the same way (GCC 15.2, Release,
  `-DPROTOCORE_REQUIRE_PACKAGE=ON`): against the protoCore 2.14.5 package
  (`*/build_cmp2145`) and against 2.14.6 (`*/build_pkg_oct12`, the trees of
  the 2.14.6 installers).  Each binary reaches its own protoCore through its
  RUNPATH; `ldd` was checked for every binary of both sides.
- **Competitors.**  CPython 3.14.0 and 3.14t (free-threaded); Node 22.17.0;
  Babashka 1.4.192 (as in protoClojure's RESULTS.md); Scala 3.9.0 on OpenJDK
  25.0.4.1 (protoScala's last JVM report used OpenJDK 21).
- **Probes** (section 1): one loop of 2,000,000 iterations per operation and
  runtime, printing a checksum that must agree between arms; `perf stat -r
  5` cycles and instructions, arms before / after / before again (A/A).
- **Harnesses** (section 2): each runtime's own comparison, unchanged except
  for the binary: protoPython `benchmarks/run_4way_interleaved.py`, protoJS
  `tests/benchmarks/run_standard_comparison.js`, protoScala
  `benchmarks/run_benchmarks.py`, protoClojure `benchmarks/bench.sh`.  Sides
  alternated run by run behind the 2026-10-04 quiet gate.  Rounds 1 and 2
  are used; the second "after" run already overlapped the editor's load
  (CPython itself 10-30 % slower in it), so the primary change is that of
  the per-run ratio runtime / competitor, which cancels a load change
  because every harness interleaves the competitor within the run.
  protoJS's suite reports whole milliseconds and Node takes 1-4 ms there, so
  for protoJS the raw change of its own in-process time is the reliable one.
- **Checks** (section 3): `perf stat -r 5` of single workloads, before /
  after / A/A, run while the machine was loaded: cycles are less sensitive
  to load than wall time, and the A/A arm shows the floor.
- Harness, probes and raw outputs:
  [data/2026-10-12-cross-runtime/](data/2026-10-12-cross-runtime/).
  `xrt_tab.py <outdir> 1,2` prints section 2's tables from `results/harness/`.

## 1. Which operations reach protoCore

Cycles per probe (G), medians of 5; A/A is the second "before" arm against
the first.  `cmp` is the loop with an extra comparison; `base` is the loop
alone (whose own `<` already compares).

| Runtime | Operation | 2.14.5 | 2.14.6 | Change | A/A | Path |
|---|---|---:|---:|---:|---:|---|
| protoPython | base loop | 21.57 | 21.53 | -0.2 % | -1.4 % | own SmallInt path in `COMPARE_OP` |
| | `<` | 21.73 | 21.69 | -0.2 % | +1.9 % | own path |
| | `%` | 27.43 | 22.57 | **-17.7 %** | +0.3 % | `binaryModulo` -> `modulo`, `integerSign` x3 |
| | `//` | 26.56 | 22.46 | **-15.5 %** | +0.6 % | `binaryFloorDivide` -> `divide`, `integerSign` x2 |
| | unary `-` | 22.68 | 21.49 | **-5.2 %** | -0.7 % | `UNARY_NEGATIVE` -> `negate` |
| protoJS | base loop | 4.218 | 4.158 | -1.4 % | +0.5 % | own paths |
| | `<` | 4.474 | 4.415 | -1.3 % | -1.7 % | own path |
| | `%` | 4.268 | 4.216 | -1.2 % | -0.0 % | own path |
| | `/` (+ `Math.trunc`) | 8.379 | 8.481 | +1.2 % | +0.6 % | doubles |
| | unary `-` | 4.657 | 4.762 | +2.3 % | +0.3 % | own path |
| | `===` between different integers | (section 2) | | -26 % | | `OP_strict_eq` -> `ProtoObject::compare` |
| protoScala | base loop | 0.485 | 0.481 | -0.8 % | -1.2 % | own path |
| | `<` | 0.634 | 0.611 | -3.7 % | -3.4 % | own path |
| | `%` | 4.556 | 2.395 | **-47.4 %** | +0.3 % | `arith` primitive -> `modulo` |
| | `/` | 4.510 | 2.350 | **-47.9 %** | +1.0 % | `arith` primitive -> `divide` |
| | unary `-` | 0.513 | 0.505 | -1.6 % | -1.6 % | own path |
| protoClojure | base loop | 2.257 | 2.217 | -1.7 % | -2.9 % | own path |
| | `<` | 2.376 | 2.436 | +2.5 % | +1.2 % | own path |
| | `/` (integer, truncating) | 6.168 | 3.852 | **-37.5 %** | -0.9 % | `divide`, `integerSign` |
| | `(- x)` | 3.788 | 3.683 | -2.7 % | +0.2 % | own path |

protoClojure has no `rem`, `quot` or `mod`, and its `/` on integers
truncates (Clojure answers a ratio).  The protoPython probes run at module
level and cost about 10,000 cycles per iteration; they measure the change,
not protoPython's best case.

## 2. The harnesses (rounds 1 and 2)

Ratio = runtime / competitor (> 1: the competitor is faster).  Full tables:
`results/tables_rounds12.md`.

### protoPython against CPython 3.14t (`run_4way_interleaved.py`)

| Workload | 2.14.5 ms | 2.14.6 ms | CPython-t ms | Ratio before | Ratio after | Change (ratio) | A/A ratio spread |
|---|---:|---:|---:|---:|---:|---:|---:|
| int_sum_loop | 487.6 | 498.0 | 471.9 | 1.05 | 1.01 | -3.9 % | 4.6 % |
| range_iterate | 520.2 | 560.8 | 297.6 | 1.79 | 1.74 | -2.6 % | 1.4 % |
| multithread_cpu | 112.8 | 135.1 | 169.1 | 0.68 | 0.71 | +4.4 % | 6.1 % |
| call_recursion | 184.4 | 196.4 | 32.6 | 5.71 | 5.82 | +2.0 % | 3.5 % |
| list_append_loop | 141.9 | 151.1 | 23.9 | 6.02 | 6.32 | +4.9 % | 0.3 % |
| pyperf_fib | 395.7 | 422.5 | 85.4 | 4.69 | 4.78 | +1.9 % | 1.1 % |
| pyperf_nqueens | 112.6 | 115.4 | 48.1 | 2.34 | 2.31 | -1.5 % | 4.4 % |
| pyperf_richards_lite | 41.4 | 43.7 | 24.2 | 1.71 | 1.83 | +6.5 % | 2.3 % |
| pyperf_sieve | 96.1 | 96.1 | 25.8 | 3.74 | 3.65 | -2.5 % | 2.3 % |
| protopyc int_sum_loop | 296.6 | 305.6 | 471.9 | 0.64 | 0.62 | -2.9 % | 1.5 % |
| protopyc str_concat_loop | 190.3 | 212.8 | 22.9 | 8.40 | 9.19 | +9.4 % | 1.0 % |

None of these workloads divides or takes a remainder; the rows above the
spread are allocation-heavy and are within the A/A floor in section 3.

### protoJS against Node 22 (`run_standard_comparison.js`, in-process ms)

| Workload | 2.14.5 ms | 2.14.6 ms | Change | A/A spread | Node ms | Ratio before | Ratio after |
|---|---:|---:|---:|---:|---:|---:|---:|
| control_flow | 239.0 | 172.5 | **-27.8 %** | 3.3 % | 4.0 | ~60 | ~43 |
| numeric_loop | 75.0 | 78.0 | +4.0 % | 8.0 % | 1.0 | ~75 | ~78 |
| function_calls | 200.5 | 196.0 | -2.2 % | 0.5 % | 1.0 | ~200 | ~196 |
| array_literal | 200.5 | 201.0 | +0.2 % | 1.5 % | 2.0 | ~100 | ~100 |
| object_property | 531.5 | 575.5 | +8.3 % | 0.9 % | 36.0 | 14.8 | 16.0 |
| object_write_only | 1735.5 | 1649.5 | -5.0 % | 12.6 % | 13.5 | 128 | 122 |
| tree_traversal | 59.5 | 59.5 | 0.0 % | 11.8 % | 1.0 | ~60 | ~60 |

`numeric_loop` (the 75x row) does not reach protoCore's integer operations;
`control_flow` does, through `===`.

### protoScala against CPython 3.14 and Scala 3.9.0 on the JVM (`run_benchmarks.py`, wall, cold start included)

| Workload | 2.14.5 ms | 2.14.6 ms | CPython ms | JVM ms | Ratio CPython before / after | Change (ratio to CPython) |
|---|---:|---:|---:|---:|---:|---:|
| int_sum_loop | 32.0 | 31.4 | 36.8 | 199.7 | 0.86 / 0.85 | -1.5 % |
| sum_loop | 84.8 | 83.0 | 90.8 | 194.9 | 0.93 / 0.91 | -2.0 % |
| fib30 | 531.0 | 543.1 | 173.1 | 198.8 | 3.03 / 3.14 | +3.5 % (spread 4.1 %) |
| tak | 35.3 | 35.0 | 34.2 | 194.6 | 1.02 / 1.02 | +0.5 % |
| object_tree | 394.4 | 356.8 | 193.3 | 212.4 | 2.04 / 1.84 | **-9.6 %** |
| list_ops | 530.1 | 487.2 | 1828.4 | (failed on JDK 25) | 0.29 / 0.26 | **-12.2 %** |
| map_build | 314.5 | 320.3 | 71.7 | 339.1 | 4.38 / 4.53 | +3.2 % (spread 6.3 %) |

The JVM figures are dominated by its start-up (~190 ms for any program).
`list_ops`'s JVM twin failed in every run (with OpenJDK 25; the cause was not
investigated).

### protoClojure against Babashka 1.4.192 (`bench.sh`, best of 3, cold start)

| Workload | 2.14.5 ms | 2.14.6 ms | bb ms | Ratio before | Ratio after | Change (ratio) |
|---|---:|---:|---:|---:|---:|---:|
| fib | 531.0 | 533.0 | 440.5 | 1.22 | 1.20 | -0.9 % |
| tak | 47.5 | 46.5 | 27.0 | 1.76 | 1.66 | -5.6 % |
| sum-loop | 68.5 | 66.0 | 163.5 | 0.42 | 0.40 | -5.1 % |
| reduce-list | 30.0 | 30.5 | 18.5 | 1.62 | 1.65 | +1.7 % |
| sum-squares | 23.0 | 22.5 | 16.5 | 1.44 | 1.32 | -7.9 % (spread 8.7 %) |

None of protoClojure's five workloads divides; the changes are within the
millisecond resolution of a best-of-3 cold start.

## 3. Checks with `perf stat` (cycles, millions; medians of 5)

| Workload | 2.14.5 | 2.14.6 | A/A | Change | Verdict |
|---|---:|---:|---:|---:|---|
| protopy list_append_loop | 663 | 656 | 646 | -1.1 % | no regression (A/A -2.6 %) |
| protopy str_concat_loop | 598 | 582 | 575 | -2.7 % | no regression (A/A -3.9 %) |
| protopy richards_lite | 187 | 178 | 188 | -4.4 % | no regression |
| protopy call_recursion | 891 | 879 | 877 | -1.3 % | unchanged |
| protoScala object_tree | 1,715 | 1,539 | 1,676 | **-10.3 %** | gain (A/A -2.3 %) |
| protoScala list_ops | 2,173 | 2,114 | 2,355 | -2.7 % | within the floor (A/A +8.4 %) |
| protoScala fib | 267 | 269 | 266 | +0.6 % | unchanged |
| protoJS control_flow | 5,572 | 4,131 | 5,611 | **-25.9 %** | gain (instructions -21 %) |
| protoJS object_property | 13,444 | 13,793 | 13,446 | +2.6 % | inconclusive (instructions -1.0 %) |

Run while the machine was loaded (about 9 CPUs of file listing); the A/A
column is the floor of that state.  The protopyc `str_concat_loop` row was
not checked (its workload is compiled ahead of time by the harness).

## What this does not show

- protoJS's structure benchmarks (`benchmarks/structures/run.py`) were not
  re-run.  Their collector-bound part is covered by the main report's
  protoJS rows (the same workloads, `gc` group, with the same binaries on
  both sides).
- Two harness rounds per side instead of three; the second "after" round
  was partly under load (handled by the ratio, section 2).
- No server; one machine.
