# protoST benchmarks — 2026-10-19

- **Host:** AMD Ryzen 5 5500U with Radeon Graphics, 12 logical CPUs, Linux 7.0.0-38-generic
- **Load average (1 min):** 2.15 before, 4.08 after
- **protoST:** protoST 0.6.6 at commit `764b84e`
- **CPython:** Python 3.14.0
- **Method:** 2 warmup + 5 timed runs per measurement; median wall-clock, min–max in parentheses. Every run verified its result (`VERIFIED` / `BENCH_RESULT`); a failed run fails the harness.

## Start-up

A process that evaluates one expression: protoST 23.9 ms (23.4–24.7), CPython 28.9 ms (28.6–29.2). The comparable workloads below use small N, so start-up is part of every time.

## Comparable workloads

Same algorithm, same N and same verified result as the protoPython twin. `Ratio` is protoST ÷ CPython on whole-process time (>1 means protoST is slower). At these N the process start-up is a large share of every time, so `Work ratio` also compares the times with each runtime's measured start-up subtracted: the cost of the work itself.

| Benchmark | N | protoST ms | CPython ms | Ratio | Work ratio |
|---|---:|---:|---:|---:|---:|
| int_sum_loop | 100000 | 47.2 (42.2–54.9) | 32.2 (31.9–33.4) | 1.47× | 7.1× |
| fib | 25 | 368.1 (352.3–376.7) | 40.0 (39.0–40.4) | 9.20× | 31.1× |
| list_append | 10000 | 48.2 (47.7–50.6) | 29.9 (29.6–30.4) | 1.61× | 25.4× |
| str_concat | 2000 | 28.8 (27.3–29.3) | 28.8 (28.4–29.1) | 1.00× | — |
| attr_lookup | 100000 | 160.4 (157.0–167.9) | 38.3 (37.3–39.8) | 4.19× | 14.5× |
| range_iterate | 100000 | 72.6 (70.8–74.4) | 31.8 (31.6–34.5) | 2.28× | 17.0× |
| exception_latency | 50000 | 963.2 (910.0–1075.8) | 45.1 (44.7–47.7) | 21.34× | 57.9× |
| **Geomean** | | | | **3.32×** | **20.8×** |

## Actors

- **Parallel speedup** (12 CPU-bound actors): 606.1 ms with one worker, 298.7 ms with the default pool: **2.03×**.
- **Cooperative yield:** 1000 actors, each parked on a `wait`, on 2 worker threads: 477.1 ms.
- **Message throughput:** 2000 round-trip sends (send + `wait`): 67.5 ms, about 29,635 round trips per second including start-up.

Worker scaling of `saturation_big.st` (32 actors × 50 messages of CPU work):

| Workers | ms | Speedup vs 1 |
|---:|---:|---:|
| 1 | 2506.6 (2501.0–2600.0) | 1.00× |
| 2 | 1430.7 (1355.1–1476.4) | 1.75× |
| 3 | 1057.9 (1031.3–1127.6) | 2.37× |
| 4 | 909.1 (879.2–932.1) | 2.76× |
| 5 | 846.4 (809.6–874.6) | 2.96× |
| 6 | 863.5 (837.4–922.8) | 2.90× |

