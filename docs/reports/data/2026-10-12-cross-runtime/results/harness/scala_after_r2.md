# protoScala benchmarks — 2026-10-12

- **Date:** 2026-10-12 18:59
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `dc8d4917`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 3.77 / 3.41 / 3.55 at start, 4.26 / 3.72 / 3.65 at midpoint, 3.70 / 3.65 / 3.63 at end
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
| `int_sum_loop` | 32.5 [31.4-42.6] | 209.7 [204.9-289.3] | 37.4 [36.1-49.2] | 0.87× |
| `fib` | 74.7 [70.1-83.3] | 232.2 [220.0-275.4] | 53.7 [44.7-74.5] | 1.39× |
| `str_concat` | 32.3 [28.5-35.7] | 228.6 [195.5-246.4] | 35.6 [31.7-55.6] | 0.91× |
| `range_iterate` | 33.4 [32.1-34.2] | 207.6 [187.9-219.4] | 36.6 [36.3-38.9] | 0.91× |
| `tak` | 36.3 [35.6-37.5] | 200.7 [196.1-221.6] | 35.2 [33.5-37.4] | 1.03× |
| `fib30` | 585.7 [539.8-627.9] | 225.3 [212.7-278.4] | 181.2 [180.1-187.5] | 3.23× |
| `sum_loop` | 86.0 [83.3-90.3] | 201.5 [179.9-205.0] | 94.0 [88.3-98.3] | 0.91× |
| `factorial_100` | 26.4 [24.4-39.0] | 196.4 [193.9-202.3] | 31.9 [29.7-33.8] | 0.83× |
| `attr_lookup` | 47.5 [45.4-52.4] | 205.7 [186.2-219.5] | 43.8 [39.6-48.0] | 1.08× |
| `object_tree` | 375.4 [366.7-385.8] | 229.6 [199.9-233.1] | 201.5 [187.2-207.7] | 1.86× |
| `list_ops` | 511.1 [495.8-548.3] | FAILED | 2016.0 [1919.6-2174.8] | 0.25× |
| `map_build` | 330.7 [320.3-342.1] | 353.0 [322.5-359.4] | 73.8 [69.6-85.9] | 4.48× |
| **Geomean vs CPython** | 1.16× (12) | 3.76× (11) | 1.00× | **1.16×** |

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
| `int_sum_loop` | 3.5 | OK |
| `fib` | 3.4 | OK |
| `str_concat` | 3.5 | OK |
| `range_iterate` | 3.3 | OK |
| `tak` | 3.5 | OK |
| `fib30` | 3.5 | OK |
| `sum_loop` | 3.4 | OK |
| `factorial_100` | 3.6 | OK |
| `attr_lookup` | 4.0 | OK |
| `object_tree` | 4.4 | OK |
| `list_ops` | 4.1 | OK |
| `map_build` | 4.3 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 31.54 | [29.53-34.05] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 31.82 | [28.92-33.64] | MISSED |

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

