# protoScala benchmarks — 2026-10-19

- **Date:** 2026-10-19 00:36
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a6946796`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 2.26 / 2.94 / 3.18 at start, 2.52 / 2.93 / 3.16 at midpoint, 2.43 / 2.87 / 3.14 at end
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
| `int_sum_loop` | 31.3 [31.3-31.9] | 187.4 [178.2-210.6] | 34.4 [33.9-38.9] | 0.91× |
| `fib` | 64.3 [62.6-70.5] | 178.8 [178.3-194.7] | 43.9 [41.8-46.7] | 1.46× |
| `str_concat` | 28.0 [26.6-30.4] | 189.3 [172.7-270.9] | 32.3 [29.2-41.9] | 0.87× |
| `range_iterate` | 30.8 [30.2-32.6] | 187.9 [182.6-192.9] | 35.4 [32.0-39.2] | 0.87× |
| `tak` | 33.5 [33.3-34.4] | 182.2 [177.0-196.6] | 33.7 [32.1-36.6] | 0.99× |
| `fib30` | 500.0 [478.3-536.1] | 186.5 [184.1-190.8] | 168.1 [166.7-171.3] | 2.97× |
| `sum_loop` | 80.5 [79.4-83.7] | 183.1 [173.9-198.0] | 85.5 [84.0-89.3] | 0.94× |
| `factorial_100` | 25.1 [24.5-26.2] | 182.7 [169.8-189.1] | 31.1 [29.2-32.5] | 0.81× |
| `attr_lookup` | 45.8 [44.2-47.4] | 180.9 [178.9-191.3] | 40.6 [39.1-41.6] | 1.13× |
| `object_tree` | 338.0 [332.1-342.6] | 193.7 [188.9-212.2] | 187.1 [182.7-199.3] | 1.81× |
| `list_ops` | 464.4 [453.4-476.6] | FAILED | 1727.0 [1684.3-1779.3] | 0.27× |
| `map_build` | 305.8 [301.7-363.4] | 327.9 [305.9-340.9] | 69.2 [67.0-72.1] | 4.42× |
| **Geomean vs CPython** | 1.15× (12) | 3.55× (11) | 1.00× | **1.15×** |

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
| `int_sum_loop` | 3.2 | OK |
| `fib` | 3.1 | OK |
| `str_concat` | 3.5 | OK |
| `range_iterate` | 3.3 | OK |
| `tak` | 3.1 | OK |
| `fib30` | 3.2 | OK |
| `sum_loop` | 3.3 | OK |
| `factorial_100` | 3.2 | OK |
| `attr_lookup` | 3.4 | OK |
| `object_tree` | 4.0 | OK |
| `list_ops` | 3.9 | OK |
| `map_build` | 3.8 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 30.93 | [28.80-32.55] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 30.29 | [29.02-32.83] | MISSED |

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

