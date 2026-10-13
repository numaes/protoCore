# protoScala benchmarks — 2026-10-12

- **Date:** 2026-10-12 22:15
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a60493af`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 1.68 / 1.95 / 4.72 at start, 2.70 / 2.24 / 4.66 at midpoint, 2.45 / 2.23 / 4.58 at end
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
| `int_sum_loop` | 29.8 [28.8-30.4] | 174.3 [163.3-191.5] | 32.4 [32.3-34.1] | 0.92× |
| `fib` | 61.3 [60.5-62.8] | 165.2 [163.6-203.3] | 39.0 [38.6-52.6] | 1.57× |
| `str_concat` | 27.6 [25.8-27.9] | 184.6 [164.1-198.2] | 28.5 [28.3-29.1] | 0.97× |
| `range_iterate` | 30.5 [28.9-31.2] | 175.5 [168.4-182.0] | 32.6 [31.1-33.6] | 0.93× |
| `tak` | 33.8 [32.5-35.2] | 166.1 [161.7-183.9] | 30.9 [30.2-31.6] | 1.09× |
| `fib30` | 471.3 [461.7-474.1] | 174.7 [169.7-192.7] | 153.8 [148.3-160.2] | 3.06× |
| `sum_loop` | 76.1 [75.1-79.2] | 163.0 [158.0-194.3] | 82.7 [80.6-94.7] | 0.92× |
| `factorial_100` | 24.6 [23.4-25.1] | 167.5 [162.3-170.9] | 28.8 [28.3-30.1] | 0.85× |
| `attr_lookup` | 43.8 [42.9-44.9] | 174.4 [165.0-185.4] | 38.0 [37.2-39.3] | 1.15× |
| `object_tree` | 324.3 [319.7-328.2] | 180.7 [179.5-200.3] | 174.2 [171.2-178.2] | 1.86× |
| `list_ops` | 437.9 [435.7-443.1] | FAILED | 1663.2 [1606.5-1733.5] | 0.26× |
| `map_build` | 294.4 [290.5-305.2] | 292.4 [281.5-311.5] | 64.1 [62.7-65.8] | 4.59× |
| **Geomean vs CPython** | 1.20× (12) | 3.57× (11) | 1.00× | **1.20×** |

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
| `int_sum_loop` | 2.9 | OK |
| `fib` | 2.8 | OK |
| `str_concat` | 3.0 | OK |
| `range_iterate` | 3.0 | OK |
| `tak` | 2.8 | OK |
| `fib30` | 3.0 | OK |
| `sum_loop` | 3.0 | OK |
| `factorial_100` | 3.0 | OK |
| `attr_lookup` | 3.0 | OK |
| `object_tree` | 3.7 | OK |
| `list_ops` | 3.6 | OK |
| `map_build` | 3.4 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 30.91 | [29.34-33.22] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 30.39 | [28.90-32.38] | MISSED |

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

