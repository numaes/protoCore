# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    21.62 |    30.33 |  1.40x |    20.54 |  0.95x |    N/A   |   N/A  |
| int_sum_loop           |   460.07 |   474.27 |  1.03x |   496.31 |  1.08x |   297.35 |  0.65x |
| list_append_loop       |    23.20 |    29.78 |  1.28x |   140.00 |  6.03x |   159.27 |  6.86x |
| str_concat_loop        |    22.43 |    31.54 |  1.41x |   126.54 |  5.64x |   187.58 |  8.36x |
| range_iterate          |   292.55 |   380.73 |  1.30x |   519.22 |  1.77x |   308.55 |  1.05x |
| multithread_cpu        |   161.95 |   572.97 |  3.54x |   113.25 |  0.70x |    48.87 |  0.30x |
| attr_lookup            |   340.51 |   520.04 |  1.53x |  2188.75 |  6.43x |  1031.33 |  3.03x |
| call_recursion         |    31.96 |    43.04 |  1.35x |   185.54 |  5.80x |   155.91 |  4.88x |
| memory_pressure [INFO] |    50.28 |    56.73 |  1.13x |  1456.55 | 28.97x |  1798.08 | 35.76x |
| pyperf_fib             |    83.07 |    98.79 |  1.19x |   387.50 |  4.66x |    N/A   |   N/A  |
| pyperf_binary_trees    |    34.13 |    43.28 |  1.27x |   489.71 | 14.35x |    N/A   |   N/A  |
| pyperf_nqueens         |    49.03 |    53.46 |  1.09x |   112.23 |  2.29x |    N/A   |   N/A  |
| pyperf_richards_lite   |    24.12 |    33.08 |  1.37x |    41.83 |  1.73x |    N/A   |   N/A  |
| pyperf_sieve           |    26.29 |    36.53 |  1.39x |    99.46 |  3.78x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.40x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **3.00x**
* protopyc / CPython-t: **2.09x**
