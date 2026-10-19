# protoST benchmarks — 2026-10-19

- **Host:** AMD Ryzen 5 5500U with Radeon Graphics, 12 logical CPUs, Linux 7.0.0-38-generic
- **Load average (1 min):** 2.20 before, 3.91 after
- **protoST:** protoST 0.6.6 at commit `764b84e`
- **CPython:** Python 3.14.0
- **Method:** 2 warmup + 5 timed runs per measurement; median wall-clock, min–max in parentheses. Every run verified its result (`VERIFIED` / `BENCH_RESULT`); a failed run fails the harness.

## Start-up

A process that evaluates one expression: protoST 23.9 ms (23.7–24.1), CPython 31.2 ms (28.9–32.5). The comparable workloads below use small N, so start-up is part of every time.

## Comparable workloads

Same algorithm, same N and same verified result as the protoPython twin. `Ratio` is protoST ÷ CPython on whole-process time (>1 means protoST is slower). At these N the process start-up is a large share of every time, so `Work ratio` also compares the times with each runtime's measured start-up subtracted: the cost of the work itself.

| Benchmark | N | protoST ms | CPython ms | Ratio | Work ratio |
|---|---:|---:|---:|---:|---:|
| int_sum_loop | 100000 | 46.7 (43.1–49.2) | 34.7 (32.8–38.0) | 1.35× | 6.5× |
| fib | 25 | 366.7 (354.7–388.2) | 41.6 (40.5–41.8) | 8.82× | 33.0× |
| list_append | 10000 | 53.2 (43.6–63.9) | 29.6 (28.6–33.0) | 1.80× | — |
| str_concat | 2000 | 28.8 (27.0–32.4) | 29.1 (28.3–30.8) | 0.99× | — |
| attr_lookup | 100000 | 174.4 (165.1–177.2) | 38.5 (38.2–42.6) | 4.53× | 20.6× |
| range_iterate | 100000 | 74.3 (72.9–77.6) | 34.1 (33.5–35.9) | 2.18× | 17.5× |
| exception_latency | 50000 | 994.1 (974.1–1068.5) | 48.3 (46.9–49.3) | 20.56× | 56.6× |
| **Geomean** | | | | **3.30×** | **21.3×** |

## Actors

- **Parallel speedup** (12 CPU-bound actors): 602.3 ms with one worker, 228.1 ms with the default pool: **2.64×**.
- **Cooperative yield:** 1000 actors, each parked on a `wait`, on 2 worker threads: 495.6 ms.
- **Message throughput:** 2000 round-trip sends (send + `wait`): 69.7 ms, about 28,709 round trips per second including start-up.

Worker scaling of `saturation_big.st` (32 actors × 50 messages of CPU work):

| Workers | ms | Speedup vs 1 |
|---:|---:|---:|
| 1 | 2696.9 (2612.4–2805.3) | 1.00× |
| 2 | 1511.9 (1431.6–1548.1) | 1.78× |
| 3 | 1138.5 (1078.1–1190.1) | 2.37× |
| 4 | 933.6 (883.6–973.5) | 2.89× |
| 5 | 867.1 (811.0–916.3) | 3.11× |
| 6 | 876.1 (861.3–911.4) | 3.08× |

