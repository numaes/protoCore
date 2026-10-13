# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    21.14 |    28.71 |  1.36x |    19.65 |  0.93x |    N/A   |   N/A  |
| int_sum_loop           |   462.05 |   473.77 |  1.03x |   459.75 |  1.00x |   278.91 |  0.60x |
| list_append_loop       |    23.00 |    30.17 |  1.31x |   132.59 |  5.76x |   151.20 |  6.57x |
| str_concat_loop        |    21.88 |    28.84 |  1.32x |   121.49 |  5.55x |   186.67 |  8.53x |
| range_iterate          |   283.73 |   363.35 |  1.28x |   505.89 |  1.78x |   292.87 |  1.03x |
| multithread_cpu        |   156.46 |   579.71 |  3.71x |    98.47 |  0.63x |    42.06 |  0.27x |
| attr_lookup            |   327.07 |   509.05 |  1.56x |  2010.36 |  6.15x |  1040.54 |  3.18x |
| call_recursion         |    30.70 |    39.71 |  1.29x |   172.52 |  5.62x |   148.75 |  4.84x |
| memory_pressure [INFO] |    46.96 |    55.50 |  1.18x |  1441.49 | 30.69x |  1622.64 | 34.55x |
| pyperf_fib             |    78.99 |    94.56 |  1.20x |   373.84 |  4.73x |    N/A   |   N/A  |
| pyperf_binary_trees    |    31.20 |    40.92 |  1.31x |   460.21 | 14.75x |    N/A   |   N/A  |
| pyperf_nqueens         |    45.19 |    50.33 |  1.11x |   105.75 |  2.34x |    N/A   |   N/A  |
| pyperf_richards_lite   |    24.05 |    31.08 |  1.29x |    40.37 |  1.68x |    N/A   |   N/A  |
| pyperf_sieve           |    24.38 |    32.72 |  1.34x |    85.71 |  3.52x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.39x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **2.92x**
* protopyc / CPython-t: **2.04x**
