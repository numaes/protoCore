# protoScala benchmarks — 2026-10-19

- **Date:** 2026-10-19 02:07
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a6946796`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 3.20 / 3.53 / 3.10 at start, 4.24 / 3.85 / 3.24 at midpoint, 3.97 / 3.82 / 3.25 at end
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
| `int_sum_loop` | 30.5 [29.1-31.5] | 175.5 [172.3-186.9] | 34.0 [33.4-35.1] | 0.90× |
| `fib` | 63.3 [62.3-64.8] | 183.0 [170.5-210.6] | 41.5 [40.0-50.8] | 1.52× |
| `str_concat` | 27.3 [26.1-29.4] | 190.4 [183.4-194.6] | 31.1 [29.8-39.3] | 0.88× |
| `range_iterate` | 31.1 [29.6-32.0] | 179.6 [169.7-191.0] | 33.0 [32.3-33.7] | 0.94× |
| `tak` | 35.5 [33.0-37.3] | 175.4 [174.4-197.8] | 32.4 [31.7-36.5] | 1.10× |
| `fib30` | 495.5 [482.5-508.6] | 184.9 [175.7-216.0] | 162.0 [157.2-167.9] | 3.06× |
| `sum_loop` | 78.4 [75.5-83.3] | 187.7 [176.3-202.4] | 85.8 [80.9-90.3] | 0.91× |
| `factorial_100` | 24.6 [24.1-25.6] | 172.8 [168.5-192.4] | 29.3 [29.2-30.9] | 0.84× |
| `attr_lookup` | 45.3 [43.1-57.2] | 185.0 [177.5-201.3] | 39.4 [39.0-40.8] | 1.15× |
| `object_tree` | 333.8 [333.0-338.5] | 196.1 [180.7-202.7] | 179.0 [177.4-196.7] | 1.86× |
| `list_ops` | 452.2 [436.4-477.5] | FAILED | 1722.8 [1675.0-1784.2] | 0.26× |
| `map_build` | 294.8 [284.9-308.2] | 306.7 [295.0-318.3] | 66.6 [64.5-70.7] | 4.43× |
| **Geomean vs CPython** | 1.18× (12) | 3.63× (11) | 1.00× | **1.18×** |

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
| `fib` | 3.0 | OK |
| `str_concat` | 3.2 | OK |
| `range_iterate` | 3.2 | OK |
| `tak` | 3.3 | OK |
| `fib30` | 3.1 | OK |
| `sum_loop` | 3.1 | OK |
| `factorial_100` | 3.4 | OK |
| `attr_lookup` | 3.2 | OK |
| `object_tree` | 3.9 | OK |
| `list_ops` | 3.7 | OK |
| `map_build` | 3.8 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 30.17 | [28.37-32.09] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 30.39 | [27.69-32.94] | MISSED |

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

