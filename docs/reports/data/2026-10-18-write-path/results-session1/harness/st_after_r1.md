# protoST benchmarks — 2026-10-19

- **Host:** AMD Ryzen 5 5500U with Radeon Graphics, 12 logical CPUs, Linux 7.0.0-38-generic
- **Load average (1 min):** 2.77 before, 4.20 after
- **protoST:** protoST 0.6.6 at commit `764b84e`
- **CPython:** Python 3.14.0
- **Method:** 2 warmup + 5 timed runs per measurement; median wall-clock, min–max in parentheses. Every run verified its result (`VERIFIED` / `BENCH_RESULT`); a failed run fails the harness.

## Start-up

A process that evaluates one expression: protoST 25.1 ms (23.7–28.1), CPython 29.0 ms (27.6–29.4). The comparable workloads below use small N, so start-up is part of every time.

## Comparable workloads

Same algorithm, same N and same verified result as the protoPython twin. `Ratio` is protoST ÷ CPython on whole-process time (>1 means protoST is slower). At these N the process start-up is a large share of every time, so `Work ratio` also compares the times with each runtime's measured start-up subtracted: the cost of the work itself.

| Benchmark | N | protoST ms | CPython ms | Ratio | Work ratio |
|---|---:|---:|---:|---:|---:|
| int_sum_loop | 100000 | 45.5 (44.1–50.4) | 33.7 (32.8–35.1) | 1.35× | 4.3× |
| fib | 25 | 383.1 (356.1–401.1) | 43.2 (41.3–50.8) | 8.88× | 25.3× |
| list_append | 10000 | 50.1 (44.4–53.8) | 30.2 (29.2–31.0) | 1.66× | 20.1× |
| str_concat | 2000 | 27.3 (27.1–30.7) | 29.5 (28.6–30.2) | 0.93× | 4.2× |
| attr_lookup | 100000 | 169.5 (165.3–176.5) | 41.1 (39.6–44.1) | 4.12× | 11.9× |
| range_iterate | 100000 | 77.5 (75.0–81.2) | 35.3 (33.9–36.5) | 2.20× | 8.3× |
| exception_latency | 50000 | 991.2 (966.7–1025.0) | 49.6 (47.1–51.8) | 19.99× | 46.9× |
| **Geomean** | | | | **3.19×** | **12.3×** |

## Actors

- **Parallel speedup** (12 CPU-bound actors): 628.2 ms with one worker, 220.3 ms with the default pool: **2.85×**.
- **Cooperative yield:** 1000 actors, each parked on a `wait`, on 2 worker threads: 483.6 ms.
- **Message throughput:** 2000 round-trip sends (send + `wait`): 71.2 ms, about 28,094 round trips per second including start-up.

Worker scaling of `saturation_big.st` (32 actors × 50 messages of CPU work):

| Workers | ms | Speedup vs 1 |
|---:|---:|---:|
| 1 | 2507.7 (2486.6–2575.8) | 1.00× |
| 2 | 1530.3 (1360.8–1701.4) | 1.64× |
| 3 | 1094.3 (1056.6–1133.9) | 2.29× |
| 4 | 936.5 (894.8–1063.5) | 2.68× |
| 5 | 844.9 (830.0–908.4) | 2.97× |
| 6 | 865.8 (805.4–1078.6) | 2.90× |

