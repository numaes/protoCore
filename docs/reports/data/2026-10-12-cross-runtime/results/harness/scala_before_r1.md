# protoScala benchmarks — 2026-10-12

- **Date:** 2026-10-12 21:55
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a60493af`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 1.35 / 2.38 / 11.41 at start, 3.22 / 2.75 / 11.01 at midpoint, 2.73 / 2.68 / 10.73 at end
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
| `int_sum_loop` | 32.2 [31.2-33.0] | 193.2 [179.4-214.9] | 36.7 [36.0-41.9] | 0.88× |
| `fib` | 64.3 [63.9-67.1] | 182.5 [173.1-192.2] | 44.8 [42.9-47.6] | 1.43× |
| `str_concat` | 28.4 [28.4-28.6] | 183.4 [181.6-197.8] | 31.8 [31.5-33.1] | 0.89× |
| `range_iterate` | 31.1 [30.3-35.0] | 185.4 [173.6-186.8] | 34.7 [34.3-38.5] | 0.89× |
| `tak` | 33.2 [32.4-35.1] | 174.5 [164.1-206.8] | 31.7 [30.5-32.0] | 1.05× |
| `fib30` | 493.8 [464.6-503.4] | 184.9 [168.3-201.2] | 152.3 [149.4-157.9] | 3.24× |
| `sum_loop` | 75.8 [73.7-79.4] | 176.9 [161.4-198.5] | 82.3 [80.2-84.7] | 0.92× |
| `factorial_100` | 24.1 [23.5-25.7] | 166.0 [157.6-181.6] | 28.3 [27.6-29.2] | 0.85× |
| `attr_lookup` | 44.7 [42.9-46.8] | 174.9 [166.5-177.2] | 38.2 [37.2-41.0] | 1.17× |
| `object_tree` | 365.8 [358.4-394.3] | 185.6 [179.4-215.9] | 181.5 [175.6-207.5] | 2.02× |
| `list_ops` | 499.4 [484.1-503.5] | FAILED | 1652.4 [1619.7-1687.8] | 0.30× |
| `map_build` | 290.1 [286.0-302.1] | 294.0 [282.3-328.1] | 63.9 [61.8-66.4] | 4.54× |
| **Geomean vs CPython** | 1.20× (12) | 3.57× (11) | 1.00× | **1.20×** |

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
| `fib` | 2.9 | OK |
| `str_concat` | 2.9 | OK |
| `range_iterate` | 3.0 | OK |
| `tak` | 2.8 | OK |
| `fib30` | 2.9 | OK |
| `sum_loop` | 3.1 | OK |
| `factorial_100` | 3.1 | OK |
| `attr_lookup` | 3.1 | OK |
| `object_tree` | 3.8 | OK |
| `list_ops` | 4.7 | OK |
| `map_build` | 3.8 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 30.97 | [29.35-33.06] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 30.83 | [29.60-32.02] | MISSED |

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

