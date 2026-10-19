# protoST benchmarks — 2026-10-19

- **Host:** AMD Ryzen 5 5500U with Radeon Graphics, 12 logical CPUs, Linux 7.0.0-38-generic
- **Load average (1 min):** 2.25 before, 4.85 after
- **protoST:** protoST 0.6.6 at commit `764b84e`
- **CPython:** Python 3.14.0
- **Method:** 2 warmup + 5 timed runs per measurement; median wall-clock, min–max in parentheses. Every run verified its result (`VERIFIED` / `BENCH_RESULT`); a failed run fails the harness.

## Start-up

A process that evaluates one expression: protoST 23.5 ms (23.1–24.3), CPython 28.6 ms (28.3–30.9). The comparable workloads below use small N, so start-up is part of every time.

## Comparable workloads

Same algorithm, same N and same verified result as the protoPython twin. `Ratio` is protoST ÷ CPython on whole-process time (>1 means protoST is slower). At these N the process start-up is a large share of every time, so `Work ratio` also compares the times with each runtime's measured start-up subtracted: the cost of the work itself.

| Benchmark | N | protoST ms | CPython ms | Ratio | Work ratio |
|---|---:|---:|---:|---:|---:|
| int_sum_loop | 100000 | 44.8 (42.9–45.4) | 32.8 (31.7–33.7) | 1.37× | 5.0× |
| fib | 25 | 358.8 (354.4–360.7) | 39.9 (39.3–40.6) | 8.99× | 29.5× |
| list_append | 10000 | 49.6 (47.3–52.6) | 30.3 (29.4–32.1) | 1.64× | 15.2× |
| str_concat | 2000 | 27.6 (26.9–28.7) | 29.5 (28.2–30.0) | 0.94× | 4.3× |
| attr_lookup | 100000 | 164.7 (162.0–174.2) | 37.9 (37.7–40.4) | 4.34× | 15.1× |
| range_iterate | 100000 | 75.4 (73.3–75.8) | 32.5 (31.5–33.1) | 2.32× | 13.3× |
| exception_latency | 50000 | 988.6 (950.8–999.4) | 45.3 (44.9–46.8) | 21.82× | 57.6× |
| **Geomean** | | | | **3.29×** | **14.1×** |

## Actors

- **Parallel speedup** (12 CPU-bound actors): 606.7 ms with one worker, 271.9 ms with the default pool: **2.23×**.
- **Cooperative yield:** 1000 actors, each parked on a `wait`, on 2 worker threads: 473.0 ms.
- **Message throughput:** 2000 round-trip sends (send + `wait`): 68.3 ms, about 29,290 round trips per second including start-up.

Worker scaling of `saturation_big.st` (32 actors × 50 messages of CPU work):

| Workers | ms | Speedup vs 1 |
|---:|---:|---:|
| 1 | 2556.0 (2468.8–2641.2) | 1.00× |
| 2 | 1388.7 (1371.8–1471.5) | 1.84× |
| 3 | 1067.5 (1029.8–1231.5) | 2.39× |
| 4 | 935.2 (890.0–963.2) | 2.73× |
| 5 | 883.3 (813.2–918.7) | 2.89× |
| 6 | 848.6 (809.7–913.2) | 3.01× |

