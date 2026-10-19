# protoScala benchmarks — 2026-10-19

- **Date:** 2026-10-19 00:25
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a6946796`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 2.47 / 2.86 / 3.21 at start, 3.31 / 3.03 / 3.25 at midpoint, 3.13 / 3.00 / 3.23 at end
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
| `int_sum_loop` | 31.9 [30.5-32.5] | 184.6 [180.0-197.2] | 36.5 [34.5-37.3] | 0.87× |
| `fib` | 66.1 [63.9-69.6] | 186.7 [171.8-203.6] | 42.7 [41.5-43.1] | 1.55× |
| `str_concat` | 28.8 [27.4-31.0] | 195.6 [183.6-203.0] | 32.6 [30.2-34.5] | 0.88× |
| `range_iterate` | 31.3 [31.3-31.6] | 182.8 [175.9-193.7] | 33.1 [32.8-36.8] | 0.95× |
| `tak` | 35.0 [33.9-35.9] | 199.3 [174.4-211.4] | 34.7 [33.7-38.1] | 1.01× |
| `fib30` | 522.5 [515.1-557.5] | 192.7 [180.2-207.9] | 174.6 [164.2-181.2] | 2.99× |
| `sum_loop` | 85.1 [79.9-88.7] | 193.1 [182.2-197.2] | 89.7 [88.1-98.8] | 0.95× |
| `factorial_100` | 25.7 [23.9-27.6] | 187.6 [171.1-190.2] | 31.1 [30.1-31.8] | 0.83× |
| `attr_lookup` | 46.1 [45.1-46.6] | 184.8 [180.2-210.3] | 40.1 [39.4-41.3] | 1.15× |
| `object_tree` | 349.3 [342.2-366.0] | 205.6 [197.4-208.8] | 185.2 [181.1-188.6] | 1.89× |
| `list_ops` | 467.0 [445.4-479.9] | FAILED | 1797.6 [1737.5-1888.3] | 0.26× |
| `map_build` | 317.0 [302.6-415.4] | 321.1 [302.9-347.8] | 76.8 [72.7-91.2] | 4.13× |
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
| `int_sum_loop` | 3.2 | OK |
| `fib` | 3.1 | OK |
| `str_concat` | 3.3 | OK |
| `range_iterate` | 3.2 | OK |
| `tak` | 3.2 | OK |
| `fib30` | 3.3 | OK |
| `sum_loop` | 3.4 | OK |
| `factorial_100` | 3.5 | OK |
| `attr_lookup` | 3.6 | OK |
| `object_tree` | 4.0 | OK |
| `list_ops` | 3.8 | OK |
| `map_build` | 3.9 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 31.84 | [30.75-34.47] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 31.00 | [28.88-33.38] | MISSED |

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

