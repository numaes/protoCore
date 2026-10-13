# protoScala benchmarks — 2026-10-12

- **Date:** 2026-10-12 22:11
- **Machine:** AMD Ryzen 5 5500U with Radeon Graphics — 6 cores, 12 logical CPUs; Linux 7.0.0-38-generic (x86_64)
- **protoScala:** commit `a60493af`; `build_release/protoscala` is Release; no `build_bench` Release build
- **Load average (1/5/15 min):** 1.27 / 1.77 / 5.45 at start, 2.64 / 2.11 / 5.35 at midpoint, 2.34 / 2.08 / 5.24 at end
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
| `int_sum_loop` | 29.7 [29.0-31.6] | 165.5 [161.8-182.4] | 32.0 [31.7-33.7] | 0.93× |
| `fib` | 61.4 [60.4-63.1] | 169.5 [159.8-183.8] | 39.5 [39.1-42.6] | 1.56× |
| `str_concat` | 26.5 [25.2-28.0] | 174.9 [167.7-186.5] | 28.9 [28.4-29.7] | 0.91× |
| `range_iterate` | 30.3 [29.6-31.7] | 165.7 [161.8-179.6] | 33.1 [31.6-46.0] | 0.91× |
| `tak` | 33.5 [33.3-34.0] | 167.3 [162.7-170.9] | 31.1 [30.3-32.9] | 1.08× |
| `fib30` | 479.3 [468.7-490.4] | 170.0 [163.5-182.2] | 155.0 [150.7-160.8] | 3.09× |
| `sum_loop` | 75.0 [74.7-80.1] | 169.2 [160.0-186.3] | 84.5 [81.6-88.3] | 0.89× |
| `factorial_100` | 25.2 [22.8-26.1] | 165.0 [159.1-178.3] | 28.8 [27.8-38.6] | 0.88× |
| `attr_lookup` | 45.0 [44.1-45.6] | 165.7 [162.7-181.7] | 37.4 [37.1-38.9] | 1.20× |
| `object_tree` | 364.5 [359.1-372.2] | 179.2 [176.1-203.8] | 174.6 [171.1-189.8] | 2.09× |
| `list_ops` | 488.1 [473.2-501.4] | FAILED | 1658.2 [1632.9-1739.4] | 0.29× |
| `map_build` | 288.8 [281.5-302.0] | 300.6 [278.2-332.4] | 64.0 [61.6-66.2] | 4.51× |
| **Geomean vs CPython** | 1.22× (12) | 3.50× (11) | 1.00× | **1.22×** |

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
| `int_sum_loop` | 2.9 | OK |
| `fib` | 2.7 | OK |
| `str_concat` | 2.9 | OK |
| `range_iterate` | 2.9 | OK |
| `tak` | 2.9 | OK |
| `fib30` | 3.0 | OK |
| `sum_loop` | 2.9 | OK |
| `factorial_100` | 3.0 | OK |
| `attr_lookup` | 3.0 | OK |
| `object_tree` | 3.6 | OK |
| `list_ops` | 3.4 | OK |
| `map_build` | 3.5 | OK |

## Cold start

`benchmarks/cold-start.sh <binary> <runs>` (self-verifying; target < 25 ms, DESIGN §1). Verdict: **MET** if every sample (including the worst) is below the target; **MISSED** if the median is at or above it; **STRADDLES** if the median is below the target but the spread crosses it (the worst sample is at or above 25 ms) — neither met nor missed, and reported as such rather than picking a side:

| Build | Case | Runs | Verified | Median (ms) | Spread [min-max] (ms) | Verdict |
|---|---|---:|---:|---:|---:|---|
| protoScala (Release) | script | 21 | 21 | 30.61 | [29.96-32.19] | MISSED |
| protoScala (Release) | repl | 21 | 21 | 30.83 | [29.66-31.66] | MISSED |

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

