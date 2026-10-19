# protoScala benchmarks — 2026-10-19

- **Date:** 2026-10-19 00:30
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a6946796`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 2.58 / 3.12 / 3.26 at start, 3.82 / 3.40 / 3.35 at midpoint, 3.76 / 3.41 / 3.35 at end
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
| `int_sum_loop` | 30.9 [30.4-40.3] | 205.5 [198.0-364.4] | 35.5 [34.6-43.3] | 0.87× |
| `fib` | 66.9 [63.3-69.2] | 184.2 [180.0-192.5] | 42.6 [41.4-44.3] | 1.57× |
| `str_concat` | 27.0 [26.4-30.5] | 200.2 [172.4-218.0] | 33.2 [30.2-41.3] | 0.81× |
| `range_iterate` | 30.7 [29.9-33.6] | 195.9 [183.1-247.4] | 34.4 [32.6-38.0] | 0.89× |
| `tak` | 34.5 [32.7-35.9] | 185.1 [180.8-200.4] | 34.2 [33.9-38.1] | 1.01× |
| `fib30` | 519.2 [500.1-601.0] | 191.2 [180.1-208.3] | 166.1 [165.0-179.2] | 3.13× |
| `sum_loop` | 80.1 [78.2-85.3] | 187.3 [182.5-198.3] | 90.8 [86.7-92.8] | 0.88× |
| `factorial_100` | 26.2 [25.4-37.9] | 184.4 [183.4-192.6] | 30.1 [29.1-30.6] | 0.87× |
| `attr_lookup` | 48.8 [46.6-50.1] | 197.9 [189.0-214.3] | 45.0 [39.3-50.1] | 1.08× |
| `object_tree` | 350.2 [342.2-402.7] | 191.9 [189.1-212.0] | 186.8 [181.2-192.9] | 1.87× |
| `list_ops` | 469.5 [453.2-479.7] | FAILED | 1758.7 [1742.2-1798.0] | 0.27× |
| `map_build` | 304.0 [300.7-309.4] | 317.1 [309.7-352.8] | 71.2 [68.4-72.4] | 4.27× |
| **Geomean vs CPython** | 1.15× (12) | 3.61× (11) | 1.00× | **1.15×** |

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
| `fib` | 3.0 | OK |
| `str_concat` | 3.3 | OK |
| `range_iterate` | 3.4 | OK |
| `tak` | 3.2 | OK |
| `fib30` | 3.1 | OK |
| `sum_loop` | 3.3 | OK |
| `factorial_100` | 3.3 | OK |
| `attr_lookup` | 3.4 | OK |
| `object_tree` | 4.2 | OK |
| `list_ops` | 3.8 | OK |
| `map_build` | 3.9 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 30.78 | [29.32-32.89] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 31.20 | [28.68-33.64] | MISSED |

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

