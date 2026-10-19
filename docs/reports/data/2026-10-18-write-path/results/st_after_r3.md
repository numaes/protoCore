# protoST benchmarks — 2026-10-19

- **Host:** AMD Ryzen 5 5500U with Radeon Graphics, 12 logical CPUs, Linux 7.0.0-38-generic
- **Load average (1 min):** 4.06 before, 6.61 after
- **protoST:** protoST 0.6.6 at commit `764b84e`
- **CPython:** Python 3.14.0
- **Method:** 2 warmup + 5 timed runs per measurement; median wall-clock, min–max in parentheses. Every run verified its result (`VERIFIED` / `BENCH_RESULT`); a failed run fails the harness.

## Start-up

A process that evaluates one expression: protoST 23.8 ms (23.6–24.4), CPython 28.7 ms (28.1–29.3). The comparable workloads below use small N, so start-up is part of every time.

## Comparable workloads

Same algorithm, same N and same verified result as the protoPython twin. `Ratio` is protoST ÷ CPython on whole-process time (>1 means protoST is slower). At these N the process start-up is a large share of every time, so `Work ratio` also compares the times with each runtime's measured start-up subtracted: the cost of the work itself.

| Benchmark | N | protoST ms | CPython ms | Ratio | Work ratio |
|---|---:|---:|---:|---:|---:|
| int_sum_loop | 100000 | 45.8 (42.8–46.6) | 33.4 (32.2–33.6) | 1.37× | 4.7× |
| fib | 25 | 363.1 (348.5–367.0) | 40.8 (39.5–42.1) | 8.89× | 28.0× |
| list_append | 10000 | 48.8 (48.0–50.8) | 29.4 (28.8–30.3) | 1.66× | 36.2× |
| str_concat | 2000 | 27.1 (26.1–40.6) | 28.6 (28.4–29.3) | 0.95× | — |
| attr_lookup | 100000 | 157.3 (154.2–160.9) | 37.8 (37.6–39.3) | 4.16× | 14.6× |
| range_iterate | 100000 | 72.3 (71.0–75.6) | 32.3 (31.9–33.5) | 2.24× | 13.5× |
| exception_latency | 50000 | 979.4 (949.7–1150.1) | 45.4 (44.7–46.6) | 21.56× | 57.2× |
| **Geomean** | | | | **3.25×** | **19.5×** |

## Actors

- **Parallel speedup** (12 CPU-bound actors): 578.7 ms with one worker, 282.2 ms with the default pool: **2.05×**.
- **Cooperative yield:** 1000 actors, each parked on a `wait`, on 2 worker threads: 461.7 ms.
- **Message throughput:** 2000 round-trip sends (send + `wait`): 65.0 ms, about 30,763 round trips per second including start-up.

Worker scaling of `saturation_big.st` (32 actors × 50 messages of CPU work):

| Workers | ms | Speedup vs 1 |
|---:|---:|---:|
| 1 | 2406.6 (2352.5–2454.8) | 1.00× |
| 2 | 1361.4 (1296.3–1378.0) | 1.77× |
| 3 | 994.2 (968.1–1038.7) | 2.42× |
| 4 | 852.5 (841.0–879.8) | 2.82× |
| 5 | 809.1 (785.8–938.7) | 2.97× |
| 6 | 829.2 (801.0–1071.1) | 2.90× |

