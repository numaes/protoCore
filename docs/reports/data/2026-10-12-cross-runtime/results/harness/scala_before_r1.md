# protoScala benchmarks — 2026-10-12

- **Date:** 2026-10-12 18:46
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `dc8d4917`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 2.55 / 3.54 / 4.03 at start, 3.05 / 3.51 / 3.99 at midpoint, 2.43 / 3.32 / 3.91 at end
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
| `int_sum_loop` | 30.5 [29.7-31.0] | 184.9 [181.9-197.7] | 35.3 [33.6-38.7] | 0.86× |
| `fib` | 65.0 [64.0-67.1] | 184.1 [177.2-194.3] | 43.3 [42.4-44.3] | 1.50× |
| `str_concat` | 28.2 [27.2-28.3] | 193.5 [177.2-203.1] | 31.2 [28.8-33.1] | 0.90× |
| `range_iterate` | 31.3 [29.9-34.0] | 185.6 [173.2-195.3] | 34.4 [32.7-35.0] | 0.91× |
| `tak` | 34.4 [33.1-38.0] | 183.7 [178.1-197.6] | 33.2 [31.8-36.1] | 1.03× |
| `fib30` | 509.7 [497.8-520.0] | 180.1 [170.9-193.5] | 164.9 [163.1-181.1] | 3.09× |
| `sum_loop` | 82.8 [79.1-87.2] | 189.1 [167.9-197.1] | 89.1 [85.8-95.0] | 0.93× |
| `factorial_100` | 25.1 [24.2-26.1] | 179.3 [174.8-202.9] | 30.1 [29.2-32.3] | 0.84× |
| `attr_lookup` | 46.4 [45.9-50.2] | 187.8 [182.0-188.8] | 40.6 [38.9-43.3] | 1.14× |
| `object_tree` | 380.0 [371.8-395.7] | 189.5 [189.2-207.0] | 183.7 [178.4-189.1] | 2.07× |
| `list_ops` | 514.8 [502.6-535.9] | FAILED | 1739.8 [1722.3-1835.7] | 0.30× |
| `map_build` | 298.9 [294.4-314.3] | 325.3 [306.3-333.8] | 70.4 [66.1-73.4] | 4.25× |
| **Geomean vs CPython** | 1.19× (12) | 3.58× (11) | 1.00× | **1.19×** |

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
| `int_sum_loop` | 3.3 | OK |
| `fib` | 3.0 | OK |
| `str_concat` | 3.3 | OK |
| `range_iterate` | 3.2 | OK |
| `tak` | 3.2 | OK |
| `fib30` | 3.2 | OK |
| `sum_loop` | 3.3 | OK |
| `factorial_100` | 3.2 | OK |
| `attr_lookup` | 3.3 | OK |
| `object_tree` | 4.2 | OK |
| `list_ops` | 3.7 | OK |
| `map_build` | 3.8 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 31.39 | [29.15-33.40] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 31.02 | [29.48-35.52] | MISSED |

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

