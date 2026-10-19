# protoST benchmarks — 2026-10-19

- **Host:** AMD Ryzen 5 5500U with Radeon Graphics, 12 logical CPUs, Linux 7.0.0-38-generic
- **Load average (1 min):** 2.49 before, 4.32 after
- **protoST:** protoST 0.6.6 at commit `764b84e`
- **CPython:** Python 3.14.0
- **Method:** 2 warmup + 5 timed runs per measurement; median wall-clock, min–max in parentheses. Every run verified its result (`VERIFIED` / `BENCH_RESULT`); a failed run fails the harness.

## Start-up

A process that evaluates one expression: protoST 24.5 ms (23.5–26.4), CPython 29.2 ms (28.8–30.7). The comparable workloads below use small N, so start-up is part of every time.

## Comparable workloads

Same algorithm, same N and same verified result as the protoPython twin. `Ratio` is protoST ÷ CPython on whole-process time (>1 means protoST is slower). At these N the process start-up is a large share of every time, so `Work ratio` also compares the times with each runtime's measured start-up subtracted: the cost of the work itself.

| Benchmark | N | protoST ms | CPython ms | Ratio | Work ratio |
|---|---:|---:|---:|---:|---:|
| int_sum_loop | 100000 | 45.7 (45.5–60.3) | 32.4 (32.1–33.9) | 1.41× | 6.6× |
| fib | 25 | 358.9 (342.5–361.5) | 40.3 (40.0–41.1) | 8.91× | 30.2× |
| list_append | 10000 | 48.2 (46.8–49.9) | 29.5 (28.8–29.8) | 1.63× | — |
| str_concat | 2000 | 28.8 (26.8–41.5) | 29.4 (28.6–30.1) | 0.98× | — |
| attr_lookup | 100000 | 158.6 (153.1–177.2) | 37.8 (37.4–40.0) | 4.19× | 15.6× |
| range_iterate | 100000 | 72.0 (70.7–73.8) | 33.9 (32.5–34.7) | 2.12× | 10.1× |
| exception_latency | 50000 | 942.9 (931.2–960.6) | 46.2 (44.9–47.3) | 20.40× | 54.0× |
| **Geomean** | | | | **3.23×** | **17.6×** |

## Actors

- **Parallel speedup** (12 CPU-bound actors): 584.8 ms with one worker, 281.9 ms with the default pool: **2.07×**.
- **Cooperative yield:** 1000 actors, each parked on a `wait`, on 2 worker threads: 439.7 ms.
- **Message throughput:** 2000 round-trip sends (send + `wait`): 64.2 ms, about 31,129 round trips per second including start-up.

Worker scaling of `saturation_big.st` (32 actors × 50 messages of CPU work):

| Workers | ms | Speedup vs 1 |
|---:|---:|---:|
| 1 | 2379.6 (2318.6–2450.3) | 1.00× |
| 2 | 1323.7 (1276.3–1477.2) | 1.80× |
| 3 | 1029.5 (1011.9–1061.3) | 2.31× |
| 4 | 900.1 (861.1–918.1) | 2.64× |
| 5 | 896.4 (807.9–936.1) | 2.65× |
| 6 | 845.6 (810.9–915.4) | 2.81× |

