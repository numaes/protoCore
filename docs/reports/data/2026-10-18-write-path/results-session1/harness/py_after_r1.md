# protoPython 4-way interleaved benchmark (sprint-9)

Each bench runs the four binaries interleaved (warmup x2 each, then N=5 interleaved rounds), so any system load shift hits every column equally — directly comparable wall-clocks.

Columns:

* **CPython** — uv-installed cpython-3.14.6-linux (Clang 22.1.3), **GIL ON**

* **CPython-t** — uv-installed cpython-3.14.6+freethreaded (Clang 22.1.3), **GIL OFF**

* **protopy** — bytecode interpreter (no GIL by construction)

* **protopyc** — AOT-compiled C++ via `protopyc --build-so` (no GIL)


| Benchmark              | CPython-t (ms, base) | CPython (ms) | cp/cpt | protopy (ms) | py/cpt | protopyc (ms) | pc/cpt |
|------------------------|---------------------:|-------------:|-------:|-------------:|-------:|--------------:|-------:|
| startup_empty          |    23.52 |    31.80 |  1.35x |    21.50 |  0.91x |    N/A   |   N/A  |
| int_sum_loop           |   495.69 |   504.71 |  1.02x |   517.02 |  1.04x |   301.39 |  0.61x |
| list_append_loop       |    24.82 |    33.14 |  1.34x |   149.64 |  6.03x |   173.52 |  6.99x |
| str_concat_loop        |    23.99 |    33.38 |  1.39x |   133.70 |  5.57x |   218.31 |  9.10x |
| range_iterate          |   304.88 |   393.49 |  1.29x |   574.12 |  1.88x |   326.14 |  1.07x |
| multithread_cpu        |   189.24 |   615.21 |  3.25x |   129.63 |  0.69x |    54.85 |  0.29x |
| attr_lookup            |   367.74 |   569.66 |  1.55x |  2473.63 |  6.73x |  1147.01 |  3.12x |
| call_recursion         |    34.17 |    44.37 |  1.30x |   199.50 |  5.84x |   160.89 |  4.71x |
| memory_pressure [INFO] |    51.87 |    59.32 |  1.14x |  1558.01 | 30.03x |  1628.64 | 31.40x |
| pyperf_fib             |    93.51 |   114.06 |  1.22x |   457.81 |  4.90x |    N/A   |   N/A  |
| pyperf_binary_trees    |    35.10 |    44.47 |  1.27x |   495.86 | 14.13x |    N/A   |   N/A  |
| pyperf_nqueens         |    50.33 |    54.57 |  1.08x |   122.75 |  2.44x |    N/A   |   N/A  |
| pyperf_richards_lite   |    24.87 |    33.21 |  1.34x |    45.75 |  1.84x |    N/A   |   N/A  |
| pyperf_sieve           |    26.89 |    34.91 |  1.30x |    91.79 |  3.41x |    N/A   |   N/A  |

## Geomeans (memory_pressure excluded) — baseline is CPython-t (no GIL)
* CPython (GIL on) / CPython-t: **1.37x** — <1.0 means GIL build is faster single-thread (the lock-cost saving)
* protopy / CPython-t: **3.02x**
* protopyc / CPython-t: **2.09x**
