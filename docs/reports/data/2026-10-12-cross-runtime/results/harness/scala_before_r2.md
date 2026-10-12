# protoScala benchmarks — 2026-10-12

- **Date:** 2026-10-12 18:55
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `dc8d4917`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 2.40 / 2.80 / 3.45 at start, 3.56 / 3.13 / 3.52 at midpoint, 3.79 / 3.24 / 3.54 at end
- **Method:** 2 warmup + 5 timed runs per cell, interleaved across runtimes (round-robin: one sample of each runtime, then the next, so ambient load hits every column alike); median wall-clock of a cold process (start-up included), spread reported as `[min-max]` beside every median. This machine is a daily-driver desktop (VS Code, Chrome and PyCharm run throughout) and this runner gates on nothing: the load averages above are context, not a quietness claim; a claim that the host was quiet has to come from an `mpstat -P ALL` idle reading taken immediately before the run and recorded in the report that cites it. Ratios to CPython are the primary result, absolute milliseconds are indicative only. Every run's printed result is verified; a wrong result, non-zero exit or timeout marks the cell FAILED and it is excluded from every aggregate.

### Runtimes

| Column | Binary | Build / version | Twin programs |
|---|---|---|---|
| protoScala | `$WS/protoScala/build_cmp2145/protoscala` | Release | `benchmarks/comparable/*.scala` |
| Scala (JVM) | `$WS/tools/scala3-dl/scala3-3.9.0-x86_64-pc-linux` + `java -XX:-UsePerfData` | Scala compiler version 3.9.0 -- Copyright 2002-2026, LAMP/EPFL; openjdk version "25.0.4.1" 2026-08-18 | the same `.scala` files, compiled once with `scalac` (untimed) |
| CPython | `/usr/local/bin/python3.14` | Python 3.14.0 | protoPython's `benchmarks/*.py` (with `BENCH_N`), or `benchmarks/comparable/python/*.py` |

## Comparable workloads

Median wall-clock in milliseconds per cold process. `—`: the runtime has no twin of that workload. The last column is protoScala (canonical build) ÷ CPython (>1: protoScala slower). The geomean row gives, per column, the geometric mean of its ratio to CPython over the rows where both cells verified (row count in parentheses).

| Workload | protoScala (ms, median [min-max]) | Scala (JVM) (ms, median [min-max]) | CPython (ms, median [min-max]) | protoScala ÷ CPython |
|---|---:|---:|---:|---:|
| `int_sum_loop` | 33.5 [33.3-35.8] | 214.0 [191.4-234.1] | 38.7 [36.3-45.2] | 0.86× |
| `fib` | 68.0 [65.4-79.4] | 218.4 [196.9-246.5] | 54.6 [49.0-57.2] | 1.25× |
| `str_concat` | 29.7 [28.3-31.0] | 223.2 [202.3-250.9] | 33.3 [31.0-34.5] | 0.89× |
| `range_iterate` | 33.3 [31.2-40.0] | 196.8 [180.6-202.4] | 38.3 [37.0-50.0] | 0.87× |
| `tak` | 36.2 [33.6-39.0] | 202.8 [190.5-242.7] | 36.1 [33.9-39.8] | 1.00× |
| `fib30` | 552.4 [520.7-566.4] | 207.6 [203.5-222.3] | 186.2 [175.2-218.3] | 2.97× |
| `sum_loop` | 86.7 [84.5-92.5] | 200.8 [193.5-239.3] | 92.5 [89.7-102.6] | 0.94× |
| `factorial_100` | 27.6 [25.4-40.1] | 203.5 [191.7-251.1] | 32.4 [31.1-48.8] | 0.85× |
| `attr_lookup` | 48.8 [47.4-51.0] | 203.1 [198.2-223.0] | 42.7 [40.2-46.6] | 1.14× |
| `object_tree` | 408.8 [391.1-424.4] | 226.7 [215.4-242.2] | 203.1 [191.1-218.1] | 2.01× |
| `list_ops` | 545.5 [533.9-557.3] | FAILED | 1880.4 [1832.9-1939.1] | 0.29× |
| `map_build` | 330.1 [312.6-336.4] | 353.5 [341.0-378.0] | 73.0 [70.5-82.3] | 4.52× |
| **Geomean vs CPython** | 1.16× (12) | 3.68× (11) | 1.00× | **1.16×** |

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
| `int_sum_loop` | 3.7 | OK |
| `fib` | 4.0 | OK |
| `str_concat` | 3.5 | OK |
| `range_iterate` | 3.2 | OK |
| `tak` | 3.4 | OK |
| `fib30` | 3.2 | OK |
| `sum_loop` | 3.6 | OK |
| `factorial_100` | 3.8 | OK |
| `attr_lookup` | 3.7 | OK |
| `object_tree` | 4.4 | OK |
| `list_ops` | 4.2 | OK |
| `map_build` | 4.3 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 31.87 | [30.30-35.04] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 31.88 | [30.26-44.14] | MISSED |

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

