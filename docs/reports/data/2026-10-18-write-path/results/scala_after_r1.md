# protoScala benchmarks — 2026-10-19

- **Date:** 2026-10-19 01:51
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a6946796`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 2.52 / 2.51 / 2.36 at start, 3.47 / 2.87 / 2.50 at midpoint, 2.84 / 2.78 / 2.48 at end
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
| `int_sum_loop` | 31.0 [30.5-32.4] | 178.8 [169.2-181.5] | 35.0 [33.3-36.0] | 0.89× |
| `fib` | 63.0 [61.7-64.3] | 176.5 [170.0-193.5] | 41.5 [39.8-42.5] | 1.52× |
| `str_concat` | 27.1 [25.3-28.4] | 192.6 [181.3-203.8] | 31.1 [29.3-32.1] | 0.87× |
| `range_iterate` | 30.8 [30.6-31.3] | 181.9 [165.6-191.1] | 33.6 [32.5-34.4] | 0.91× |
| `tak` | 32.9 [32.5-35.3] | 186.5 [168.3-194.4] | 32.8 [31.1-33.5] | 1.00× |
| `fib30` | 487.4 [481.6-498.8] | 173.6 [170.9-186.8] | 157.8 [153.3-164.9] | 3.09× |
| `sum_loop` | 77.3 [74.7-81.5] | 179.1 [164.1-184.8] | 84.0 [81.7-88.1] | 0.92× |
| `factorial_100` | 25.3 [23.3-28.8] | 182.2 [167.1-201.1] | 29.1 [29.0-37.6] | 0.87× |
| `attr_lookup` | 44.7 [42.7-45.7] | 183.9 [175.4-196.5] | 39.2 [38.0-39.8] | 1.14× |
| `object_tree` | 337.7 [332.2-351.3] | 191.3 [181.7-192.9] | 176.1 [175.5-181.6] | 1.92× |
| `list_ops` | 443.8 [434.1-462.5] | FAILED | 1698.7 [1666.3-1716.4] | 0.26× |
| `map_build` | 298.8 [283.2-308.7] | 309.3 [295.6-339.8] | 65.5 [64.8-66.4] | 4.56× |
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
| `int_sum_loop` | 3.2 | OK |
| `fib` | 2.9 | OK |
| `str_concat` | 3.1 | OK |
| `range_iterate` | 3.2 | OK |
| `tak` | 3.0 | OK |
| `fib30` | 3.1 | OK |
| `sum_loop` | 3.2 | OK |
| `factorial_100` | 3.2 | OK |
| `attr_lookup` | 3.5 | OK |
| `object_tree` | 4.0 | OK |
| `list_ops` | 3.8 | OK |
| `map_build` | 3.8 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 30.66 | [28.50-32.08] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 31.50 | [29.10-47.16] | MISSED |

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

