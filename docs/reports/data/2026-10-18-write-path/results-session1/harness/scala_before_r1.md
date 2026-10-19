# protoScala benchmarks — 2026-10-19

- **Date:** 2026-10-19 00:13
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a6946796`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 2.06 / 2.36 / 3.47 at start, 2.66 / 2.50 / 3.45 at midpoint, 2.24 / 2.41 / 3.38 at end
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
| `int_sum_loop` | 30.9 [30.1-32.8] | 194.7 [177.3-209.6] | 35.1 [34.3-38.2] | 0.88× |
| `fib` | 64.2 [61.8-67.5] | 187.6 [177.7-215.5] | 42.3 [41.1-45.6] | 1.52× |
| `str_concat` | 28.3 [27.6-30.0] | 183.1 [179.4-201.7] | 30.2 [29.7-31.1] | 0.94× |
| `range_iterate` | 31.8 [31.1-32.1] | 183.8 [178.3-203.3] | 33.8 [32.6-37.0] | 0.94× |
| `tak` | 35.0 [33.7-35.8] | 183.4 [180.3-197.8] | 33.5 [32.3-34.2] | 1.04× |
| `fib30` | 519.4 [492.8-525.0] | 191.1 [176.0-198.1] | 167.5 [164.1-181.7] | 3.10× |
| `sum_loop` | 87.5 [81.0-90.6] | 193.7 [174.9-205.9] | 85.6 [83.9-92.8] | 1.02× |
| `factorial_100` | 24.8 [24.1-26.0] | 183.1 [175.3-187.6] | 31.0 [28.8-32.2] | 0.80× |
| `attr_lookup` | 45.1 [44.2-48.5] | 183.5 [178.2-189.5] | 41.1 [39.1-42.6] | 1.10× |
| `object_tree` | 342.9 [336.7-448.2] | 218.6 [187.5-230.5] | 187.2 [181.1-225.5] | 1.83× |
| `list_ops` | 465.4 [453.0-485.8] | FAILED | 1808.2 [1744.3-1885.2] | 0.26× |
| `map_build` | 316.9 [312.6-323.7] | 330.0 [322.9-356.5] | 72.4 [69.8-77.5] | 4.38× |
| **Geomean vs CPython** | 1.18× (12) | 3.67× (11) | 1.00× | **1.18×** |

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
| `str_concat` | 3.5 | OK |
| `range_iterate` | 3.3 | OK |
| `tak` | 3.2 | OK |
| `fib30` | 3.1 | OK |
| `sum_loop` | 3.4 | OK |
| `factorial_100` | 3.4 | OK |
| `attr_lookup` | 3.4 | OK |
| `object_tree` | 4.1 | OK |
| `list_ops` | 3.8 | OK |
| `map_build` | 3.9 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 31.73 | [28.95-43.95] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 31.39 | [30.34-43.21] | MISSED |

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

