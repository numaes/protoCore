# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    23.23 |    30.08 |  1.29x |    20.82 |  0.90x |    N/A   |   N/A  |
| int_sum_loop           |   500.73 |   516.35 |  1.03x |   507.68 |  1.01x |   309.67 |  0.62x |
| list_append_loop       |    23.85 |    32.06 |  1.34x |   147.42 |  6.18x |   163.23 |  6.84x |
| str_concat_loop        |    22.56 |    32.38 |  1.44x |   134.38 |  5.96x |   211.07 |  9.36x |
| range_iterate          |   306.02 |   407.85 |  1.33x |   545.07 |  1.78x |   326.10 |  1.07x |
| multithread_cpu        |   171.97 |   604.57 |  3.52x |   113.93 |  0.66x |    64.85 |  0.38x |
| attr_lookup            |   353.22 |   547.03 |  1.55x |  2189.05 |  6.20x |  1134.93 |  3.21x |
| call_recursion         |    35.83 |    44.22 |  1.23x |   186.34 |  5.20x |   166.27 |  4.64x |
| memory_pressure [INFO] |    50.68 |    59.39 |  1.17x |  1474.45 | 29.09x |  1801.36 | 35.54x |
| pyperf_fib             |    86.48 |    99.43 |  1.15x |   413.74 |  4.78x |    N/A   |   N/A  |
| pyperf_binary_trees    |    33.35 |    44.29 |  1.33x |   492.01 | 14.75x |    N/A   |   N/A  |
| pyperf_nqueens         |    50.05 |    56.17 |  1.12x |   122.11 |  2.44x |    N/A   |   N/A  |
| pyperf_richards_lite   |    24.50 |    35.06 |  1.43x |    42.06 |  1.72x |    N/A   |   N/A  |
| pyperf_sieve           |    24.99 |    34.31 |  1.37x |    94.18 |  3.77x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.40x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **2.97x**
* protopyc / CPython-t: **2.18x**
