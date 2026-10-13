# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    21.36 |    28.75 |  1.35x |    19.67 |  0.92x |    N/A   |   N/A  |
| int_sum_loop           |   425.70 |   455.15 |  1.07x |   443.17 |  1.04x |   265.03 |  0.62x |
| list_append_loop       |    22.04 |    30.40 |  1.38x |   128.88 |  5.85x |   148.15 |  6.72x |
| str_concat_loop        |    21.92 |    29.28 |  1.34x |   116.82 |  5.33x |   178.25 |  8.13x |
| range_iterate          |   266.14 |   351.69 |  1.32x |   487.94 |  1.83x |   289.78 |  1.09x |
| multithread_cpu        |   145.76 |   580.76 |  3.98x |   113.92 |  0.78x |    42.98 |  0.29x |
| attr_lookup            |   309.43 |   483.29 |  1.56x |  1970.02 |  6.37x |   996.71 |  3.22x |
| call_recursion         |    30.75 |    40.31 |  1.31x |   169.32 |  5.51x |   145.24 |  4.72x |
| memory_pressure [INFO] |    46.52 |    53.57 |  1.15x |  1474.07 | 31.69x |  1720.47 | 36.98x |
| pyperf_fib             |    79.95 |    95.79 |  1.20x |   363.64 |  4.55x |    N/A   |   N/A  |
| pyperf_binary_trees    |    32.07 |    41.05 |  1.28x |   482.50 | 15.05x |    N/A   |   N/A  |
| pyperf_nqueens         |    47.09 |    50.44 |  1.07x |   108.39 |  2.30x |    N/A   |   N/A  |
| pyperf_richards_lite   |    23.69 |    31.19 |  1.32x |    40.12 |  1.69x |    N/A   |   N/A  |
| pyperf_sieve           |    24.58 |    31.92 |  1.30x |    87.59 |  3.56x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.40x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **2.97x**
* protopyc / CPython-t: **2.08x**
