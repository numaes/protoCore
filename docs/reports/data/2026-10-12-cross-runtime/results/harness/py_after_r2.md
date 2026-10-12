# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    22.91 |    32.61 |  1.42x |    21.38 |  0.93x |    N/A   |   N/A  |
| int_sum_loop           |   502.99 |   514.35 |  1.02x |   517.71 |  1.03x |   306.90 |  0.61x |
| list_append_loop       |    24.00 |    32.59 |  1.36x |   154.89 |  6.45x |   170.11 |  7.09x |
| str_concat_loop        |    23.36 |    34.61 |  1.48x |   133.73 |  5.73x |   216.17 |  9.26x |
| range_iterate          |   340.78 |   428.23 |  1.26x |   609.44 |  1.79x |   363.34 |  1.07x |
| multithread_cpu        |   209.93 |   688.14 |  3.28x |   162.47 |  0.77x |    55.91 |  0.27x |
| attr_lookup            |   368.18 |   572.72 |  1.56x |  2292.36 |  6.23x |  1144.04 |  3.11x |
| call_recursion         |    35.08 |    46.32 |  1.32x |   202.71 |  5.78x |   169.94 |  4.84x |
| memory_pressure [INFO] |    53.88 |    63.01 |  1.17x |  1719.49 | 31.92x |  1854.98 | 34.43x |
| pyperf_fib             |    91.71 |   107.32 |  1.17x |   436.36 |  4.76x |    N/A   |   N/A  |
| pyperf_binary_trees    |    34.53 |    42.54 |  1.23x |   542.15 | 15.70x |    N/A   |   N/A  |
| pyperf_nqueens         |    53.06 |    60.66 |  1.14x |   119.09 |  2.24x |    N/A   |   N/A  |
| pyperf_richards_lite   |    24.57 |    34.21 |  1.39x |    46.13 |  1.88x |    N/A   |   N/A  |
| pyperf_sieve           |    27.44 |    34.98 |  1.27x |    99.70 |  3.63x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.39x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **3.06x**
* protopyc / CPython-t: **2.08x**
