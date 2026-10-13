# protoScala benchmarks — 2026-10-12

- **Date:** 2026-10-12 22:07
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a60493af`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 1.48 / 2.20 / 6.61 at start, 1.72 / 2.17 / 6.37 at midpoint, 1.58 / 2.09 / 6.21 at end
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
| `int_sum_loop` | 30.3 [29.1-30.8] | 172.7 [167.7-175.3] | 33.1 [32.5-34.6] | 0.92× |
| `fib` | 60.8 [59.6-62.6] | 167.2 [163.1-184.7] | 40.6 [39.2-41.8] | 1.50× |
| `str_concat` | 27.1 [25.2-27.5] | 171.7 [170.0-179.9] | 28.9 [28.7-30.0] | 0.94× |
| `range_iterate` | 29.7 [28.7-30.1] | 166.5 [158.1-173.1] | 32.6 [31.6-35.6] | 0.91× |
| `tak` | 32.8 [32.2-33.8] | 174.5 [162.1-194.9] | 31.2 [30.7-43.2] | 1.05× |
| `fib30` | 469.8 [454.9-494.0] | 166.6 [164.7-184.4] | 152.0 [150.7-156.3] | 3.09× |
| `sum_loop` | 75.1 [74.8-75.7] | 164.8 [159.4-197.0] | 82.5 [80.2-100.6] | 0.91× |
| `factorial_100` | 24.5 [23.5-25.5] | 164.5 [162.1-164.9] | 28.2 [27.7-29.9] | 0.87× |
| `attr_lookup` | 44.5 [43.4-44.8] | 174.5 [166.8-177.8] | 38.0 [37.7-39.9] | 1.17× |
| `object_tree` | 331.1 [327.3-340.3] | 181.2 [179.3-202.4] | 174.8 [169.5-181.3] | 1.89× |
| `list_ops` | 444.6 [435.4-452.1] | FAILED | 1672.5 [1589.7-1707.5] | 0.27× |
| `map_build` | 291.7 [288.2-299.1] | 297.6 [286.8-315.3] | 67.2 [63.3-70.3] | 4.34× |
| **Geomean vs CPython** | 1.19× (12) | 3.51× (11) | 1.00× | **1.19×** |

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
| `fib` | 2.7 | OK |
| `str_concat` | 3.0 | OK |
| `range_iterate` | 3.1 | OK |
| `tak` | 2.8 | OK |
| `fib30` | 2.8 | OK |
| `sum_loop` | 2.9 | OK |
| `factorial_100` | 3.1 | OK |
| `attr_lookup` | 3.0 | OK |
| `object_tree` | 3.6 | OK |
| `list_ops` | 3.5 | OK |
| `map_build` | 3.4 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 31.00 | [29.89-32.44] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 30.84 | [28.88-32.71] | MISSED |

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

