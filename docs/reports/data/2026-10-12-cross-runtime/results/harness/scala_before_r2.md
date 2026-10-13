# protoScala benchmarks — 2026-10-12

- **Date:** 2026-10-12 22:03
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a60493af`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 1.71 / 2.56 / 7.98 at start, 2.51 / 2.65 / 7.70 at midpoint, 2.14 / 2.56 / 7.51 at end
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
| `int_sum_loop` | 29.6 [27.8-32.7] | 170.7 [162.7-177.9] | 32.6 [32.2-35.8] | 0.91× |
| `fib` | 62.8 [61.5-65.2] | 170.7 [167.4-176.6] | 39.3 [38.7-41.4] | 1.60× |
| `str_concat` | 26.7 [25.4-28.1] | 180.3 [167.4-190.6] | 29.6 [28.6-31.1] | 0.90× |
| `range_iterate` | 29.8 [28.6-31.9] | 165.3 [162.5-173.4] | 31.6 [31.2-34.4] | 0.94× |
| `tak` | 32.7 [32.3-36.7] | 180.2 [164.8-181.8] | 31.4 [31.1-32.9] | 1.04× |
| `fib30` | 476.8 [467.4-488.8] | 171.6 [165.1-190.0] | 154.6 [153.2-155.2] | 3.08× |
| `sum_loop` | 75.9 [75.5-76.5] | 177.4 [165.2-195.9] | 87.5 [80.4-88.2] | 0.87× |
| `factorial_100` | 24.0 [23.2-25.3] | 168.3 [160.8-186.5] | 27.9 [27.5-28.8] | 0.86× |
| `attr_lookup` | 44.2 [44.1-44.4] | 168.6 [164.6-189.6] | 38.1 [37.2-38.7] | 1.16× |
| `object_tree` | 363.0 [357.6-365.9] | 180.5 [177.7-196.3] | 173.4 [172.1-177.4] | 2.09× |
| `list_ops` | 489.1 [481.2-578.7] | FAILED | 1677.7 [1616.3-1718.5] | 0.29× |
| `map_build` | 288.4 [284.1-308.8] | 289.1 [282.9-314.2] | 64.2 [62.2-65.9] | 4.49× |
| **Geomean vs CPython** | 1.21× (12) | 3.56× (11) | 1.00× | **1.21×** |

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
| `int_sum_loop` | 3.0 | OK |
| `fib` | 2.8 | OK |
| `str_concat` | 3.2 | OK |
| `range_iterate` | 3.0 | OK |
| `tak` | 2.8 | OK |
| `fib30` | 2.8 | OK |
| `sum_loop` | 3.0 | OK |
| `factorial_100` | 3.1 | OK |
| `attr_lookup` | 3.0 | OK |
| `object_tree` | 3.6 | OK |
| `list_ops` | 3.5 | OK |
| `map_build` | 3.4 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 31.14 | [29.43-33.83] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 30.89 | [29.26-32.79] | MISSED |

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

