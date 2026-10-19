# protoST benchmarks — 2026-10-19

- **Host:** AMD Ryzen 5 5500U with Radeon Graphics, 12 logical CPUs, Linux 7.0.0-38-generic
- **Load average (1 min):** 2.13 before, 5.11 after
- **protoST:** protoST 0.6.6 at commit `764b84e`
- **CPython:** Python 3.14.0
- **Method:** 2 warmup + 5 timed runs per measurement; median wall-clock, min–max in parentheses. Every run verified its result (`VERIFIED` / `BENCH_RESULT`); a failed run fails the harness.

## Start-up

A process that evaluates one expression: protoST 24.2 ms (23.5–27.9), CPython 30.2 ms (30.0–34.6). The comparable workloads below use small N, so start-up is part of every time.

## Comparable workloads

Same algorithm, same N and same verified result as the protoPython twin. `Ratio` is protoST ÷ CPython on whole-process time (>1 means protoST is slower). At these N the process start-up is a large share of every time, so `Work ratio` also compares the times with each runtime's measured start-up subtracted: the cost of the work itself.

| Benchmark | N | protoST ms | CPython ms | Ratio | Work ratio |
|---|---:|---:|---:|---:|---:|
| int_sum_loop | 100000 | 52.6 (47.7–70.8) | 33.7 (32.4–36.0) | 1.56× | 8.3× |
| fib | 25 | 372.5 (365.6–391.8) | 41.0 (40.0–46.2) | 9.08× | 32.3× |
| list_append | 10000 | 51.5 (42.6–53.0) | 30.3 (30.0–44.7) | 1.70× | — |
| str_concat | 2000 | 28.0 (27.2–30.8) | 31.1 (29.8–33.0) | 0.90× | 4.5× |
| attr_lookup | 100000 | 171.4 (169.7–176.0) | 41.3 (39.1–42.6) | 4.15× | 13.3× |
| range_iterate | 100000 | 79.1 (73.3–82.3) | 33.3 (31.7–36.4) | 2.37× | 17.7× |
| exception_latency | 50000 | 1014.8 (1006.8–1041.7) | 46.8 (45.6–49.6) | 21.66× | 59.6× |
| **Geomean** | | | | **3.34×** | **16.0×** |

## Actors

- **Parallel speedup** (12 CPU-bound actors): 639.0 ms with one worker, 229.9 ms with the default pool: **2.78×**.
- **Cooperative yield:** 1000 actors, each parked on a `wait`, on 2 worker threads: 523.4 ms.
- **Message throughput:** 2000 round-trip sends (send + `wait`): 71.6 ms, about 27,947 round trips per second including start-up.

Worker scaling of `saturation_big.st` (32 actors × 50 messages of CPU work):

| Workers | ms | Speedup vs 1 |
|---:|---:|---:|
| 1 | 2673.6 (2583.3–2728.0) | 1.00× |
| 2 | 1462.2 (1426.6–1557.5) | 1.83× |
| 3 | 1149.2 (1075.2–1300.5) | 2.33× |
| 4 | 925.1 (892.0–985.7) | 2.89× |
| 5 | 908.3 (870.5–999.7) | 2.94× |
| 6 | 884.2 (844.0–933.7) | 3.02× |

