# protoST benchmarks — 2026-10-19

- **Host:** AMD Ryzen 5 5500U with Radeon Graphics, 12 logical CPUs, Linux 7.0.0-38-generic
- **Load average (1 min):** 2.83 before, 4.83 after
- **protoST:** protoST 0.6.6 at commit `764b84e`
- **CPython:** Python 3.14.0
- **Method:** 2 warmup + 5 timed runs per measurement; median wall-clock, min–max in parentheses. Every run verified its result (`VERIFIED` / `BENCH_RESULT`); a failed run fails the harness.

## Start-up

A process that evaluates one expression: protoST 27.8 ms (24.9–33.7), CPython 30.3 ms (29.1–33.7). The comparable workloads below use small N, so start-up is part of every time.

## Comparable workloads

Same algorithm, same N and same verified result as the protoPython twin. `Ratio` is protoST ÷ CPython on whole-process time (>1 means protoST is slower). At these N the process start-up is a large share of every time, so `Work ratio` also compares the times with each runtime's measured start-up subtracted: the cost of the work itself.

| Benchmark | N | protoST ms | CPython ms | Ratio | Work ratio |
|---|---:|---:|---:|---:|---:|
| int_sum_loop | 100000 | 49.5 (48.7–51.0) | 34.8 (33.9–35.7) | 1.42× | 4.8× |
| fib | 25 | 374.5 (352.3–414.3) | 41.9 (40.7–55.4) | 8.94× | 29.8× |
| list_append | 10000 | 48.3 (40.7–49.0) | 30.9 (28.0–32.3) | 1.56× | 31.1× |
| str_concat | 2000 | 28.1 (26.7–29.8) | 30.1 (28.7–30.5) | 0.93× | — |
| attr_lookup | 100000 | 177.0 (161.7–186.3) | 40.7 (37.5–43.5) | 4.35× | 14.4× |
| range_iterate | 100000 | 78.3 (72.8–78.5) | 34.3 (32.7–34.8) | 2.28× | 12.5× |
| exception_latency | 50000 | 999.0 (936.5–1025.2) | 47.8 (45.5–49.5) | 20.89× | 55.3× |
| **Geomean** | | | | **3.25×** | **18.8×** |

## Actors

- **Parallel speedup** (12 CPU-bound actors): 593.8 ms with one worker, 214.8 ms with the default pool: **2.77×**.
- **Cooperative yield:** 1000 actors, each parked on a `wait`, on 2 worker threads: 480.5 ms.
- **Message throughput:** 2000 round-trip sends (send + `wait`): 67.4 ms, about 29,691 round trips per second including start-up.

Worker scaling of `saturation_big.st` (32 actors × 50 messages of CPU work):

| Workers | ms | Speedup vs 1 |
|---:|---:|---:|
| 1 | 2539.6 (2428.1–2573.4) | 1.00× |
| 2 | 1456.6 (1354.9–1485.7) | 1.74× |
| 3 | 1095.3 (1048.6–1128.4) | 2.32× |
| 4 | 918.6 (901.7–995.1) | 2.76× |
| 5 | 876.9 (802.8–907.3) | 2.90× |
| 6 | 892.0 (822.8–904.9) | 2.85× |

