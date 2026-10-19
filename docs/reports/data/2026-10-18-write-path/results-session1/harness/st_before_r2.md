# protoST benchmarks — 2026-10-19

- **Host:** AMD Ryzen 5 5500U with Radeon Graphics, 12 logical CPUs, Linux 7.0.0-38-generic
- **Load average (1 min):** 3.37 before, 4.90 after
- **protoST:** protoST 0.6.6 at commit `764b84e`
- **CPython:** Python 3.14.0
- **Method:** 2 warmup + 5 timed runs per measurement; median wall-clock, min–max in parentheses. Every run verified its result (`VERIFIED` / `BENCH_RESULT`); a failed run fails the harness.

## Start-up

A process that evaluates one expression: protoST 25.3 ms (24.4–25.8), CPython 29.3 ms (28.4–30.3). The comparable workloads below use small N, so start-up is part of every time.

## Comparable workloads

Same algorithm, same N and same verified result as the protoPython twin. `Ratio` is protoST ÷ CPython on whole-process time (>1 means protoST is slower). At these N the process start-up is a large share of every time, so `Work ratio` also compares the times with each runtime's measured start-up subtracted: the cost of the work itself.

| Benchmark | N | protoST ms | CPython ms | Ratio | Work ratio |
|---|---:|---:|---:|---:|---:|
| int_sum_loop | 100000 | 47.5 (42.6–49.4) | 34.2 (32.9–37.6) | 1.39× | 4.5× |
| fib | 25 | 375.0 (358.1–392.3) | 42.2 (40.7–46.6) | 8.88× | 26.9× |
| list_append | 10000 | 43.4 (42.4–52.8) | 30.8 (30.4–31.6) | 1.41× | 12.1× |
| str_concat | 2000 | 27.5 (26.3–31.1) | 31.2 (30.3–34.5) | 0.88× | 1.1× |
| attr_lookup | 100000 | 176.2 (171.2–184.8) | 40.5 (39.3–43.2) | 4.35× | 13.5× |
| range_iterate | 100000 | 76.9 (75.2–80.1) | 33.9 (33.1–36.7) | 2.27× | 11.2× |
| exception_latency | 50000 | 1032.0 (1017.2–1118.5) | 48.0 (46.8–50.9) | 21.48× | 53.6× |
| **Geomean** | | | | **3.18×** | **10.4×** |

## Actors

- **Parallel speedup** (12 CPU-bound actors): 648.1 ms with one worker, 219.2 ms with the default pool: **2.96×**.
- **Cooperative yield:** 1000 actors, each parked on a `wait`, on 2 worker threads: 517.9 ms.
- **Message throughput:** 2000 round-trip sends (send + `wait`): 72.0 ms, about 27,789 round trips per second including start-up.

Worker scaling of `saturation_big.st` (32 actors × 50 messages of CPU work):

| Workers | ms | Speedup vs 1 |
|---:|---:|---:|
| 1 | 2746.9 (2667.4–2801.7) | 1.00× |
| 2 | 1485.6 (1479.0–1589.2) | 1.85× |
| 3 | 1127.4 (1108.5–1190.5) | 2.44× |
| 4 | 975.4 (954.4–1007.2) | 2.82× |
| 5 | 996.3 (917.7–1070.9) | 2.76× |
| 6 | 917.1 (868.1–968.4) | 3.00× |

