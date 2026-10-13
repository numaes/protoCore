# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    21.32 |    28.87 |  1.35x |    19.67 |  0.92x |    N/A   |   N/A  |
| int_sum_loop           |   433.26 |   476.55 |  1.10x |   461.55 |  1.07x |   276.17 |  0.64x |
| list_append_loop       |    22.87 |    30.05 |  1.31x |   132.34 |  5.79x |   153.95 |  6.73x |
| str_concat_loop        |    21.67 |    29.20 |  1.35x |   119.20 |  5.50x |   183.98 |  8.49x |
| range_iterate          |   273.29 |   371.26 |  1.36x |   506.70 |  1.85x |   297.34 |  1.09x |
| multithread_cpu        |   151.08 |   576.40 |  3.82x |   113.58 |  0.75x |    50.97 |  0.34x |
| attr_lookup            |   319.07 |   491.79 |  1.54x |  2016.88 |  6.32x |  1014.89 |  3.18x |
| call_recursion         |    30.60 |    40.19 |  1.31x |   175.80 |  5.74x |   144.44 |  4.72x |
| memory_pressure [INFO] |    46.35 |    55.71 |  1.20x |  1458.00 | 31.45x |  1675.09 | 36.14x |
| pyperf_fib             |    78.76 |    94.25 |  1.20x |   381.53 |  4.84x |    N/A   |   N/A  |
| pyperf_binary_trees    |    32.29 |    40.74 |  1.26x |   535.28 | 16.58x |    N/A   |   N/A  |
| pyperf_nqueens         |    45.69 |    51.35 |  1.12x |   113.38 |  2.48x |    N/A   |   N/A  |
| pyperf_richards_lite   |    23.00 |    31.43 |  1.37x |    40.64 |  1.77x |    N/A   |   N/A  |
| pyperf_sieve           |    24.45 |    31.88 |  1.30x |    88.35 |  3.61x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.41x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **3.05x**
* protopyc / CPython-t: **2.13x**
