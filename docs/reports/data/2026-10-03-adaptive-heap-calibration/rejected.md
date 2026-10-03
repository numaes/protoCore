| round | law | `core/ahb-100k` | `core/ahb-1000k` | `st/fib` | `py/sieve` | `jsq/records-par` | `jsq/join-seq` |
|---|---|---|---|---|---|---|---|
| 1 | today | 646 MB, 2.62 s | 672 MB, 4.12 s | 301 MB, 0.50 s | 951 MB, 1.33 s | 5533 MB, 6.74 s | 1093 MB, 7.80 s |
| 1 | 2.10.0 law | 205 MB, 1.75 s | 1226 MB, 3.35 s | 552 MB, 2.18 s | 250 MB, 1.22 s | 2318 MB, 7.63 s | 425 MB, 12.71 s |
| 1 | k_live 2, k_cap 8 lifted after 3 high cycles, S0 32 MiB | 176 MB, 2.02 s | 1179 MB, 3.46 s | 588 MB, 2.31 s | 188 MB, 1.53 s | 2153 MB, 9.14 s | 345 MB, 8.44 s |
| 1 | same + early wake | 178 MB, 2.29 s | 1039 MB, 4.11 s | 589 MB, 2.35 s | 144 MB, 1.54 s | 1931 MB, 9.04 s | 339 MB, 6.85 s |
| 2 | today | 647 MB, 2.49 s | 669 MB, 4.05 s | 301 MB, 0.53 s | 951 MB, 1.58 s | 5433 MB, 7.41 s | 1093 MB, 4.79 s |
| 2 | rate-aware (Tm, m 2), k_live 2, S0 32 MiB | 232 MB, 1.57 s | 994 MB, 4.49 s | 552 MB, 2.04 s | 214 MB, 1.83 s | 476 MB, 21.13 s | 255 MB, 15.86 s |
| 2 | same + early wake | 232 MB, 1.66 s | 835 MB, 5.91 s | 553 MB, 2.05 s | 199 MB, 1.92 s | 525 MB, 18.25 s | 249 MB, 14.29 s |
| 2 | rate-aware (Tm, m 1) + early wake | 101 MB, 3.80 s | 256 MB, 13.38 s | 553 MB, 2.07 s | 179 MB, 5.89 s | 427 MB, 25.51 s | 254 MB, 19.48 s |
| 3 | today | 647 MB, 2.45 s | 670 MB, 4.29 s | 285 MB, 0.72 s | 951 MB, 1.42 s | 5450 MB, 6.09 s | 1108 MB, 5.51 s |
| 3 | pure cap 8, k_live 2, S0 32 MiB, pacing 1/2 | 127 MB, 2.35 s | 612 MB, 5.06 s | 589 MB, 2.61 s | 159 MB, 1.90 s | 1254 MB, 8.91 s | 368 MB, 5.90 s |
| 3 | pure cap 8, k_live 2, S0 128 MiB, pacing 1/2 | 148 MB, 2.03 s | 578 MB, 5.13 s | 589 MB, 2.07 s | 141 MB, 2.23 s | 1408 MB, 8.70 s | 359 MB, 5.23 s |
| 3 | pure cap 6, k_live 3, S0 128 MiB, pacing 1/2 | 147 MB, 2.17 s | 437 MB, 6.94 s | 588 MB, 1.51 s | 199 MB, 2.16 s | 997 MB, 11.92 s | 292 MB, 5.84 s |
| 3 | pure cap 8, k_live 4, S0 128 MiB, pacing 1/2 | 145 MB, 2.05 s | 578 MB, 5.07 s | 329 MB, 0.95 s | 199 MB, 2.28 s | 1321 MB, 9.06 s | 429 MB, 4.98 s |
| 4 | today | - | 671 MB, 3.88 s | 301 MB, 0.53 s | 951 MB, 1.50 s | 5449 MB, 5.53 s | - |
| 4 | cap 8, k_live 4, S0 128 MiB, pacing 1/4 | - | 576 MB, 4.52 s | 329 MB, 0.86 s | 239 MB, 1.15 s | 1483 MB, 9.10 s | - |
| 4 | same, pacing 1/10 | - | 539 MB, 4.90 s | 321 MB, 0.93 s | 326 MB, 1.29 s | 1531 MB, 11.46 s | - |
| 4 | same as P5 + early wake | - | 570 MB, 5.87 s | 335 MB, 0.89 s | 191 MB, 1.58 s | 1252 MB, 9.99 s | - |
| 5 | today | 647 MB, 2.23 s | 670 MB, 3.91 s | 301 MB, 0.56 s | 951 MB, 1.44 s | 5445 MB, 5.92 s | 1093 MB, 4.40 s |
| 5 | cap 8, k_live 4, S0 128 MiB, pacing 1/4 | 151 MB, 2.42 s | 560 MB, 4.87 s | 321 MB, 0.93 s | 231 MB, 1.29 s | 1479 MB, 9.25 s | 469 MB, 4.65 s |
| 5 | cap 12, k_live 4 | 152 MB, 2.26 s | 878 MB, 3.98 s | 322 MB, 0.90 s | 311 MB, 1.21 s | 2306 MB, 9.09 s | 469 MB, 4.42 s |
| 5 | cap 8, k_live 4, S0 256 MiB | 265 MB, 1.68 s | 523 MB, 4.90 s | 534 MB, 1.33 s | 266 MB, 1.04 s | 1465 MB, 8.84 s | 408 MB, 4.23 s |
| 6 | today | 647 MB, 2.58 s | 671 MB, 4.15 s | 301 MB, 0.46 s | 951 MB, 1.36 s | 5445 MB, 5.90 s | 1093 MB, 4.35 s |
| 6 | cap 8, k_live 3, p_high 0.2, g 1.25 | 152 MB, 2.46 s | 524 MB, 5.16 s | 321 MB, 0.80 s | 213 MB, 1.35 s | 1402 MB, 10.14 s | 338 MB, 4.69 s |
| 6 | **cap 8, k_live 3, S0 128 MiB, pacing 1/4 (chosen)** | 152 MB, 2.58 s | 576 MB, 5.00 s | 329 MB, 0.78 s | 239 MB, 1.20 s | 1448 MB, 9.02 s | 335 MB, 4.54 s |
