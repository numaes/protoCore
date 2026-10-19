# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    22.21 |    30.96 |  1.39x |    20.10 |  0.90x |    N/A   |   N/A  |
| int_sum_loop           |   461.28 |   479.39 |  1.04x |   481.91 |  1.04x |   294.87 |  0.64x |
| list_append_loop       |    23.83 |    31.58 |  1.33x |   138.66 |  5.82x |   157.13 |  6.59x |
| str_concat_loop        |    24.33 |    31.98 |  1.31x |   125.82 |  5.17x |   196.40 |  8.07x |
| range_iterate          |   287.83 |   363.96 |  1.26x |   513.24 |  1.78x |   311.11 |  1.08x |
| multithread_cpu        |   157.91 |   579.30 |  3.67x |   127.99 |  0.81x |    45.98 |  0.29x |
| attr_lookup            |   328.51 |   517.54 |  1.58x |  2062.97 |  6.28x |  1099.93 |  3.35x |
| call_recursion         |    31.72 |    40.28 |  1.27x |   182.74 |  5.76x |   146.44 |  4.62x |
| memory_pressure [INFO] |    48.33 |    57.14 |  1.18x |  1444.23 | 29.88x |  1530.79 | 31.67x |
| pyperf_fib             |    83.51 |    96.87 |  1.16x |   380.76 |  4.56x |    N/A   |   N/A  |
| pyperf_binary_trees    |    32.12 |    42.50 |  1.32x |   457.93 | 14.26x |    N/A   |   N/A  |
| pyperf_nqueens         |    47.76 |    54.36 |  1.14x |   111.24 |  2.33x |    N/A   |   N/A  |
| pyperf_richards_lite   |    24.02 |    32.09 |  1.34x |    41.52 |  1.73x |    N/A   |   N/A  |
| pyperf_sieve           |    25.25 |    32.82 |  1.30x |    87.94 |  3.48x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.39x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **2.96x**
* protopyc / CPython-t: **2.07x**
