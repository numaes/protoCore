# protoScala benchmarks — 2026-10-12

- **Date:** 2026-10-12 18:50
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `dc8d4917`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 2.61 / 2.87 / 3.63 at start, 4.28 / 3.33 / 3.74 at midpoint, 3.64 / 3.26 / 3.70 at end
- **Method:** 2 warmup + 5 timed runs per cell, interleaved across runtimes (round-robin: one sample of each runtime, then the next, so ambient load hits every column alike); median wall-clock of a cold process (start-up included), spread reported as `[min-max]` beside every median. This machine is a daily-driver desktop (VS Code, Chrome and PyCharm run throughout) and this runner gates on nothing: the load averages above are context, not a quietness claim; a claim that the host was quiet has to come from an `mpstat -P ALL` idle reading taken immediately before the run and recorded in the report that cites it. Ratios to CPython are the primary result, absolute milliseconds are indicative only. Every run's printed result is verified; a wrong result, non-zero exit or timeout marks the cell FAILED and it is excluded from every aggregate.

### Runtimes

| Column | Binary | Build / version | Twin programs |
|---|---|---|---|
| protoScala | `$WS/protoScala/build_pkg_oct12/protoscala` | Release | `benchmarks/comparable/*.scala` |
| Scala (JVM) | `$WS/tools/scala3-dl/scala3-3.9.0-x86_64-pc-linux` + `java -XX:-UsePerfData` | Scala compiler version 3.9.0 -- Copyright 2002-2026, LAMP/EPFL; openjdk version "25.0.4.1" 2026-08-18 | the same `.scala` files, compiled once with `scalac` (untimed) |
| CPython | `/usr/local/bin/python3.14` | Python 3.14.0 | protoPython's `benchmarks/*.py` (with `BENCH_N`), or `benchmarks/comparable/python/*.py` |

## Comparable workloads

Median wall-clock in milliseconds per cold process. `—`: the runtime has no twin of that workload. The last column is protoScala (canonical build) ÷ CPython (>1: protoScala slower). The geomean row gives, per column, the geometric mean of its ratio to CPython over the rows where both cells verified (row count in parentheses).

| Workload | protoScala (ms, median [min-max]) | Scala (JVM) (ms, median [min-max]) | CPython (ms, median [min-max]) | protoScala ÷ CPython |
|---|---:|---:|---:|---:|
| `int_sum_loop` | 30.3 [29.0-31.3] | 189.7 [171.6-204.1] | 36.3 [34.2-36.5] | 0.83× |
| `fib` | 64.1 [62.7-68.4] | 194.1 [175.6-202.2] | 42.2 [40.5-46.6] | 1.52× |
| `str_concat` | 28.3 [26.5-30.1] | 175.4 [170.8-220.7] | 29.4 [28.8-29.9] | 0.97× |
| `range_iterate` | 31.0 [30.5-33.6] | 181.4 [171.3-191.6] | 33.8 [33.7-39.1] | 0.92× |
| `tak` | 33.7 [33.0-35.5] | 188.4 [183.9-209.3] | 33.1 [31.6-35.9] | 1.02× |
| `fib30` | 500.5 [492.8-519.3] | 189.9 [188.0-226.0] | 164.7 [159.0-176.1] | 3.04× |
| `sum_loop` | 80.0 [77.9-86.0] | 182.6 [174.0-191.5] | 87.5 [83.3-101.4] | 0.91× |
| `factorial_100` | 26.0 [23.8-27.8] | 188.0 [177.1-198.4] | 29.7 [29.0-31.1] | 0.88× |
| `attr_lookup` | 46.3 [43.8-47.7] | 196.7 [177.5-198.5] | 41.0 [38.4-42.1] | 1.13× |
| `object_tree` | 338.1 [331.8-354.7] | 198.2 [189.9-217.9] | 185.2 [177.9-189.3] | 1.83× |
| `list_ops` | 463.4 [453.4-468.4] | FAILED | 1776.4 [1728.2-1878.0] | 0.26× |
| `map_build` | 309.9 [302.7-321.0] | 317.2 [303.3-344.3] | 67.8 [67.8-71.1] | 4.57× |
| **Geomean vs CPython** | 1.17× (12) | 3.66× (11) | 1.00× | **1.17×** |

### FAILED cells

- `list_ops` / jvm: printed '1409965408 100000 50000', expected '9999900000 100000 50000'

### Workloads

| Workload | Work | Origin of the twin set | Notes |
|---|---|---|---|
| `int_sum_loop` | sum of 0 until 100000 | protoPython / protoST | protoST's twin sums 1..N (result 5000050000); the others sum 0 until N. |
| `fib` | recursive fib(25) | protoPython / protoST |  |
| `str_concat` | 2000 string concatenations | protoPython / protoST |  |
| `range_iterate` | count 100000 iterations | protoPython / protoST | protoScala: a `while` loop. `for` landed in Phase 2, but `Range` is Phase 3, so the twin keeps the loop. |
| `tak` | tak(18, 12, 6) | protoClojure |  |
| `fib30` | recursive fib(30) | protoClojure | protoClojure's `fib.clj`; CPython/protopy run `call_recursion.py` with `BENCH_N=30`. |
| `sum_loop` | sum of 0..1000000 | protoClojure |  |
| `factorial_100` | 100! (BigInt, 158 digits) | protoClojure | Declared `BigInt` so the JVM does not overflow (Int at 13!, Long at 21!); protoScala promotes automatically (D1). |
| `attr_lookup` | 3 field reads x 100000 | protoPython / protoST |  |
| `object_tree` | build, path-copy and fold a 131071-object case-class tree | protoScala | the deep-object-graph workload of DESIGN §1; CPython runs the __slots__ twin in comparable/python/. |
| `list_ops` | map / filter / foldLeft over a 100000-element List | protoScala |  |
| `map_build` | build and read back a 50000-entry Map | protoScala |  |

### JVM compile time (untimed for the table)

`scalac -d build_jvm/<workload> <file>.scala`, once per workload:

| Workload | scalac (s) | Status |
|---|---:|---|
| `int_sum_loop` | 3.1 | OK |
| `fib` | 3.1 | OK |
| `str_concat` | 3.3 | OK |
| `range_iterate` | 3.3 | OK |
| `tak` | 3.2 | OK |
| `fib30` | 3.3 | OK |
| `sum_loop` | 3.5 | OK |
| `factorial_100` | 3.3 | OK |
| `attr_lookup` | 3.3 | OK |
| `object_tree` | 4.0 | OK |
| `list_ops` | 3.8 | OK |
| `map_build` | 3.7 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 32.33 | [30.11-41.41] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 31.43 | [29.23-42.90] | MISSED |

## Pending workloads

Workloads of the sibling suites that protoScala cannot express yet; each waits for the phase named below. They are not approximated.

| Workload | Suite | Arrives with | Why |
|---|---|---|---|
| `list_append` | protoPython / protoST | expressible since Phase 3; not in suite v1 | `:+` landed in Phase 3, so the twin is now writable. It is not part of the ROADMAP's suite v1 and `list_ops` already exercises the same surface. |
| `sum_squares` | protoClojure | expressible since Phase 3; not in suite v1 | `map` plus `sum`/`foldLeft` landed in Phase 3, so the twin is now writable, and `list_ops` already exercises both. |
| `exception_latency` | protoPython / protoST | Phase 4 (exceptions) | `throw` and `try`/`catch`. |

## Reproduce

```bash
SCALA_HOME=/path/to/scala3 benchmarks/bench.sh --name suite
```

