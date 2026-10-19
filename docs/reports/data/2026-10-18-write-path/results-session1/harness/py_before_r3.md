# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    22.67 |    30.59 |  1.35x |    20.57 |  0.91x |    N/A   |   N/A  |
| int_sum_loop           |   479.78 |   510.89 |  1.06x |   493.08 |  1.03x |   299.85 |  0.62x |
| list_append_loop       |    23.02 |    32.73 |  1.42x |   141.48 |  6.15x |   161.43 |  7.01x |
| str_concat_loop        |    22.70 |    31.52 |  1.39x |   126.02 |  5.55x |   206.50 |  9.10x |
| range_iterate          |   299.94 |   381.77 |  1.27x |   530.17 |  1.77x |   319.71 |  1.07x |
| multithread_cpu        |   177.11 |   594.07 |  3.35x |   114.30 |  0.65x |    47.34 |  0.27x |
| attr_lookup            |   346.64 |   527.56 |  1.52x |  2107.61 |  6.08x |  1101.80 |  3.18x |
| call_recursion         |    32.41 |    42.92 |  1.32x |   188.77 |  5.82x |   152.76 |  4.71x |
| memory_pressure [INFO] |    50.11 |    57.14 |  1.14x |  1464.65 | 29.23x |  1756.03 | 35.04x |
| pyperf_fib             |    86.03 |   101.04 |  1.17x |   387.41 |  4.50x |    N/A   |   N/A  |
| pyperf_binary_trees    |    33.37 |    43.00 |  1.29x |   489.80 | 14.68x |    N/A   |   N/A  |
| pyperf_nqueens         |    48.15 |    55.01 |  1.14x |   110.66 |  2.30x |    N/A   |   N/A  |
| pyperf_richards_lite   |    24.90 |    32.24 |  1.29x |    40.32 |  1.62x |    N/A   |   N/A  |
| pyperf_sieve           |    25.61 |    33.60 |  1.31x |    90.04 |  3.52x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.39x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **2.92x**
* protopyc / CPython-t: **2.08x**
