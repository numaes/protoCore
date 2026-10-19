# protoScala benchmarks — 2026-10-19

- **Date:** 2026-10-19 00:19
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a6946796`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 2.81 / 2.82 / 3.33 at start, 3.02 / 2.92 / 3.34 at midpoint, 2.99 / 2.93 / 3.32 at end
- **Method:** 2 warmup + 5 timed runs per cell, interleaved across runtimes (round-robin: one sample of each runtime, then the next, so ambient load hits every column alike); median wall-clock of a cold process (start-up included), spread reported as `[min-max]` beside every median. This machine is a daily-driver desktop (VS Code, Chrome and PyCharm run throughout) and this runner gates on nothing: the load averages above are context, not a quietness claim; a claim that the host was quiet has to come from an `mpstat -P ALL` idle reading taken immediately before the run and recorded in the report that cites it. Ratios to CPython are the primary result, absolute milliseconds are indicative only. Every run's printed result is verified; a wrong result, non-zero exit or timeout marks the cell FAILED and it is excluded from every aggregate.

### Runtimes

| Column | Binary | Build / version | Twin programs |
|---|---|---|---|
| protoScala | `/home/gamarino/Documentos/proyectos/protoScala/build_pkg_oct18/protoscala` | Release | `benchmarks/comparable/*.scala` |
| Scala (JVM) | `/home/gamarino/Documentos/proyectos/tools/scala3-dl/scala3-3.9.0-x86_64-pc-linux` + `java -XX:-UsePerfData` | Scala compiler version 3.9.0 -- Copyright 2002-2026, LAMP/EPFL; openjdk version "25.0.4.1" 2026-08-18 | the same `.scala` files, compiled once with `scalac` (untimed) |
| CPython | `/usr/local/bin/python3.14` | Python 3.14.0 | protoPython's `benchmarks/*.py` (with `BENCH_N`), or `benchmarks/comparable/python/*.py` |

## Comparable workloads

Median wall-clock in milliseconds per cold process. `—`: the runtime has no twin of that workload. The last column is protoScala (canonical build) ÷ CPython (>1: protoScala slower). The geomean row gives, per column, the geometric mean of its ratio to CPython over the rows where both cells verified (row count in parentheses).

| Workload | protoScala (ms, median [min-max]) | Scala (JVM) (ms, median [min-max]) | CPython (ms, median [min-max]) | protoScala ÷ CPython |
|---|---:|---:|---:|---:|
| `int_sum_loop` | 31.5 [30.2-31.9] | 188.0 [180.5-195.6] | 35.0 [34.3-37.5] | 0.90× |
| `fib` | 63.8 [62.7-67.8] | 188.9 [183.1-200.4] | 43.2 [41.6-44.9] | 1.48× |
| `str_concat` | 27.6 [27.1-28.5] | 196.9 [180.0-213.3] | 31.5 [30.5-31.8] | 0.88× |
| `range_iterate` | 30.8 [30.2-32.8] | 180.4 [180.0-192.2] | 33.9 [32.9-36.7] | 0.91× |
| `tak` | 34.2 [33.3-35.1] | 179.4 [173.3-196.8] | 34.0 [32.1-36.8] | 1.01× |
| `fib30` | 511.1 [504.7-541.9] | 183.1 [180.5-188.4] | 173.3 [163.0-181.0] | 2.95× |
| `sum_loop` | 82.6 [79.8-89.9] | 179.3 [171.9-203.8] | 86.8 [86.1-99.6] | 0.95× |
| `factorial_100` | 25.3 [23.7-26.7] | 186.6 [177.8-209.5] | 30.1 [28.9-32.2] | 0.84× |
| `attr_lookup` | 46.8 [45.5-49.7] | 187.8 [181.6-270.2] | 41.5 [38.4-71.7] | 1.13× |
| `object_tree` | 350.0 [335.4-401.7] | 210.7 [186.3-318.0] | 191.6 [183.4-194.2] | 1.83× |
| `list_ops` | 480.5 [460.9-485.3] | FAILED | 1798.5 [1754.9-1822.5] | 0.27× |
| `map_build` | 312.9 [302.8-320.6] | 333.0 [309.5-337.6] | 72.2 [68.0-74.6] | 4.33× |
| **Geomean vs CPython** | 1.16× (12) | 3.59× (11) | 1.00× | **1.16×** |

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
| `int_sum_loop` | 3.4 | OK |
| `fib` | 3.2 | OK |
| `str_concat` | 3.7 | OK |
| `range_iterate` | 3.6 | OK |
| `tak` | 3.4 | OK |
| `fib30` | 3.3 | OK |
| `sum_loop` | 3.5 | OK |
| `factorial_100` | 3.4 | OK |
| `attr_lookup` | 3.4 | OK |
| `object_tree` | 4.0 | OK |
| `list_ops` | 4.0 | OK |
| `map_build` | 4.1 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 31.71 | [30.22-36.83] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 31.02 | [28.84-32.99] | MISSED |

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

