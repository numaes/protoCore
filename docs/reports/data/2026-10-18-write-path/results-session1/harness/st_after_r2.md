# protoST benchmarks — 2026-10-19

- **Host:** AMD Ryzen 5 5500U with Radeon Graphics, 12 logical CPUs, Linux 7.0.0-38-generic
- **Load average (1 min):** 3.64 before, 3.45 after
- **protoST:** protoST 0.6.6 at commit `764b84e`
- **CPython:** Python 3.14.0
- **Method:** 2 warmup + 5 timed runs per measurement; median wall-clock, min–max in parentheses. Every run verified its result (`VERIFIED` / `BENCH_RESULT`); a failed run fails the harness.

## Start-up

A process that evaluates one expression: protoST 24.5 ms (23.4–26.7), CPython 59.4 ms (29.3–60.6). The comparable workloads below use small N, so start-up is part of every time.

## Comparable workloads

Same algorithm, same N and same verified result as the protoPython twin. `Ratio` is protoST ÷ CPython on whole-process time (>1 means protoST is slower). At these N the process start-up is a large share of every time, so `Work ratio` also compares the times with each runtime's measured start-up subtracted: the cost of the work itself.

| Benchmark | N | protoST ms | CPython ms | Ratio | Work ratio |
|---|---:|---:|---:|---:|---:|
| int_sum_loop | 100000 | 47.7 (47.1–50.7) | 34.1 (32.0–35.7) | 1.40× | — |
| fib | 25 | 375.5 (361.9–402.3) | 53.6 (40.5–68.6) | 7.01× | — |
| list_append | 10000 | 49.8 (42.4–51.1) | 31.0 (28.8–31.8) | 1.61× | — |
| str_concat | 2000 | 28.8 (26.9–30.3) | 29.7 (28.8–29.9) | 0.97× | — |
| attr_lookup | 100000 | 168.4 (160.5–172.4) | 41.0 (40.8–44.4) | 4.11× | — |
| range_iterate | 100000 | 74.3 (73.3–78.5) | 35.5 (34.1–36.0) | 2.09× | — |
| exception_latency | 50000 | 984.5 (967.3–996.3) | 47.0 (46.4–50.4) | 20.94× | — |
| **Geomean** | | | | **3.10×** | **0.0×** |

## Actors

- **Parallel speedup** (12 CPU-bound actors): 610.0 ms with one worker, 291.5 ms with the default pool: **2.09×**.
- **Cooperative yield:** 1000 actors, each parked on a `wait`, on 2 worker threads: 467.1 ms.
- **Message throughput:** 2000 round-trip sends (send + `wait`): 72.9 ms, about 27,446 round trips per second including start-up.

Worker scaling of `saturation_big.st` (32 actors × 50 messages of CPU work):

| Workers | ms | Speedup vs 1 |
|---:|---:|---:|
| 1 | 2629.5 (2417.1–2649.5) | 1.00× |
| 2 | 1469.8 (1453.5–1525.6) | 1.79× |
| 3 | 1041.4 (1006.0–1093.4) | 2.52× |
| 4 | 941.5 (907.5–968.6) | 2.79× |
| 5 | 833.8 (777.8–958.4) | 3.15× |
| 6 | 797.9 (778.0–898.1) | 3.30× |

