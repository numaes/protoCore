# protoST benchmarks — 2026-10-19

- **Host:** AMD Ryzen 5 5500U with Radeon Graphics, 12 logical CPUs, Linux 7.0.0-38-generic
- **Load average (1 min):** 3.68 before, 4.49 after
- **protoST:** protoST 0.6.6 at commit `764b84e`
- **CPython:** Python 3.14.0
- **Method:** 2 warmup + 5 timed runs per measurement; median wall-clock, min–max in parentheses. Every run verified its result (`VERIFIED` / `BENCH_RESULT`); a failed run fails the harness.

## Start-up

A process that evaluates one expression: protoST 23.9 ms (23.3–25.2), CPython 29.7 ms (28.4–30.7). The comparable workloads below use small N, so start-up is part of every time.

## Comparable workloads

Same algorithm, same N and same verified result as the protoPython twin. `Ratio` is protoST ÷ CPython on whole-process time (>1 means protoST is slower). At these N the process start-up is a large share of every time, so `Work ratio` also compares the times with each runtime's measured start-up subtracted: the cost of the work itself.

| Benchmark | N | protoST ms | CPython ms | Ratio | Work ratio |
|---|---:|---:|---:|---:|---:|
| int_sum_loop | 100000 | 46.6 (45.0–49.5) | 33.2 (31.6–34.1) | 1.41× | 6.6× |
| fib | 25 | 354.1 (348.4–363.6) | 41.2 (40.1–43.2) | 8.60× | 28.8× |
| list_append | 10000 | 49.3 (47.2–50.6) | 29.9 (29.4–30.2) | 1.65× | — |
| str_concat | 2000 | 28.1 (27.8–29.6) | 28.6 (27.9–29.0) | 0.98× | — |
| attr_lookup | 100000 | 165.6 (159.9–170.0) | 39.1 (37.8–52.0) | 4.23× | 15.1× |
| range_iterate | 100000 | 74.1 (72.8–91.2) | 34.9 (31.6–47.1) | 2.12× | 9.6× |
| exception_latency | 50000 | 953.3 (922.5–989.8) | 46.4 (45.6–46.7) | 20.55× | 55.7× |
| **Geomean** | | | | **3.22×** | **17.3×** |

## Actors

- **Parallel speedup** (12 CPU-bound actors): 598.6 ms with one worker, 219.1 ms with the default pool: **2.73×**.
- **Cooperative yield:** 1000 actors, each parked on a `wait`, on 2 worker threads: 467.0 ms.
- **Message throughput:** 2000 round-trip sends (send + `wait`): 66.4 ms, about 30,133 round trips per second including start-up.

Worker scaling of `saturation_big.st` (32 actors × 50 messages of CPU work):

| Workers | ms | Speedup vs 1 |
|---:|---:|---:|
| 1 | 2625.5 (2536.1–2648.6) | 1.00× |
| 2 | 1419.1 (1382.2–1470.8) | 1.85× |
| 3 | 1078.2 (1026.6–1088.5) | 2.44× |
| 4 | 918.8 (873.3–979.9) | 2.86× |
| 5 | 904.9 (821.7–935.8) | 2.90× |
| 6 | 833.5 (811.8–892.5) | 3.15× |

