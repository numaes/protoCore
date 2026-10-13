# protoScala benchmarks — 2026-10-12

- **Date:** 2026-10-12 21:59
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a60493af`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 3.06 / 3.04 / 9.63 at start, 3.69 / 3.30 / 9.34 at midpoint, 2.83 / 3.13 / 9.09 at end
- **Method:** 2 warmup + 5 timed runs per cell, interleaved across runtimes (round-robin: one sample of each runtime, then the next, so ambient load hits every column alike); median wall-clock of a cold process (start-up included), spread reported as `[min-max]` beside every median. This machine is a daily-driver desktop (VS Code, Chrome and PyCharm run throughout) and this runner gates on nothing: the load averages above are context, not a quietness claim; a claim that the host was quiet has to come from an `mpstat -P ALL` idle reading taken immediately before the run and recorded in the report that cites it. Ratios to CPython are the primary result, absolute milliseconds are indicative only. Every run's printed result is verified; a wrong result, non-zero exit or timeout marks the cell FAILED and it is excluded from every aggregate.

### Runtimes

| Column | Binary | Build / version | Twin programs |
|---|---|---|---|
| protoScala | `$WS/protoScala/build_pkg_oct12/protoscala` | Release | `benchmarks/comparable/*.scala` |
| Scala (JVM) | `$WS/tools/scala3-dl/scala3-3.9.0-x86_64-pc-linux` + `java -XX:-UsePerfData` | Scala compiler version 3.9.0 -- Copyright 2002-2026, LAMP/EPFL; openjdk version "25.0.4.1" 2026-08-18 | the same `.scala` files, compiled once with `scalac` (untimed) |
| CPython | `/usr/local/bin/python3.14` | Python 3.14.0 | protoPython's `benchmarks/*.py` (with `BENCH_N`), or `benchmarks/comparable/python/*.py` |

## Comparable workloads

Median wall-clock in milliseconds per cold process. `—`: the runtime has no twin of that workload. The last column is protoScala (canonical build) ÷ CPython (>1: protoScala slower). The geomean row gives, per column, the geometric mean of its ratio to CPython over the rows where both cells verified (row count in parentheses).

| Workload | protoScala (ms, median [min-max]) | Scala (JVM) (ms, median [min-max]) | CPython (ms, median [min-max]) | protoScala ÷ CPython |
|---|---:|---:|---:|---:|
| `int_sum_loop` | 30.0 [28.2-30.2] | 164.3 [160.1-174.8] | 32.1 [31.6-33.1] | 0.93× |
| `fib` | 60.3 [59.8-64.2] | 163.7 [161.2-174.9] | 40.2 [38.8-43.1] | 1.50× |
| `str_concat` | 26.8 [25.9-37.5] | 172.9 [167.0-197.8] | 28.3 [27.4-28.5] | 0.95× |
| `range_iterate` | 30.0 [29.0-30.7] | 166.5 [159.8-181.2] | 31.8 [31.3-32.7] | 0.94× |
| `tak` | 33.4 [32.3-34.0] | 182.8 [160.6-191.6] | 31.8 [30.6-33.5] | 1.05× |
| `fib30` | 463.8 [457.1-485.1] | 174.2 [162.5-185.3] | 151.5 [150.1-155.5] | 3.06× |
| `sum_loop` | 75.0 [73.9-81.1] | 176.8 [165.6-189.2] | 82.6 [81.9-85.8] | 0.91× |
| `factorial_100` | 25.5 [23.7-26.4] | 168.9 [163.2-171.1] | 28.3 [27.8-29.7] | 0.90× |
| `attr_lookup` | 44.8 [42.7-46.1] | 172.5 [165.8-187.5] | 38.7 [37.4-41.2] | 1.16× |
| `object_tree` | 332.7 [325.7-338.0] | 183.8 [181.1-202.1] | 176.9 [173.1-181.8] | 1.88× |
| `list_ops` | 445.1 [439.1-456.6] | FAILED | 1608.5 [1607.1-1645.6] | 0.28× |
| `map_build` | 290.2 [284.9-295.8] | 300.0 [281.1-309.7] | 65.4 [64.1-72.8] | 4.44× |
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
| `int_sum_loop` | 3.2 | OK |
| `fib` | 2.8 | OK |
| `str_concat` | 3.0 | OK |
| `range_iterate` | 3.0 | OK |
| `tak` | 2.8 | OK |
| `fib30` | 2.8 | OK |
| `sum_loop` | 2.9 | OK |
| `factorial_100` | 2.9 | OK |
| `attr_lookup` | 3.0 | OK |
| `object_tree` | 3.6 | OK |
| `list_ops` | 3.5 | OK |
| `map_build` | 3.5 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 31.08 | [30.11-32.81] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 31.16 | [29.89-43.81] | MISSED |

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

