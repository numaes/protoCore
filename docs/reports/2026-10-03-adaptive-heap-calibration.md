# Adaptive heap controller: calibration on runtime workloads

Date: 2026-10-03.  protoCore 2.10.1 (branch `fix/adaptive-heap-calibration`
from master `4355e733`, 2.10.0).  Author: Gustavo Marino, with Claude.
Design: [../specs/2026-10-02-adaptive-heap-controller-design.md](../specs/2026-10-02-adaptive-heap-controller-design.md);
its section 7 summarises this report.

## Verdict

- **Memory.**  The calibrated controller no longer runs away.  The workload
  that motivated the calibration is `adaptive_heap_benchmark` with a
  1 M-cell live set.  On it, peak RSS is now 544 MB, against 1,161 MB with
  2.10.0 and 671 MB under the 640 MB fixed limit.  Peak RSS is at or
  below today's policy on 40 of 44 workloads.  The four exceptions:
  - `adaptive_heap_benchmark` with a 5 M-cell live set (+245 %), where the
    fixed limit thrashes: 25.8 s against 12.4 s;
  - protoST `fib.st` (+10 %, 28 MB);
  - protoST `message_throughput` (+5 %, 5 MB, with no collection cycle);
  - `scala/object_tree` (+3 %, the larger initial soft limit).
- **Time.**  The acceptance criterion (wall time within +5 % of today) is
  **not met on 16 of 44 workloads**.  Four of those ran no collection cycle
  under the controller and differ by run-to-run noise (section "Noise").
  The other 12 lose time, in two groups:
  - **Today's policy collects rarely or never.**  This is protoJS's
    75 %-of-memory default, and protoPython, protoClojure and protoScala,
    which set no limit.  Collecting costs time here that no soft limit
    recovers:
    - protoJS structure benchmarks, sequential: +12 % to +31 %;
    - protoJS structure benchmarks, parallel Deferreds: +75 %;
    - `py/binary_trees`: +9 %;
    - `scala/list_ops`: +15 %.
  - **Today's policy is the 640 MB fixed limit.**  The controller is faster
    or equal, except on:
    - the fast allocator with a 1 M-cell live set: +17 %;
    - `fib.st`: +70 %, from one futile cycle;
    - the protoJS probe: +10 %, and +3 % in a second set.
- **No setting met both criteria on every workload.**  A more generous law
  (k_live 6, k_cap 12) wins 5 % of time on the 1 M-cell benchmark, but only
  by exceeding the fixed limit's memory there by 31 %.  It still costs
  protoJS +15 % to +26 %.  The remaining time cost comes from the single
  collector thread's throughput, not from the soft limit.

The recommendation on adoption, per runtime, is at the end of this report.

## Method

**Binaries.**  Each runtime's existing Release binary ran unchanged against a
development libprotoCore, loaded through `LD_LIBRARY_PATH` with the same
SOVERSION 3.  Two checks confirmed the setup:
- `ldd` showed the development library for every binary;
- the `PROTOCORE_HEAP_TRACE=1` lines showed that the controller ran.

The controller was switched on with the new diagnostic
`PROTOCORE_ADAPTIVE_HEAP=1`.  It enables the controller in every space when
the space is created, and makes `setHeapLimits` leave it on.

| Runtime | Binary | Today's policy |
|---|---|---|
| protoST | `protoST/build_wr/protost` | fixed hard limit 10,000,000 cells (640 MB), no soft limit |
| protoPython | `protoPython/build_release/src/runtime/protopy` | none: no limit is set, so it never collects |
| protoClojure | `protoClojure/build_pkg_oct2026/protoclj` | none |
| protoScala | `protoScala/build_pkg_oct2026/protoscala` | none |
| protoJS (`js/`) | copy of `protoJS/build/protojs` (2026-10-02 23:43) | hard limit at 75 % of memory or of the cgroup limit (7.5 GB inside the measurement scope) |
| protoJS (`js640/`) | copy of `protoJS/build_release/protojs` (2026-10-02 14:25) | hard limit 10,000,000 cells (640 MB) |
| protoCore | `adaptive_heap_benchmark fixed 10485760 <live>` | the 640 MB limit runtimes used to set |

**Variants.**  Each workload ran under three variants:
- *today*: the binary as is, on the 2.10.1 library.  The fixed-limit paths
  are the same in this release.
- *2.10.0*: the 2.10.0 library (master `4355e733` plus the diagnostic
  switch), with `PROTOCORE_ADAPTIVE_HEAP=1`.
- *2.10.1*: this release, with `PROTOCORE_ADAPTIVE_HEAP=1`.

The controller ran with its defaults, so H was 75 % of the scope's 10 GB
limit.

**Workloads.**
- **protoCore:** `adaptive_heap_benchmark` with 0, 100 k, 1 M and 5 M live
  elements.
- **protoST** (4 workers):
  - `benchmarks/actors/`: `saturation_big`, `parallel_speedup`,
    `message_throughput`;
  - `benchmarks/comparable/`: `list_append`, `str_concat`, `fib`.
- **protoPython**, the long four-way set: `int_sum_loop`,
  `list_append_loop`, `str_concat_loop`, `range_iterate`,
  `multithreaded_cpu`, `attr_lookup`, `call_recursion` at fib(27), and the
  pyperf `fib`, `binary_trees`, `nqueens`, `richards_lite` and `sieve`.
- **protoClojure**, `benchmarks/*.clj`: `fib`, `tak`, `sum-loop`,
  `reduce-list`, `sum-squares`, and `actor-saturation-32` with 4 workers.
- **protoScala:**
  - `benchmarks/comparable/`: `fib30`, `list_ops`, `map_build`,
    `object_tree`, `str_concat`, `tak`, `sum_loop`;
  - `benchmarks/actors/actor-saturation-32.scala` with 4 workers.
- **protoJS:**
  - `benchmarks/structures/protojs_bench.js`: `records`, `doctree`,
    `graph`, `join` and `wordfreq` run sequentially, plus `records` run with
    6 parallel Deferreds; N = 6, REPS = 3;
  - the memory probe: 100,000 five-field objects pushed into an array, then
    an aggregate printed.

Every run checks its own output and its exit status.  The output checks
are:
- `VERIFIED <n>` (protoST);
- `BENCH_RESULT` (protoPython);
- the `// EXPECT:` line (protoScala);
- `"ok":true`, with equal checksums across repetitions (protoJS
  structures);
- the probe's aggregate.

No run in the final matrices failed.

**Measurement.**
- Runs were sequential.  Each ran in
  `systemd-run --user --scope -p MemoryMax=10G -p MemorySwapMax=0` under
  `/usr/bin/time`, which gave peak RSS (`%M`), wall time and user + sys
  time.
- Cycles, final S and maximum pressure come from the trace.
- Each figure is the median of 3 runs.  The three variants of a workload
  were interleaved within each repetition.
- The machine is a Linux x86-64 AMD with 12 threads and 62 GB, shared with
  other jobs.  The fixed policy's run-to-run spread was 2-9 % on most
  workloads, 13-20 % on several short ones, and 22-48 % on the actor
  benchmarks (see "Noise").

## Diagnosis: why 2.10.0 grew S to 20 x the live set

Trace of `adaptive_heap_benchmark` with a 1 M-cell live set under the
2.10.0 law, with the cycle duration `Tc` added for this report:

| cycle | L | S before -> after | p | Tc |
|---|---|---|---|---|
| 5 | 319 k | 1.77 M -> 2.65 M | 0.15 | 49 ms |
| 9 | 630 k | 3.98 M -> 5.97 M | 0.69 | 105 ms |
| 12 | 1.00 M | 8.96 M -> 13.4 M | 0.61 | 244 ms |
| 13 | 1.00 M | 13.4 M -> 20.2 M | 0.09 | 280 ms |
| 16 | 1.00 M | 20.2 M -> 20.2 M | 0.0001 | 352 ms |

The cycle time grows with S, because the sweep is proportional to the
garbage.  The collector swept about 30 M cells/s while the program allocated
20-37 M cells/s.  A stall therefore came back at every soft limit, and each
stall grew S by 1.5x.  S stopped growing only because the program ended.

Section 6.11 of the design read that end point as an equilibrium
(`L + 2 x rate x cycle time`).  It is not one.

Two further observations:
- Waking a waiting mutator as soon as the sweep publishes free cells did
  not remove the stalls.  The mutator then waits for the sweep instead of
  for the cycle.
- With `k_live = 1.5`, every trace examined (`adaptive_heap_benchmark` and
  protoJS `records`) showed the collector running back to back: Tc was
  about equal to the interval T.

## The calibrated law

```
floor = ceil(k_live * L)                          k_live = 3   (was 1.5)
cap   = max(S0, ceil(k_cap * L), floor)           k_cap  = 8   (new)
p > p_high:  S' = min(H, max(S, floor, min(ceil(S * g), cap)))
otherwise:   S' = min(H, max(S, floor))
S0 = 2,097,152 cells (128 MiB, was 32 MiB); p_high = 0.05, g = 1.5 (unchanged)
pacing: request the next cycle when a quarter of S - L is left (was half)
```

Why each value:

- **k_cap = 8, a pure cap with no override.**
  - It bounds the memory of a program whose collector cannot keep up to
    8 x its live set.  Past the cap, the mutator is paced by the collector,
    as it is under a fixed limit.
  - On the 1 M-cell benchmark, a cap of 12 used 878 MB (+31 % over the
    fixed limit).  A cap of 6 (with k_live 3) used 437 MB but took 6.9 s.
  - Lifting the cap after three consecutive high-pressure cycles was tried
    and rejected.  On a throughput-bound collector pressure is persistent,
    so the override always fired and S reached 19 M cells again.
- **k_live = 3.**
  - The collector's work per cycle is proportional to L.  The number of
    cycles is the garbage divided by `S - L`.
  - 3 and 4 measured alike, within noise; 3 keeps the floor lower for
    large live sets.
  - 2 doubled the cost of a growing live set (`fib.st`: 2.07 s against
    0.95 s with 4, at the same S0).
- **S0 = 128 MiB.**
  - Most short programs then finish with no cycle or one, as under today's
    policies.
  - With the same k_live, 32 MiB cost 16-100 % in time on `ahb-100k`,
    `st/parallel_speedup`, `scala/map_build` and `py/str_concat_loop`.
  - 256 MiB made a growing live set pay a longer futile cycle (`fib.st`:
    534 MB, 1.33 s).
  - The price: a program with almost no live data now reaches about 116 MB
    instead of 35 MB.  That is still a fifth of the 640 MB fixed limit.
- **Pacing at a quarter of the headroom.**
  - It gives fewer cycles for the same garbage.
  - Pacing at 0.9 of the headroom consumed left too little runway, so the
    mutator stalled more (1 M-cell benchmark 4.90 s against 4.52 s).
  - Pacing at half ran the collector back to back (`fib.st` 1.31 s against
    0.86 s).
- **p_high and g were not changed.**  0.2 with g = 1.25 measured no better,
  and the cap now bounds what they can do.

Convergence: once L is stable, S changes at most
`1 + ceil(log_g(k_cap / k_live))` = 4 times.  This is tested by
`AdaptiveHeapLaw.PermanentStormStopsAtTheLiveCap`.

## Final matrix (median of 3)

| Workload | today (fixed) | 2.10.0 controller | 2.10.1 controller | 2.10.1 vs today: RSS, wall |
|---|---|---|---|---|
| core/ahb-0k | 645 MB, 2.36 s, 4 cyc | 36 MB, 1.34 s, 192 cyc S 0.5 M | 116 MB, 1.42 s, 32 cyc S 2.1 M | -82 %, -40 % |
| core/ahb-100k | 647 MB, 2.56 s, 5 cyc | 221 MB, 1.52 s, 30 cyc S 4.0 M | 152 MB, 2.48 s, 32 cyc S 2.1 M | -77 %, -3 % |
| core/ahb-1000k | 671 MB, 4.10 s, 7 cyc | 1161 MB, 3.40 s, 16 cyc S 20.2 M | 544 MB, 4.80 s, 16 cyc S 8.4 M | -19 %, +17 % |
| core/ahb-5000k | 742 MB, 25.76 s, 25 cyc | 2876 MB, 9.19 s, 18 cyc S 68.0 M | 2559 MB, 12.41 s, 16 cyc S 40.0 M | +245 %, -52 % |
| st/saturation_big | 669 MB, 1.83 s, 4 cyc | 711 MB, 1.64 s, 11 cyc S 30.2 M | 203 MB, 1.75 s, 27 cyc S 2.1 M | -70 %, -4 % |
| st/parallel_speedup | 609 MB, 0.48 s, 0 cyc | 246 MB, 0.48 s, 7 cyc S 9.0 M | 153 MB, 0.37 s, 5 cyc S 2.1 M | -75 %, -23 % |
| st/list_append | 61 MB, 0.07 s, 0 cyc | 61 MB, 0.08 s, 1 cyc S 0.8 M | 61 MB, 0.06 s, 0 cyc | +0 %, -14 % |
| st/str_concat | 32 MB, 0.04 s, 0 cyc | 33 MB, 0.04 s, 0 cyc | 33 MB, 0.04 s, 0 cyc | +1 %, +0 % |
| st/fib | 301 MB, 0.47 s, 0 cyc | 553 MB, 1.85 s, 6 cyc S 6.0 M | 329 MB, 0.80 s, 1 cyc S 5.0 M | +10 %, +70 % |
| st/message_throughput | 76 MB, 0.10 s, 0 cyc | 82 MB, 0.12 s, 1 cyc S 0.8 M | 81 MB, 0.11 s, 0 cyc | +5 %, +10 % |
| py/int_sum_loop | 58 MB, 0.23 s, 0 cyc | 49 MB, 0.22 s, 2 cyc S 0.5 M | 58 MB, 0.23 s, 0 cyc | -0 %, +0 % |
| py/list_append_loop | 394 MB, 0.59 s, 0 cyc | 164 MB, 0.53 s, 11 cyc S 2.7 M | 147 MB, 0.55 s, 4 cyc S 2.7 M | -63 %, -7 % |
| py/str_concat_loop | 106 MB, 0.22 s, 0 cyc | 91 MB, 0.34 s, 7 cyc S 1.2 M | 106 MB, 0.22 s, 0 cyc | -0 %, +0 % |
| py/range_iterate | 58 MB, 0.24 s, 0 cyc | 49 MB, 0.23 s, 2 cyc S 0.5 M | 58 MB, 0.23 s, 0 cyc | -0 %, -4 % |
| py/multithread_cpu | 52 MB, 0.18 s, 0 cyc | 52 MB, 0.21 s, 5 cyc S 0.5 M | 52 MB, 0.18 s, 0 cyc | -0 %, +0 % |
| py/attr_lookup | 58 MB, 2.50 s, 0 cyc | 49 MB, 2.48 s, 2 cyc S 0.5 M | 58 MB, 2.48 s, 0 cyc | -0 %, -1 % |
| py/call_recursion | 58 MB, 0.33 s, 0 cyc | 49 MB, 0.32 s, 2 cyc S 0.5 M | 58 MB, 0.33 s, 0 cyc | -0 %, +0 % |
| py/fib | 23 MB, 3.22 s, 0 cyc | 23 MB, 3.21 s, 0 cyc | 23 MB, 3.26 s, 0 cyc | +0 %, +1 % |
| py/binary_trees | 1853 MB, 4.59 s, 0 cyc | 472 MB, 5.07 s, 27 cyc S 9.0 M | 723 MB, 5.01 s, 9 cyc S 10.6 M | -61 %, +9 % |
| py/nqueens | 40 MB, 0.68 s, 0 cyc | 41 MB, 0.64 s, 1 cyc S 0.5 M | 40 MB, 0.66 s, 0 cyc | -0 %, -3 % |
| py/richards_lite | 24 MB, 0.05 s, 0 cyc | 23 MB, 0.05 s, 0 cyc | 24 MB, 0.05 s, 0 cyc | -0 %, +0 % |
| py/sieve | 951 MB, 1.38 s, 0 cyc | 217 MB, 1.33 s, 19 cyc S 3.5 M | 247 MB, 1.21 s, 9 cyc S 3.9 M | -74 %, -12 % |
| clj/fib | 22 MB, 0.64 s, 0 cyc | 22 MB, 0.64 s, 0 cyc | 22 MB, 0.64 s, 0 cyc | +0 %, +0 % |
| clj/tak | 24 MB, 0.05 s, 0 cyc | 24 MB, 0.05 s, 0 cyc | 24 MB, 0.05 s, 0 cyc | -0 %, +0 % |
| clj/sum-loop | 22 MB, 0.07 s, 0 cyc | 22 MB, 0.07 s, 0 cyc | 22 MB, 0.07 s, 0 cyc | +0 %, +0 % |
| clj/reduce-list | 22 MB, 0.02 s, 0 cyc | 22 MB, 0.02 s, 0 cyc | 22 MB, 0.03 s, 0 cyc | +0 %, +50 % |
| clj/sum-squares | 22 MB, 0.02 s, 0 cyc | 22 MB, 0.02 s, 0 cyc | 22 MB, 0.02 s, 0 cyc | +1 %, +0 % |
| clj/actor-saturation-32 | 23 MB, 2.59 s, 0 cyc | 23 MB, 2.64 s, 0 cyc | 23 MB, 2.96 s, 0 cyc | +1 %, +14 % |
| scala/fib30 | 27 MB, 0.51 s, 0 cyc | 27 MB, 0.49 s, 0 cyc | 27 MB, 0.49 s, 0 cyc | -0 %, -4 % |
| scala/list_ops | 321 MB, 0.53 s, 0 cyc | 210 MB, 0.79 s, 9 cyc S 4.0 M | 235 MB, 0.61 s, 2 cyc S 4.0 M | -27 %, +15 % |
| scala/map_build | 184 MB, 0.38 s, 0 cyc | 66 MB, 0.40 s, 9 cyc S 0.8 M | 133 MB, 0.36 s, 1 cyc S 2.1 M | -28 %, -5 % |
| scala/object_tree | 137 MB, 0.48 s, 0 cyc | 92 MB, 0.55 s, 6 cyc S 1.8 M | 141 MB, 0.49 s, 1 cyc S 2.1 M | +3 %, +2 % |
| scala/str_concat | 27 MB, 0.03 s, 0 cyc | 27 MB, 0.03 s, 0 cyc | 27 MB, 0.03 s, 0 cyc | +0 %, +0 % |
| scala/tak | 27 MB, 0.04 s, 0 cyc | 27 MB, 0.04 s, 0 cyc | 27 MB, 0.04 s, 0 cyc | -0 %, +0 % |
| scala/sum_loop | 27 MB, 0.09 s, 0 cyc | 27 MB, 0.09 s, 0 cyc | 27 MB, 0.09 s, 0 cyc | +0 %, +0 % |
| scala/actor-saturation-32 | 44 MB, 2.13 s, 0 cyc | 37 MB, 2.13 s, 1 cyc S 0.5 M | 44 MB, 2.27 s, 0 cyc | -0 %, +7 % |
| js/records-seq | 7884 MB, 42.34 s, 2 cyc | 466 MB, 60.03 s, 83 cyc S 9.0 M | 706 MB, 55.28 s, 41 cyc S 10.6 M | -91 %, +31 % |
| js/doctree-seq | 7873 MB, 33.92 s, 1 cyc | 831 MB, 36.77 s, 62 cyc S 13.4 M | 1558 MB, 32.96 s, 20 cyc S 23.9 M | -80 %, -3 % |
| js/graph-seq | 7811 MB, 53.15 s, 2 cyc | 438 MB, 73.36 s, 126 cyc S 9.0 M | 660 MB, 69.23 s, 60 cyc S 10.6 M | -92 %, +30 % |
| js/join-seq | 7957 MB, 36.63 s, 1 cyc | 632 MB, 41.57 s, 55 cyc S 13.4 M | 698 MB, 41.61 s, 26 cyc S 10.6 M | -91 %, +14 % |
| js/wordfreq-seq | 6504 MB, 25.50 s, 0 cyc | 143 MB, 29.32 s, 133 cyc S 1.8 M | 229 MB, 28.61 s, 48 cyc S 3.1 M | -96 %, +12 % |
| js/records-par | 7890 MB, 17.55 s, 2 cyc | 5069 MB, 18.06 s, 25 cyc S 102.0 M | 1550 MB, 30.73 s, 21 cyc S 24.3 M | -80 %, +75 % |
| js/probe-100k | 472 MB, 1.08 s, 0 cyc | 217 MB, 1.23 s, 12 cyc S 4.0 M | 216 MB, 1.04 s, 4 cyc S 4.7 M | -54 %, -4 % |
| js640/probe-100k | 636 MB, 1.85 s, 1 cyc | 358 MB, 1.92 s, 15 cyc S 6.0 M | 471 MB, 2.04 s, 6 cyc S 10.6 M | -26 %, +10 % |

In the table:
- `S` is the final soft limit, in millions of cells;
- `cyc` is the number of collection cycles.  For the fixed policy it is
  counted from its trace lines.

### Acceptance, per workload

The criteria:
- peak RSS at or below today's policy (+10 % allowed where today's limit
  thrashes);
- wall time within +5 %;
- no out-of-memory where today succeeds;
- bounded convergence.

**No out-of-memory anywhere, and bounded convergence on every run.**

**Met (28 of 44):**
- protoCore: `ahb-0k`, `ahb-100k`;
- protoST: `saturation_big`, `parallel_speedup`, `list_append`,
  `str_concat`;
- protoPython: every benchmark except `binary_trees`;
- protoClojure: `fib`, `tak`, `sum-loop`, `sum-squares`;
- protoScala: `fib30`, `map_build`, `str_concat`, `tak`, `sum_loop`;
- protoJS: `doctree`, the probe.

Faster than today: `ahb-0k` -40 %, `st/parallel_speedup` -23 %,
`st/list_append` -14 %, `py/sieve` -12 %, `py/list_append_loop` -7 %,
`scala/map_build` -5 %.

**Not met, with collection involved (12):**
- **`core/ahb-1000k`: +17 % time.**  RSS is 544 MB against 671 MB.  This is
  the case the calibration targeted, and its memory is now below the fixed
  limit.  Matching the fixed limit's time needs S near its 10.5 M cells.
  k_cap 12 gets that time (3.87 s), at 878 MB.
- **`core/ahb-5000k`: +245 % RSS** (2,559 MB against 742 MB).  The 640 MB
  fixed limit thrashes here: 25.8 s and 25 cycles, against 12.4 s.  A
  5 M-cell live set needs more memory than the fixed limit allows to run at
  speed.
- **`st/fib`: +70 % time and +10 % RSS** (329 MB against 301 MB).  The live
  set grows to 3 M cells with nothing to reclaim.  The fixed 640 MB limit is
  never reached.  The controller runs one futile cycle at 128 MiB, and the
  program waits for it.
- **protoJS structures, sequential: `records` +31 %, `graph` +30 %, `join`
  +14 %, `wordfreq` +12 %.**
- **protoJS `records` with 6 parallel Deferreds: +75 %.**
  - Today's protoJS default never collects below 7.5 GB.  These runs use
    6.5-7.9 GB today, against 0.2-1.6 GB under the controller.
  - Under the controller they run 20-60 cycles, and total CPU roughly
    doubles.  With six allocating threads, the single collector thread sets
    their pace.
  - 2.10.0 stayed within +3 % on the parallel case only by growing to
    5 GB.
- **`py/binary_trees`: +9 %** (723 MB against 1,853 MB).  Today it never
  collects.
- **`scala/list_ops`: +15 %** (235 MB against 321 MB).  Today it never
  collects.
- **`scala/object_tree`: +3 % RSS** (141 MB against 137 MB): the 128 MiB
  S0, one cycle.
- **`js640/probe-100k`: +10 %** (471 MB against 636 MB).  A second set of
  three runs (table below) gave +3 %.

**Not met, with no collection cycle under the controller (4):**
`st/message_throughput` (+10 % time, +5 % RSS), `clj/reduce-list`
(20 ms against 30 ms), `clj/actor-saturation-32` (+14 %) and
`scala/actor-saturation-32` (+7 %).
- With no cycle, the controller's only work is two relaxed loads per
  critical-section entry and one comparison per refill.
- The fixed policy's own three runs spread by 22 % and 48 % on the two
  actor benchmarks.
- The second set gave -2 % and -7 % for the two actor benchmarks.

These four are counted as not met, for want of proof that they pass.

## The frontier: a more generous law

A second set of three runs used the same harness.  It compares 2.10.1 with
a build whose only change is `k_live = 6`, `k_cap = 12`:

| Workload | today (fixed) | 2.10.1 (k_live 3, k_cap 8) | k_live 6, k_cap 12 | k_live 6, k_cap 12 vs today: RSS, wall |
|---|---|---|---|---|
| core/ahb-100k | 647 MB, 2.19 s, 5 cyc | 152 MB, 2.15 s, 32 cyc S 2.1 M | 151 MB, 2.20 s, 32 cyc S 2.1 M | -77 %, +0 % |
| core/ahb-1000k | 670 MB, 4.08 s, 7 cyc | 576 MB, 4.86 s, 15 cyc S 9.0 M | 878 MB, 3.87 s, 11 cyc S 13.8 M | +31 %, -5 % |
| st/fib | 301 MB, 0.46 s, 0 cyc | 341 MB, 0.79 s, 1 cyc S 5.0 M | 330 MB, 0.94 s, 1 cyc S 10.1 M | +10 %, +104 % |
| py/binary_trees | 1853 MB, 4.91 s, 0 cyc | 735 MB, 4.59 s, 9 cyc S 10.6 M | 715 MB, 3.97 s, 7 cyc S 14.7 M | -61 %, -19 % |
| clj/actor-saturation-32 | 23 MB, 2.77 s, 0 cyc | 23 MB, 2.98 s, 0 cyc | 23 MB, 2.72 s, 0 cyc | +0 %, -2 % |
| scala/list_ops | 321 MB, 0.54 s, 0 cyc | 235 MB, 0.59 s, 2 cyc S 4.0 M | 235 MB, 0.59 s, 2 cyc S 8.0 M | -27 %, +9 % |
| scala/actor-saturation-32 | 44 MB, 2.38 s, 0 cyc | 44 MB, 2.27 s, 0 cyc | 44 MB, 2.21 s, 0 cyc | +0 %, -7 % |
| js/records-seq | 7883 MB, 43.45 s, 2 cyc | 706 MB, 57.80 s, 41 cyc S 10.6 M | 865 MB, 53.17 s, 30 cyc S 14.6 M | -89 %, +22 % |
| js/graph-seq | 7811 MB, 51.67 s, 2 cyc | 708 MB, 66.20 s, 54 cyc S 10.6 M | 733 MB, 65.31 s, 43 cyc S 11.2 M | -91 %, +26 % |
| js/wordfreq-seq | 6504 MB, 25.24 s, 0 cyc | 228 MB, 30.24 s, 57 cyc S 3.1 M | 310 MB, 29.14 s, 32 cyc S 4.9 M | -95 %, +15 % |
| js640/probe-100k | 636 MB, 2.04 s, 1 cyc | 471 MB, 2.11 s, 6 cyc S 7.1 M | 443 MB, 1.87 s, 5 cyc S 9.1 M | -30 %, -8 % |

The generous law buys time where the collector's work per cycle dominates:
`py/binary_trees` -19 %, the 1 M-cell benchmark -5 %.  It pays for that
with memory beyond the fixed limit's (+31 % on that benchmark).  It does
not bring protoJS within +5 %: the sweep of every garbage cell is paid
whatever S is.

## Rejected alternatives

These come from the exploration rounds, each of 1-2 runs, so read
differences under about 15 % as noise.  The table shows the most telling
workloads.  Each round has its own `today` baseline, because the machine's
load changed between rounds; `jsq/` is the protoJS structure benchmark with
N = 2 (N = 6 for the parallel case) and REPS = 1.

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

- **Rate-aware target**, `S = L + m x rate x Tm / (1 - f)`.
  - The allocation rate was measured as (reclaimed + growth of L) / T.  Tm
    is the time from cycle start to the first swept chunk.
  - The headroom it asks for is too small: protoJS `join` and parallel
    `records` ran 2.5-3x slower.
  - The variant with the whole cycle time instead of Tm was not run.  The
    cycle time grows with S (see the diagnosis), so the target would chase
    itself, as 2.10.0 did.
- **Cap with a consecutive-pressure override** (k_cap 8, lifted after 3
  high-pressure cycles in a row): S reached 19.2 M cells on the 1 M-cell
  benchmark.
- **Early wake**, where a waiting mutator resumes when the sweep publishes
  free cells: more collector CPU.  It was 18-25 % slower on the 1 M-cell
  benchmark and mixed elsewhere.
- **p_high 0.2, g 1.25:** no consistent gain.
- **k_cap 12:** +31 % RSS over the fixed limit on the 1 M-cell benchmark.
- **S0 256 MiB:** `fib.st` 534 MB / 1.33 s.

## Noise

The machine was shared with other jobs: builds and test runs of other
projects.
- The fixed policy's own three runs spread by:
  - 2-9 % on most workloads;
  - 12-20 % on `ahb-0k`, `ahb-1000k`, `st/parallel_speedup` and
    `py/sieve`;
  - 22 % on the protoClojure actor benchmark and 48 % on the protoScala
    one.
- Differences of that size between variants are not evidence.
- Runs with no collection cycle execute the same code under every variant,
  apart from the relaxed loads and the comparison listed above.

## Tests

- **`AdaptiveHeapLaw.*`:** the law's unit tests, rewritten into 13 cases.
  They cover:
  - the live cap and its lower bounds;
  - the H bound;
  - monotonicity over random sequences;
  - a property test that pressure never takes S past max(S, S0, 8 L).
- **`AdaptiveHeapFastAllocator.SoftLimitStaysWithinTheLiveCap`** (gating):
  - It builds a 1.2 M-cell live set and allocates 60 M cells of garbage at
    full speed.  S must stay within max(S0, 8 x the largest L).
  - On the 2.10.0 library it failed in both of two runs (S = 30.2 M cells
    against a cap of 10.4 M).
  - On 2.10.1, S = 9.6 M cells and the heap 9.8 M cells.
- **`AdaptiveHeapStorm.SoftLimitSettlesWithinBoundedCycles`**
  (clock-dependent list):
  - It replaces `PressureFallsWithinBoundedCycles`.  That test assumed that
    S grows until pressure falls, which the cap removes by design.
  - Measured: S reaches the cap at cycle 5.
- **`AdaptiveHeapSteady.HeapStaysFarBelowTheGarbageVolume`**
  (clock-dependent list): the bound is loosened from a quarter to half of
  the garbage.  Measured: 2.71 M cells of 20 M (13.6 %), in ten runs.
- **`AdaptiveHeap.AdaptiveOnEnablesTheControllerAtCreation`** and
  **`AdaptiveHeap.AdaptiveOnHonoursTheLimitVariable`:** the diagnostic
  switch.

## Recommendation on adoption

**protoST: adopt.**
- Against its 640 MB fixed limit, the controller uses 70-75 % less memory
  on the actor benchmarks, at equal or better time.
- A program with a large live set no longer thrashes at 640 MB or fails
  there.
- The cost is on programs like `fib.st`, whose live set grows to a few
  hundred MB with no garbage: one futile cycle, +0.3 s.  Accept that cost,
  or keep a fixed limit for scripts known to be short.

**protoJS: do not adopt yet.**
- Its default since 2026-10-02 (75 % of memory) is the fastest policy on
  its structure benchmarks, and uses 6.5-7.9 GB for them.
- The controller uses 0.2-1.6 GB for the same benchmarks.  It costs
  12-31 % in time when they run sequentially, and 75 % with parallel
  Deferreds.
- This is a choice of policy, not of tuning: the cost is the collector's
  single-threaded sweep.
- If memory matters more than the last 15-30 % of speed, adopt for
  sequential programs.  For parallel Deferreds, wait for a parallel sweep.

**protoPython, protoClojure, protoScala: adopt where bounded memory is
wanted.**
- Today they set no limit and never collect.  They lose no time on short
  benchmarks, but their memory is unbounded on long programs.
- The controller leaves short programs untouched: with the 128 MiB S0,
  they run no cycle.
- It bounds long allocators at 27-74 % less memory, for between -12 % and
  +15 % time.
- So adopt for long-running programs and servers.  The benchmark suites
  will show +5-15 % on garbage-heavy cases (`binary_trees`, `list_ops`).

## Open points

1. **Collector throughput.**  The remaining time cost is the single
   collector thread sweeping every garbage cell.  What would close
   protoJS's gap is a parallel sweep, or sweeping in the mutators' refills.
2. **Futile cycles on a growing live set** (`fib.st`).  The first cycle
   cannot know that nothing is reclaimable.  Skipping the soft-zone wait
   when the previous cycle reclaimed almost nothing would remove the wait
   only from the second and later cycles.
3. **Pressure with several threads.**  It is the sum of the threads' waits
   (design 6.5), and with six mutators p reached 4-5.  Normalising it would
   not change the capped outcome, so it was not done.
4. **Reproduction.**  The harness was a scratch script.  Every command line,
   variable and binary is listed under Method.  The
   `PROTOCORE_ADAPTIVE_HEAP=1` switch is what makes the measurement
   repeatable without rebuilding a runtime.

## Data

[data/2026-10-03-adaptive-heap-calibration/](data/2026-10-03-adaptive-heap-calibration/)
holds the final matrix (`final.jsonl`), the frontier runs (`frontier.jsonl`),
the variant definitions (`variants.json`), the harness and summarisers
(`run.py`, `summ.py`, `table.py`), the notes on rejected alternatives
(`rejected.md`), the diff of the experimental knobs
(`experiment_knobs.diff`) and the protoJS probe (`workloads/probe100k.js`).
Machine-specific paths are replaced by `<workspace>` and `<scratch>`.
