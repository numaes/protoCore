# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    23.25 |    32.66 |  1.40x |    21.67 |  0.93x |    N/A   |   N/A  |
| int_sum_loop           |   474.42 |   476.22 |  1.00x |   483.28 |  1.02x |   291.77 |  0.61x |
| list_append_loop       |    23.84 |    31.71 |  1.33x |   138.80 |  5.82x |   155.36 |  6.52x |
| str_concat_loop        |    23.16 |    30.83 |  1.33x |   131.50 |  5.68x |   194.22 |  8.39x |
| range_iterate          |   291.71 |   368.12 |  1.26x |   535.16 |  1.83x |   313.00 |  1.07x |
| multithread_cpu        |   170.61 |   622.81 |  3.65x |   144.55 |  0.85x |    41.14 |  0.24x |
| attr_lookup            |   347.76 |   536.04 |  1.54x |  2151.47 |  6.19x |  1091.09 |  3.14x |
| call_recursion         |    31.60 |    40.74 |  1.29x |   185.64 |  5.87x |   156.18 |  4.94x |
| memory_pressure [INFO] |    49.22 |    56.89 |  1.16x |  1556.87 | 31.63x |  1816.32 | 36.90x |
| pyperf_fib             |    90.60 |   102.10 |  1.13x |   393.38 |  4.34x |    N/A   |   N/A  |
| pyperf_binary_trees    |    33.28 |    43.80 |  1.32x |   512.39 | 15.40x |    N/A   |   N/A  |
| pyperf_nqueens         |    48.68 |    55.72 |  1.14x |   116.56 |  2.39x |    N/A   |   N/A  |
| pyperf_richards_lite   |    24.62 |    33.55 |  1.36x |    41.11 |  1.67x |    N/A   |   N/A  |
| pyperf_sieve           |    26.04 |    33.96 |  1.30x |    92.57 |  3.55x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.38x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **3.01x**
* protopyc / CPython-t: **2.01x**
