# protoScala benchmarks — 2026-10-19

- **Date:** 2026-10-19 01:45
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a6946796`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 2.31 / 2.07 / 2.18 at start, 2.54 / 2.24 / 2.24 at midpoint, 2.18 / 2.18 / 2.22 at end
- **Method:** 2 warmup + 5 timed runs per cell, interleaved across runtimes (round-robin: one sample of each runtime, then the next, so ambient load hits every column alike); median wall-clock of a cold process (start-up included), spread reported as `[min-max]` beside every median. This machine is a daily-driver desktop (VS Code, Chrome and PyCharm run throughout) and this runner gates on nothing: the load averages above are context, not a quietness claim; a claim that the host was quiet has to come from an `mpstat -P ALL` idle reading taken immediately before the run and recorded in the report that cites it. Ratios to CPython are the primary result, absolute milliseconds are indicative only. Every run's printed result is verified; a wrong result, non-zero exit or timeout marks the cell FAILED and it is excluded from every aggregate.

### Runtimes

| Column | Binary | Build / version | Twin programs |
|---|---|---|---|
| protoScala | `/home/gamarino/Documentos/proyectos/protoScala/build_cmp2146/protoscala` | Release | `benchmarks/comparable/*.scala` |
| Scala (JVM) | `/home/gamarino/Documentos/proyectos/tools/scala3-dl/scala3-3.9.0-x86_64-pc-linux` + `java -XX:-UsePerfData` | Scala compiler version 3.9.0 -- Copyright 2002-2026, LAMP/EPFL; openjdk version "25.0.4.1" 2026-08-18 | the same `.scala` files, compiled once with `scalac` (untimed) |
| CPython | `/usr/local/bin/python3.14` | Python 3.14.0 | protoPython's `benchmarks/*.py` (with `BENCH_N`), or `benchmarks/comparable/python/*.py` |

## Comparable workloads

Median wall-clock in milliseconds per cold process. `—`: the runtime has no twin of that workload. The last column is protoScala (canonical build) ÷ CPython (>1: protoScala slower). The geomean row gives, per column, the geometric mean of its ratio to CPython over the rows where both cells verified (row count in parentheses).

| Workload | protoScala (ms, median [min-max]) | Scala (JVM) (ms, median [min-max]) | CPython (ms, median [min-max]) | protoScala ÷ CPython |
|---|---:|---:|---:|---:|
| `int_sum_loop` | 30.0 [29.2-32.0] | 174.1 [171.9-206.3] | 34.2 [32.9-34.6] | 0.88× |
| `fib` | 62.3 [60.4-64.1] | 186.2 [166.0-190.7] | 40.0 [39.8-40.8] | 1.56× |
| `str_concat` | 27.0 [26.5-28.2] | 181.2 [171.8-198.5] | 29.1 [28.9-31.1] | 0.93× |
| `range_iterate` | 29.8 [28.8-30.5] | 174.5 [167.4-180.0] | 32.5 [32.3-33.3] | 0.91× |
| `tak` | 34.2 [31.5-36.1] | 176.9 [165.9-190.3] | 31.9 [31.7-33.0] | 1.07× |
| `fib30` | 476.1 [470.2-480.9] | 203.5 [181.3-446.6] | 158.9 [155.3-165.2] | 3.00× |
| `sum_loop` | 76.0 [74.2-81.8] | 190.8 [174.1-192.6] | 84.6 [82.4-91.8] | 0.90× |
| `factorial_100` | 25.0 [24.1-25.6] | 172.7 [165.4-195.8] | 29.0 [28.4-43.3] | 0.86× |
| `attr_lookup` | 44.7 [42.8-46.8] | 180.3 [173.6-216.3] | 39.2 [37.5-40.8] | 1.14× |
| `object_tree` | 331.4 [324.4-349.3] | 203.6 [181.4-209.8] | 177.5 [175.3-183.2] | 1.87× |
| `list_ops` | 445.9 [443.6-451.3] | FAILED | 1700.5 [1658.1-1716.3] | 0.26× |
| `map_build` | 294.3 [285.3-315.4] | 308.3 [298.8-324.3] | 65.9 [64.3-69.1] | 4.46× |
| **Geomean vs CPython** | 1.18× (12) | 3.72× (11) | 1.00× | **1.18×** |

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
| `fib` | 3.1 | OK |
| `str_concat` | 3.3 | OK |
| `range_iterate` | 3.2 | OK |
| `tak` | 3.1 | OK |
| `fib30` | 3.1 | OK |
| `sum_loop` | 3.2 | OK |
| `factorial_100` | 3.2 | OK |
| `attr_lookup` | 3.3 | OK |
| `object_tree` | 3.8 | OK |
| `list_ops` | 3.8 | OK |
| `map_build` | 3.7 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 30.61 | [28.76-32.61] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 30.08 | [29.15-32.82] | MISSED |

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

