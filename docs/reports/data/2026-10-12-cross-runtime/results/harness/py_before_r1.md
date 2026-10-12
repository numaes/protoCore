# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    21.59 |    31.07 |  1.44x |    20.40 |  0.94x |    N/A   |   N/A  |
| int_sum_loop           |   464.80 |   478.97 |  1.03x |   478.98 |  1.03x |   295.93 |  0.64x |
| list_append_loop       |    23.90 |    30.15 |  1.26x |   143.76 |  6.01x |   166.04 |  6.95x |
| str_concat_loop        |    22.87 |    31.35 |  1.37x |   127.74 |  5.59x |   193.10 |  8.44x |
| range_iterate          |   289.60 |   376.68 |  1.30x |   521.12 |  1.80x |   318.23 |  1.10x |
| multithread_cpu        |   170.66 |   598.27 |  3.51x |   112.32 |  0.66x |    46.14 |  0.27x |
| attr_lookup            |   339.04 |   525.14 |  1.55x |  2176.95 |  6.42x |  1066.85 |  3.15x |
| call_recursion         |    32.69 |    41.59 |  1.27x |   183.28 |  5.61x |   154.04 |  4.71x |
| memory_pressure [INFO] |    50.62 |    56.56 |  1.12x |  1465.78 | 28.95x |  1746.16 | 34.49x |
| pyperf_fib             |    85.61 |   100.94 |  1.18x |   403.85 |  4.72x |    N/A   |   N/A  |
| pyperf_binary_trees    |    32.92 |    42.88 |  1.30x |   505.44 | 15.35x |    N/A   |   N/A  |
| pyperf_nqueens         |    47.21 |    55.43 |  1.17x |   112.88 |  2.39x |    N/A   |   N/A  |
| pyperf_richards_lite   |    24.20 |    33.09 |  1.37x |    41.01 |  1.69x |    N/A   |   N/A  |
| pyperf_sieve           |    25.06 |    34.24 |  1.37x |    92.65 |  3.70x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.40x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **2.99x**
* protopyc / CPython-t: **2.07x**
