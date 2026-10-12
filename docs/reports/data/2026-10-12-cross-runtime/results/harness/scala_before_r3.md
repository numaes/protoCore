# protoScala benchmarks — 2026-10-12

- **Date:** 2026-10-12 19:04
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `dc8d4917`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 2.86 / 3.52 / 3.59 at start, 5.46 / 4.14 / 3.80 at midpoint, 6.47 / 4.51 / 3.94 at end (the run waited 60 s for the 1-minute load to drop below 4.0)
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
| `int_sum_loop` | 33.0 [32.7-33.9] | 225.3 [210.6-226.9] | 42.4 [38.7-46.6] | 0.78× |
| `fib` | 75.3 [70.7-108.7] | 258.9 [220.0-293.1] | 58.1 [47.5-64.9] | 1.30× |
| `str_concat` | 29.9 [28.5-33.9] | 219.5 [206.9-235.0] | 34.6 [33.2-36.0] | 0.87× |
| `range_iterate` | 33.5 [32.4-35.8] | 200.6 [187.9-225.7] | 36.1 [35.0-38.5] | 0.93× |
| `tak` | 36.5 [34.8-37.7] | 208.1 [194.8-230.0] | 35.4 [32.7-37.0] | 1.03× |
| `fib30` | 507.8 [494.5-527.1] | 197.0 [181.9-199.6] | 175.3 [169.0-184.0] | 2.90× |
| `sum_loop` | 85.8 [83.7-90.9] | 190.1 [176.5-204.6] | 92.8 [85.8-119.8] | 0.92× |
| `factorial_100` | 25.9 [24.4-39.5] | 207.3 [186.8-223.4] | 32.3 [29.7-34.2] | 0.80× |
| `attr_lookup` | 46.3 [45.8-49.7] | 204.9 [196.5-223.9] | 41.7 [40.5-44.7] | 1.11× |
| `object_tree` | 406.1 [399.6-424.9] | 213.3 [205.4-255.6] | 199.1 [189.3-233.4] | 2.04× |
| `list_ops` | 554.4 [544.2-570.0] | FAILED | 1918.3 [1888.8-2028.4] | 0.29× |
| `map_build` | 348.9 [327.8-374.7] | 378.0 [356.8-457.8] | 77.0 [72.3-120.6] | 4.53× |
| **Geomean vs CPython** | 1.15× (12) | 3.72× (11) | 1.00× | **1.15×** |

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
| `int_sum_loop` | 3.9 | OK |
| `fib` | 3.5 | OK |
| `str_concat` | 3.6 | OK |
| `range_iterate` | 3.6 | OK |
| `tak` | 3.5 | OK |
| `fib30` | 3.6 | OK |
| `sum_loop` | 3.6 | OK |
| `factorial_100` | 4.2 | OK |
| `attr_lookup` | 3.9 | OK |
| `object_tree` | 4.5 | OK |
| `list_ops` | 4.8 | OK |
| `map_build` | 4.4 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 32.42 | [29.88-41.50] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 33.88 | [30.07-48.06] | MISSED |

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

