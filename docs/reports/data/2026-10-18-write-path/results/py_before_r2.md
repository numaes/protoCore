# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    21.83 |    29.96 |  1.37x |    20.21 |  0.93x |    N/A   |   N/A  |
| int_sum_loop           |   464.11 |   471.87 |  1.02x |   483.16 |  1.04x |   296.34 |  0.64x |
| list_append_loop       |    23.15 |    30.96 |  1.34x |   137.26 |  5.93x |   157.07 |  6.79x |
| str_concat_loop        |    21.81 |    30.06 |  1.38x |   124.24 |  5.70x |   200.24 |  9.18x |
| range_iterate          |   281.83 |   359.56 |  1.28x |   503.32 |  1.79x |   315.09 |  1.12x |
| multithread_cpu        |   164.59 |   593.10 |  3.60x |   105.58 |  0.64x |    44.82 |  0.27x |
| attr_lookup            |   331.86 |   508.25 |  1.53x |  2057.47 |  6.20x |  1056.07 |  3.18x |
| call_recursion         |    32.00 |    40.69 |  1.27x |   176.69 |  5.52x |   148.27 |  4.63x |
| memory_pressure [INFO] |    48.69 |    57.45 |  1.18x |  1418.37 | 29.13x |  1773.30 | 36.42x |
| pyperf_fib             |    81.97 |    96.35 |  1.18x |   382.61 |  4.67x |    N/A   |   N/A  |
| pyperf_binary_trees    |    32.84 |    42.64 |  1.30x |   475.61 | 14.48x |    N/A   |   N/A  |
| pyperf_nqueens         |    49.10 |    54.24 |  1.10x |   107.37 |  2.19x |    N/A   |   N/A  |
| pyperf_richards_lite   |    24.15 |    32.43 |  1.34x |    41.20 |  1.71x |    N/A   |   N/A  |
| pyperf_sieve           |    24.94 |    33.23 |  1.33x |    88.45 |  3.55x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.39x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **2.92x**
* protopyc / CPython-t: **2.10x**
