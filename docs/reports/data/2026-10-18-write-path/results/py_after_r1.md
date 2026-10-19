# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    21.75 |    30.17 |  1.39x |    19.93 |  0.92x |    N/A   |   N/A  |
| int_sum_loop           |   463.62 |   487.99 |  1.05x |   470.51 |  1.01x |   294.67 |  0.64x |
| list_append_loop       |    23.32 |    31.26 |  1.34x |   136.28 |  5.84x |   155.53 |  6.67x |
| str_concat_loop        |    22.01 |    30.70 |  1.39x |   122.59 |  5.57x |   193.43 |  8.79x |
| range_iterate          |   290.26 |   357.32 |  1.23x |   506.34 |  1.74x |   309.21 |  1.07x |
| multithread_cpu        |   173.62 |   573.64 |  3.30x |   103.42 |  0.60x |    44.92 |  0.26x |
| attr_lookup            |   329.22 |   503.40 |  1.53x |  2041.98 |  6.20x |  1062.57 |  3.23x |
| call_recursion         |    32.48 |    41.44 |  1.28x |   185.38 |  5.71x |   147.47 |  4.54x |
| memory_pressure [INFO] |    48.24 |    55.95 |  1.16x |  1470.01 | 30.47x |  1507.21 | 31.24x |
| pyperf_fib             |    80.83 |    96.11 |  1.19x |   374.86 |  4.64x |    N/A   |   N/A  |
| pyperf_binary_trees    |    32.58 |    42.76 |  1.31x |   471.56 | 14.47x |    N/A   |   N/A  |
| pyperf_nqueens         |    46.58 |    53.89 |  1.16x |   112.31 |  2.41x |    N/A   |   N/A  |
| pyperf_richards_lite   |    24.33 |    31.58 |  1.30x |    40.88 |  1.68x |    N/A   |   N/A  |
| pyperf_sieve           |    25.11 |    33.50 |  1.33x |    86.75 |  3.45x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.38x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **2.90x**
* protopyc / CPython-t: **2.05x**
