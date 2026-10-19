# protoScala benchmarks — 2026-10-19

- **Date:** 2026-10-19 01:56
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a6946796`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 2.40 / 2.84 / 2.59 at start, 3.07 / 2.98 / 2.66 at midpoint, 2.48 / 2.84 / 2.62 at end
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
| `int_sum_loop` | 30.2 [29.3-31.4] | 182.2 [174.2-203.7] | 33.5 [32.3-36.4] | 0.90× |
| `fib` | 62.4 [60.5-67.4] | 181.0 [174.6-195.6] | 41.4 [40.7-44.4] | 1.51× |
| `str_concat` | 27.6 [26.4-28.1] | 186.3 [173.9-197.6] | 29.2 [29.0-31.7] | 0.94× |
| `range_iterate` | 31.5 [30.1-32.7] | 178.1 [174.9-182.7] | 33.3 [32.0-36.2] | 0.95× |
| `tak` | 33.4 [32.7-35.7] | 178.0 [172.1-189.3] | 31.7 [31.2-33.4] | 1.05× |
| `fib30` | 484.0 [478.1-520.1] | 184.4 [170.9-196.1] | 159.0 [156.3-175.1] | 3.04× |
| `sum_loop` | 77.5 [76.1-80.2] | 184.5 [171.2-189.3] | 84.2 [81.6-89.9] | 0.92× |
| `factorial_100` | 25.7 [24.6-28.9] | 182.4 [172.5-213.6] | 30.1 [29.0-33.0] | 0.85× |
| `attr_lookup` | 44.7 [44.0-46.0] | 181.0 [171.6-193.3] | 39.3 [39.0-41.4] | 1.14× |
| `object_tree` | 341.8 [335.1-351.9] | 192.4 [183.0-220.3] | 179.4 [176.9-313.9] | 1.90× |
| `list_ops` | 451.8 [441.0-498.5] | FAILED | 1692.0 [1643.6-1725.9] | 0.27× |
| `map_build` | 291.0 [288.0-297.4] | 318.0 [290.5-342.0] | 67.9 [65.6-72.6] | 4.29× |
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
| `int_sum_loop` | 3.1 | OK |
| `fib` | 3.1 | OK |
| `str_concat` | 3.0 | OK |
| `range_iterate` | 3.1 | OK |
| `tak` | 2.9 | OK |
| `fib30` | 3.0 | OK |
| `sum_loop` | 3.2 | OK |
| `factorial_100` | 3.2 | OK |
| `attr_lookup` | 3.3 | OK |
| `object_tree` | 3.9 | OK |
| `list_ops` | 3.9 | OK |
| `map_build` | 3.9 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 30.86 | [28.66-42.48] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 31.09 | [28.85-35.67] | MISSED |

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

