# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    21.27 |    29.05 |  1.37x |    19.87 |  0.93x |    N/A   |   N/A  |
| int_sum_loop           |   435.38 |   491.51 |  1.13x |   464.52 |  1.07x |   280.12 |  0.64x |
| list_append_loop       |    23.23 |    30.20 |  1.30x |   134.92 |  5.81x |   153.93 |  6.63x |
| str_concat_loop        |    21.78 |    29.12 |  1.34x |   120.05 |  5.51x |   182.50 |  8.38x |
| range_iterate          |   274.03 |   366.66 |  1.34x |   491.94 |  1.80x |   300.25 |  1.10x |
| multithread_cpu        |   169.14 |   586.88 |  3.47x |   102.39 |  0.61x |    44.48 |  0.26x |
| attr_lookup            |   318.65 |   494.21 |  1.55x |  1988.92 |  6.24x |  1001.08 |  3.14x |
| call_recursion         |    31.01 |    39.84 |  1.28x |   171.63 |  5.53x |   143.90 |  4.64x |
| memory_pressure [INFO] |    46.36 |    55.67 |  1.20x |  1460.30 | 31.50x |  1696.61 | 36.60x |
| pyperf_fib             |    80.94 |    95.23 |  1.18x |   368.91 |  4.56x |    N/A   |   N/A  |
| pyperf_binary_trees    |    31.28 |    41.75 |  1.33x |   475.66 | 15.21x |    N/A   |   N/A  |
| pyperf_nqueens         |    46.34 |    51.41 |  1.11x |   108.74 |  2.35x |    N/A   |   N/A  |
| pyperf_richards_lite   |    23.06 |    31.46 |  1.36x |    40.21 |  1.74x |    N/A   |   N/A  |
| pyperf_sieve           |    24.29 |    31.58 |  1.30x |    87.41 |  3.60x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.40x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **2.94x**
* protopyc / CPython-t: **2.05x**
