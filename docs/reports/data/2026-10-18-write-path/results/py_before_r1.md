# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    21.49 |    29.22 |  1.36x |    20.74 |  0.97x |    N/A   |   N/A  |
| int_sum_loop           |   429.82 |   454.76 |  1.06x |   444.80 |  1.03x |   272.08 |  0.63x |
| list_append_loop       |    22.92 |    30.52 |  1.33x |   135.12 |  5.90x |   152.08 |  6.64x |
| str_concat_loop        |    21.89 |    30.36 |  1.39x |   117.46 |  5.37x |   184.42 |  8.43x |
| range_iterate          |   273.27 |   345.03 |  1.26x |   480.76 |  1.76x |   293.54 |  1.07x |
| multithread_cpu        |   154.77 |   559.99 |  3.62x |   111.53 |  0.72x |    51.48 |  0.33x |
| attr_lookup            |   312.00 |   492.82 |  1.58x |  1972.60 |  6.32x |  1047.27 |  3.36x |
| call_recursion         |    30.94 |    40.53 |  1.31x |   175.88 |  5.68x |   144.29 |  4.66x |
| memory_pressure [INFO] |    48.23 |    56.18 |  1.16x |  1471.84 | 30.52x |  1725.58 | 35.78x |
| pyperf_fib             |    85.68 |   100.71 |  1.18x |   408.87 |  4.77x |    N/A   |   N/A  |
| pyperf_binary_trees    |    33.16 |    41.62 |  1.26x |   466.91 | 14.08x |    N/A   |   N/A  |
| pyperf_nqueens         |    46.93 |    53.47 |  1.14x |   105.14 |  2.24x |    N/A   |   N/A  |
| pyperf_richards_lite   |    24.05 |    31.94 |  1.33x |    39.75 |  1.65x |    N/A   |   N/A  |
| pyperf_sieve           |    25.09 |    32.74 |  1.30x |    89.35 |  3.56x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.39x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **2.95x**
* protopyc / CPython-t: **2.13x**
