# protoST benchmarks — 2026-10-19

- **Host:** AMD Ryzen 5 5500U with Radeon Graphics, 12 logical CPUs, Linux 7.0.0-38-generic
- **Load average (1 min):** 3.11 before, 5.48 after
- **protoST:** protoST 0.6.6 at commit `764b84e`
- **CPython:** Python 3.14.0
- **Method:** 2 warmup + 5 timed runs per measurement; median wall-clock, min–max in parentheses. Every run verified its result (`VERIFIED` / `BENCH_RESULT`); a failed run fails the harness.

## Start-up

A process that evaluates one expression: protoST 32.6 ms (26.1–45.9), CPython 32.1 ms (27.4–43.9). The comparable workloads below use small N, so start-up is part of every time.

## Comparable workloads

Same algorithm, same N and same verified result as the protoPython twin. `Ratio` is protoST ÷ CPython on whole-process time (>1 means protoST is slower). At these N the process start-up is a large share of every time, so `Work ratio` also compares the times with each runtime's measured start-up subtracted: the cost of the work itself.

| Benchmark | N | protoST ms | CPython ms | Ratio | Work ratio |
|---|---:|---:|---:|---:|---:|
| int_sum_loop | 100000 | 47.7 (44.4–59.3) | 32.2 (31.6–33.2) | 1.48× | — |
| fib | 25 | 357.8 (355.4–362.8) | 39.9 (38.0–41.8) | 8.96× | 41.7× |
| list_append | 10000 | 48.6 (42.6–51.2) | 29.0 (28.5–30.8) | 1.67× | — |
| str_concat | 2000 | 27.5 (26.9–27.9) | 28.9 (28.0–30.2) | 0.95× | — |
| attr_lookup | 100000 | 159.4 (154.7–163.2) | 39.7 (38.0–41.0) | 4.02× | 16.7× |
| range_iterate | 100000 | 72.8 (71.7–75.1) | 32.4 (31.9–33.7) | 2.25× | — |
| exception_latency | 50000 | 958.5 (927.7–983.2) | 46.4 (45.0–60.6) | 20.65× | 64.7× |
| **Geomean** | | | | **3.26×** | **35.6×** |

## Actors

- **Parallel speedup** (12 CPU-bound actors): 578.5 ms with one worker, 288.7 ms with the default pool: **2.00×**.
- **Cooperative yield:** 1000 actors, each parked on a `wait`, on 2 worker threads: 452.3 ms.
- **Message throughput:** 2000 round-trip sends (send + `wait`): 67.4 ms, about 29,687 round trips per second including start-up.

Worker scaling of `saturation_big.st` (32 actors × 50 messages of CPU work):

| Workers | ms | Speedup vs 1 |
|---:|---:|---:|
| 1 | 2363.4 (2352.1–2449.2) | 1.00× |
| 2 | 1307.9 (1274.0–1337.3) | 1.81× |
| 3 | 1015.0 (1005.5–1041.1) | 2.33× |
| 4 | 872.0 (846.4–1133.1) | 2.71× |
| 5 | 853.3 (797.7–894.1) | 2.77× |
| 6 | 834.2 (773.0–873.2) | 2.83× |

