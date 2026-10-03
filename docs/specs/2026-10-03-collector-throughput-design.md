# Collector throughput and heap sizing: two complementary parts

Status: **draft for review**.  Both parts are to be reviewed by the
maintainer before any implementation.  Date: 2026-10-03.  Base: protoCore
2.10.2 (master `294822fc`).  Author: Gustavo Marino, with Claude.

Inputs:
[../reports/2026-10-03-gc-phase-breakdown.md](../reports/2026-10-03-gc-phase-breakdown.md)
(the data this spec answers; "the phase report" below),
[../reports/2026-10-03-adaptive-heap-calibration.md](../reports/2026-10-03-adaptive-heap-calibration.md),
[2026-10-02-adaptive-heap-controller-design.md](2026-10-02-adaptive-heap-controller-design.md),
[../GarbageCollector.md](../GarbageCollector.md).  Line numbers refer to
`294822fc`.

This is a design, not an implementation.  Nothing in it has been built or
measured beyond what the phase report measured.  Every expected gain below is
a bound, an estimate or a hypothesis, and is labelled as such.  The phase
report's workloads are synthetic benchmarks written for this platform.  This
spec uses them as evidence about mechanisms, and does not fit its acceptance
to them (section 9).

## 0. Summary

Mutators wait for the collector in two different regimes.  This spec
proposes one remedy for each.  The remedies are complementary, not
alternatives.

- **Regime 1: the collector keeps up** (reclamation throughput T at least
  the allocation rate r).  The waits are a sizing and timing problem.  A
  cycle requested early enough, with headroom of about r x cycle duration
  inside a generous memory budget, removes them.  More headroom also lowers
  the total collection work, because the live set is re-marked and the
  survivors re-swept fewer times.
- **Regime 2: allocation outruns the collector** (r > T).  Memory only
  postpones the wait: headroom H lasts H / (r - T) seconds, unless the run's
  whole garbage fits in the budget.  Only a faster collector helps, and a
  faster collector shrinks regime 2.

The parts:

- **Part A, heap sizing** (section 4).
  - **Objective:** *minimise time within a memory budget*, replacing the
    2.10.x calibration's *minimise memory within a time tolerance*.  The
    budget is the hard limit of the process sizing rule.  Memory is never
    returned to the operating system, so memory inside the budget is there to
    be used.
  - **Pacing:** request a cycle when the cells left fall below
    r x C (C: the measured cycle duration), under fixed limits and under the
    controller alike.  The wait ends when cells arrive, not on the 50 ms
    watchdog.
  - **Soft limit:** set from the measured r, T and live set while r < T, and
    grown while the growth measurably reduces waits.  Growth stops where a
    larger S only lengthens cycles, which is the signature of regime 2.  The
    fitted constants `k_live`, `k_cap` and `p_high` go away.
- **Part B, collector throughput** (sections 5-7).
  - **Diagnosis first** (section 5).  The sweep's per-cell cost triples with
    concurrent allocators, and the cause is unmeasured: coherence, DRAM
    latency, contended shared lines or finalizers.  The plan covers counters,
    `perf c2c` and stall attribution, a differential microbenchmark, and
    seven cheap fixes, including batched segment recycling and a multi-cursor
    sweep.  A gate follows before any threading.
  - **Parallel sweep** (section 6).  The collector thread plus K helpers
    from one process-wide pool.  Segments are claimed in runs, each sweeper
    keeps its own state and publishes in batches.  Phase 5b, the bulk unmark
    and the token stay on the collector thread.  Embedder finalizers stay
    serial on the collector thread.  There are no barriers, no model change
    and no ABI change.
  - **Mark stays serial** (section 7), with explicit triggers for revisiting
    it.
- **Acceptance as properties** (section 9):
  - no mutator wait when the collector has spare capacity and the budget
    allows the headroom;
  - the budget is never exceeded;
  - no out-of-memory while the live set fits;
  - bounded convergence;
  - no single-thread regression beyond noise.

  A memory-as-only-variable experiment (limits from 10 M to 400 M cells)
  classifies each synthetic workload's regime, reported as evidence.

## 1. Problem

From the phase report (synthetic workloads, one run per configuration, AMD
6 cores / 12 threads):

- The sweep is 41-99 % of the collector's busy time (median 61 %) and the
  largest phase on 30 of 32 completed runs.  Stop-the-world is at most
  0.23 ms per cycle.  Phase 5b and the bulk unmark together are 0-14 %.
- With 12 allocating threads, the single collector is busy 56-95 % of the
  wall time.  The mutators wait for headroom 17-63 % of their thread time.
  protoScala and protoClojure with 6 tasks: 92-94 % busy, 45-66 % waiting.
- Sweep cost per cell rises with concurrent allocators: protoClojure 45 ->
  143 ns (t1 -> t6, same cells per cycle), protoScala 40 -> 158 ns, protoJS
  `records` 17 -> 58 -> 85 ns (N = 1, 6, 12).  The collector thread was on
  the CPU 99-100 % of the sweep's wall time, so each cell costs more; the
  collector is not being descheduled.
- At N = 6 with a 40 M-cell limit, the collector is busy for less time than
  the mutators' own work, yet the mutators wait 10-22 %.  A fixed limit
  requests a cycle only at the ceiling.
- At N = 12 the adaptive controller was slower than the fixed limit on all
  five workloads (+2 % to +91 %).

**A derived fact the phase report does not state.**  Dividing each run's
`headroom_wait` by its number of waits gives the mean length of one
`reclaimWaitLocked` call:

| Run | Waits | Mean wait |
|---|---:|---:|
| js40_records_n6 | 428 | 50.2 ms |
| js40_records_n12 | 4,603 | 50.2 ms |
| js40_graph_n12 | 7,233 | 50.3 ms |
| jsad_records_n12 | 7,094 | 49.4 ms |
| jsad_wordfreq_n12 | 3,887 | 49.1 ms |
| scala_tree_t6 | 4,824 | 50.2 ms |
| clj_coll_t6 | 10,253 | 50.2 ms |
| cad_20k | 13,695 | 50.2 ms |
| core_fixed640_live1M (1 thread) | 25 | 50.0 ms |

A wait is bounded by the 50 ms watchdog of `reclaimWaitLocked`
(`core/ProtoSpace.cpp:1755-1756`).  A mean this close to 50 ms means nearly every
wait ended on the watchdog, not on a notification.  Under a fixed limit this
follows from the code.  The predicate waits for a change of `gcCycleCount`,
which counts cycle **starts** (line 1761).  `memoryReclaimedCV` is notified
only at cycle end (line 1221).  A wait that begins inside a running cycle
therefore always lasts 50 ms.  The thread then re-checks the freelist
(`waitForHeapHeadroom`, line 1833) and waits again if it is still empty.
Under the controller, the cycles are longer than 50 ms, so the watchdog fires
first there too.  The mutators therefore poll the freelist every 50 ms while
the sweep publishes chunks.  The share of the measured wait that is polling
latency, rather than missing cells, is unmeasured (section 5.2, counter C6).

## 2. Two regimes

Let r be the rate at which the mutators would produce garbage if they never
waited.  Let T be the collector's reclamation throughput: cells reclaimed per
second of collector busy time, mark included.  With headroom G (cells free or
growable when a cycle starts), live set L and a cycle duration of about
`C = (L + G) / T'` (T': cells processed per busy second; mark visits L, sweep
visits the candidates):

- **A cycle hides behind the mutators** when its runway lasts as long as the
  cycle: `G >= r x C`.  Substituting C and writing `rho = r / T'`:

  ```
  G (1 - rho) >= rho x L        =>        G* = rho x L / (1 - rho)
  ```

  G* is finite only while `rho < 1`.  **Regime 1** is `rho < 1` with
  `L + G* <= budget`: some headroom removes the waits, and it is found by
  measurement, not tuning.
- **As rho approaches 1, G* diverges.**  Each increase in headroom lengthens
  the cycle almost as much as it lengthens the runway.  This is what the
  calibration observed when its rate x cycle-time target "chased itself" and
  2.10.0's S ran towards H.  It is a property of the regime, not of the
  constants.
- **Regime 2** is `rho >= 1`, or `L + G*` above the budget.  Headroom H then
  lasts `H / (r - T)` seconds before the mutators are paced by the collector,
  whatever the heap size.  The exception is a run whose total garbage fits in
  the budget, so that it never needs a cycle that blocks: no cycle-time
  argument applies to it.
- **A larger heap also raises T.**  Every cycle re-marks the live set and,
  with survivor re-inclusion, re-sweeps the survivors.  Fewer cycles mean
  less of that fixed work per reclaimed cell.  Mark is 19-34 % of busy time
  in the protoJS 40 M-cell N = 12 runs and 28 % in CAD.  The boundary
  between the regimes therefore depends on S, which is why Part A measures
  the marginal effect of growing S instead of predicting it.

**Estimates from the phase report** (synthetic workloads, one run each).
- *Demand* is cells freed / mutator floor (wall time minus waits per
  thread); it approximates r, assuming the waits were spread evenly.
- *Capacity* is cells freed / collector busy time; it approximates T at the
  run's own heap size.

| Run | Demand (M cells/s) | Capacity (M cells/s) | Regime at this heap size | Garbage freed (GB) |
|---|---:|---:|---|---:|
| js40_records_n6 | 13.7 | 16.2 | 1 | 11.2 |
| js40_graph_n6 | 15.7 | 20.5 | 1 | 15.5 |
| js40_wordfreq_n12 | 14.3 | 21.1 | 1 | 11.5 |
| js40_records_n12 | 15.0 | 9.1 | 2 | 21.5 |
| js40_graph_n12 | 17.1 | 9.3 | 2 | 33.6 |
| jsad_records_n12 | 18.6 | 9.0 | 2 | 20.3 |
| scala_tree_t6 | 6.5 | 3.9 | 2 | 20.4 |
| clj_coll_t6 | 18.0 | 6.4 | 2 | 49.9 |

Reading:
- The regime-1 rows still waited 10-22 % (N = 6) and 17 % (`wordfreq`
  N = 12).  The cycle started at the ceiling, which is a timing problem.
- With a 40 M-cell limit at N = 1, four of five protoJS workloads ran no cycle
  at all, so enough memory removed the collection entirely.
- In regime 2, js40 `records` N = 12 frees about 400 M cells (21.5 GB at
  64 bytes) in the whole run.  That fits in a budget of 75 % of this
  machine's 62 GB (46 GB), so a budget-sized heap would remove its waits
  without any collector speed-up.  `clj_coll_t6` (50 GB) would not fit.
- The same numbers show why Part B matters.  Doubling T moves `records`,
  `graph` and `scala_tree` N = 12 / t6 to `rho < 1`, but not `clj_coll_t6`
  (rho = 2.8).

## 3. Constraints

- **No model change.**  The sweep stays a sweep of the segments captured in
  Phase 2.  There is no new cell kind, no new collector subsystem and no
  special case for a language.  Helpers run the same sweep code as the
  collector thread, and the serial sweep is the K = 0 case of the parallel one
  ([feedback: purity over performance]).  Collector throughput is
  paradigm-aligned work: immutable style produces more garbage, so the sweep
  is the platform's cost, not a mutable-style artefact.
- **No write barriers, and mark is unchanged.**  "Concurrent Mark Without
  Barriers" (GarbageCollector.md) holds as written.
- **No ABI change.**  The layouts of `ProtoSpace`, `ProtoContext` and `Cell`
  in `headers/protoCore.h` stay as they are, and no virtual is added.  New
  state lives in side structures owned by `core/` (the pattern of
  `core/AdaptiveHeap.cpp` and of the `popLocks` array in
  `submitYoungGeneration`).  `DirtySegment` is declared in
  `headers/proto_internal.h`, which is not installed
  (`CMakeLists.txt:113`, only `protoCore.h` is a `PUBLIC_HEADER`), so its
  layout may change.
- **Finalizer contract** (GarbageCollector.md § 7).  Finalizers run on
  collector-owned threads, never on mutator threads.  They never allocate,
  never publish with CAS, never loop over protoCore data and never block.
- **Synchronisation primitives.**  Never `std::counting_semaphore` or
  `std::binary_semaphore`: libstdc++ 13 loses wakeups (protoST S19).  This
  design also avoids `std::latch` and `std::barrier`, which sit on the same
  atomic-wait machinery and are not needed.
- **Process sizing rule.**  The heap is still the sum of each space's peak.
  Helpers allocate no cells and no heap.
- **Memory budget.**  Part A never lets the sum of the spaces' heaps
  exceed the budget B, and it keeps the out-of-memory rule of 2.10.x.

## 4. Part A: heap sizing (minimise time within a memory budget)

### 4.1 Objective

**Recommended objective:** minimise the time mutators lose to collection,
subject to the sum of the spaces' heaps never exceeding the budget B.

- **B** is the hard limit of the process sizing rule (perennials plus the sum
  of each space's peak): the adaptive controller's H.  Its default is 75 % of
  the smaller of physical memory and the process memory limit.  The embedder
  or the operator (`PROTOCORE_HEAP_LIMIT_CELLS`) can set it.
- **Why memory is there to be used.**  protoCore never returns memory to the
  operating system.  A heap that stays well below B buys nothing for the
  process; B is the agreement with the rest of the machine.  Within it, a
  cell of headroom that removes a wait is worth having.
- **Contrast with 2.10.x.**  The calibration minimised memory within a time
  tolerance.  `p_high = 0.05` was the tolerance, and `k_live = 3` and
  `k_cap = 8` bounded memory to a multiple of the live set.  Its own report
  records the consequence: protoJS +12 % to +75 % in time against a policy
  that collects less.  Under the recommended objective, the cap is B, and the
  growth stops for a measured reason (4.5), not at a fitted multiple.
- **The heap does not grow for nothing.**  S grows only while mutators wait
  and growth reduces the waits.  A program with a low allocation rate needs
  little runway and keeps a small heap, as it does today.

This is a change of policy.  It is the maintainer's decision (section 13,
question 1).

### 4.2 Signals

All signals are measured per space at cycle end, on the collector thread.
None touches the allocation fast path.

| Signal | Definition | Where |
|---|---|---|
| r | cells handed out by `getFreeCells` in the interval, divided by the time the mutators were *not* waiting: `T_int x (1 - w)`.  This is an estimate of the unthrottled rate. | counter under `globalMutex`, which the refill already holds |
| C | cycle duration, from request to completion | `onCycleStart` / `onCycleEnd`, as for `Tc` in the trace today |
| T | cells reclaimed per second of collector busy time | busy time per cycle (the phase report's `busy`, made permanent and cheap: two `steady_clock` reads per phase) |
| w | wait share per thread, `P / (T_int x threads)` | the existing P, normalised.  This answers the calibration's open point 3: P summed over threads reached p = 4-5 with six mutators. |
| u | collector utilisation, `busy / T_int` | as above |
| L, R | live set; cells left unreclaimed | existing |

### 4.3 Pacing: when a cycle starts

One rule for fixed limits and for the controller.  The ceiling is S under the
controller and `maxHeapSize` under a fixed limit.

```
left   = freeCellsCount + max(0, ceiling - heapSize)
runway = min(max(0, ceiling - R),  r x C x (1 + m))
request a cycle when left < runway (and none is requested or running)
```

- **The one constant.**  m is a structural slack fraction: one quarter of a
  cycle of measurement error, m = 0.25.  It is not fitted to a workload.
- **Conservative inputs.**  r and C are the larger of the last two cycles'
  values, which avoids a smoothing constant.
- **First cycle.**  Before it, `runway = 0`, which is today's behaviour.
- **A full-headroom runway is information.**  When the runway equals the
  whole headroom (`ceiling - R`), the cycle starts as soon as the previous
  one ends: the collector cannot keep up at this ceiling.  That is the input
  to 4.5.
- **Implementation.**  The pure function `pacing::runway(...)` is
  unit-tested.  It lives in the hook `adaptive::pace` already occupies
  (`core/AdaptiveHeap.cpp:402`, called from `getFreeCells` under the lock).
  The state lives in the side structure keyed by space, extended to
  fixed-limit spaces.
- **Replaces** the controller's fixed quarter of `S - L`
  (`kTriggerFraction = 0.75`, `core/AdaptiveHeap.h:51`).

**Today, for reference.**
- `PROTOCORE_HEAP_LIMIT_CELLS=<hard>` (the phase report's js40/js10 runs)
  sets `softHeapLimit = 0`.  A fixed-limit space then requests a cycle only
  when a refill finds the heap at `maxHeapSize` with an empty freelist
  (`getFreeCells`, `ProtoSpace.cpp:2081-2091`).  By then every mutator that
  needs cells is about to wait.
- With a soft limit, a refill at or above it waits for one cycle (the "soft
  zone"), which is a wait as well.
- At N = 6 with a 40 M-cell limit, the collector is idle most of the run, yet
  the mutators wait 10-22 %.  The single-threaded `core_fixed640_live1M`
  waits 1.25 s of a 3.6 s run (25 waits of 50 ms).

**Costs.**
- Cycles start with less garbage, so there are more of them, each paying a
  mark of the live set.  The runway is at most the headroom, and it is
  `r x C`, not a fixed fraction, so cycles are no more frequent than the
  collector needs to keep up.
- Mutators running at the request pay the stop-the-world quorum.  That is
  near zero today, because waiting mutators are already out of the running
  set.  Under the controller's pacing at N = 12 the quorum was 28-270 ms per
  run and the parked time 0.4-2.7 s summed over 12 threads, below 1 % of
  thread time.

**This is not the calibration's rejected rate-aware target.**
- That target *sized the soft limit* from rate x mark time.  Here r x C only
  times the request.
- Where S is sized (4.5), the divergence that defeated the rate-aware target
  is detected and acted upon instead of chased.

### 4.4 Waits that end when cells arrive

The mean wait is about 50 ms in every multi-threaded run (section 1): the
mutators poll the freelist on the watchdog.

- **Fixed limits.**  `reclaimWaitLocked` keeps its predicate.  In addition,
  the callers of `publishFreeChunk` call `notify_one` on
  `memoryReclaimedCV` whenever a waiter count is non-zero.  The count is
  changed around the wait, under `globalMutex`, which publication holds
  anyway.  `waitForHeapHeadroom` re-checks the freelist after any wake
  (line 1833), so the notify needs no new predicate.  It costs nothing when
  no thread waits.
- **Controller.**  Early wake was rejected in the calibration: 18-25 %
  slower on the single-threaded 1 M-cell benchmark, mixed elsewhere.  It is
  re-measured at N = 12 with counter C6 (section 5.2), which separates
  polling latency from missing cells.  It is adopted if it reduces the wait
  share without raising collector busy time.
- **Out of memory.**  The OOM strike logic is unaffected.  A wake that finds
  an empty freelist goes through the same "cycle completed?" check
  (line 1839).

### 4.5 The soft limit: sized from measurements, grown while it pays

Under the controller, after each cycle (pure function
`adaptive::nextSoftLimit`, extended):

```
rho = r / T
if rho < 1:                      # regime 1 at the current S
    G*     = rho x L / (1 - rho)
    target = min(B, L + G* x (1 + m))
    S'     = max(S, target)      # non-decreasing, as today
else:                            # regime 2 at the current S
    if w > 0 and growth not stopped:
        S' = min(B, S x 2)       # probe: does a larger heap reduce waits?
    else:
        S' = S
after each probe: if w did not fall below the previous cycle's w
                  on two consecutive probes, stop growth (hold S)
re-arm growth when L or r changes by a factor of 2 since the stop
```

- **Regime 1** gets the headroom that lets a cycle run behind the mutators.
  The formula is the condition of section 2, not a fitted multiple.  Once
  `S >= L + G*`, pacing (4.3) does the rest, and the expected wait is zero.
- **Regime 2** gets probes.  A larger S cuts the per-cycle fixed work
  (re-mark, survivor re-sweep), which raises T and may bring rho below 1.
  It may also let a run's whole garbage fit (js40 `records` N = 12, section
  2).  If two consecutive doublings did not reduce the wait share, a larger
  heap is only lengthening cycles.  That is the regime-2 signature, and
  growth stops there: memory is not spent where it buys no time.
- **What stays from 2.10.x:**
  - S is non-decreasing;
  - S never exceeds B;
  - S0 is kept as the starting point;
  - the out-of-memory rule is kept (the cells left unreclaimed after a cycle,
    plus the other spaces' heaps, exceed B less one batch per thread, on two
    consecutive cycles).
- **What goes:** `k_live`, `k_cap` and `p_high`.  The remaining constants are
  structural:
  - m = 0.25, the cycle slack;
  - doubling, the fewest probes, at most `log2(B / S0)`: 9 from 128 MiB to
    64 GiB;
  - two non-improving probes, to absorb a single noisy cycle;
  - a factor-2 change to re-arm.

  None is chosen by fitting a benchmark.
- **Convergence is bounded.**  At most `log2(B / S0)` doublings in total, and
  no oscillation, because S never decreases.  Re-arming needs a factor-2
  change in L or r, so it happens at most `log2` of the range of either.
- **Fixed limits** (`setHeapLimits`) keep the embedder's S and H.  Only
  pacing (4.3) and early wake (4.4) apply to them.

### 4.6 Why the controller was slower at N = 12

Same hard limit (40 M cells), fixed against controller, from the phase
report:

| Workload | Cycles | Mark s | Sweep s | Busy s | Wait s (sum) | Quorum ms |
|---|---|---|---|---|---|---|
| records | 10 -> 22 | 7.5 -> 8.7 | 27.0 -> 24.3 | 37.0 -> 35.1 | 231 -> 351 | 0.9 -> 57 |
| join | 8 -> 17 | 10.0 -> 12.7 | 18.3 -> 20.9 | 29.9 -> 35.5 | 178 -> 208 | 10.5 -> 116 |
| doctree | 6 -> 14 | 6.0 -> 9.2 | 13.4 -> 17.3 | 21.7 -> 29.3 | 121 -> 167 | 0.4 -> 28 |
| wordfreq | 5 -> 24 | 1.7 -> 8.2 | 6.0 -> 10.7 | 8.5 -> 20.2 | 31 -> 191 | 1.9 -> 261 |
| graph | 17 -> 21 | 15.5 -> 20.6 | 37.3 -> 35.9 | 56.3 -> 60.7 | 364 -> 396 | 2.0 -> 270 |

- **More cycles on a saturated collector** (69-95 % busy).  The 2.10.1 cap
  holds S at `8 L`, below H at these live sets.  Whether S actually sat at
  the cap is not in the phase report; the `PROTOCORE_HEAP_TRACE` lines must
  confirm it.
- **Each extra cycle pays a fixed cost:** the mark of the live set and the
  re-sweep of the survivors.  On `wordfreq` mark grows 4.8x for the same
  work.  On a saturated collector that is mutator wait almost one for one.
- **`wordfreq` N = 12 is regime 1 at the fixed limit** (section 2).  The
  controller's smaller heap pushed it towards regime 2: busy time 8.5 ->
  20.2 s.  This is the cost of minimising memory, and the case the
  recommended objective changes: 4.5 would have kept growing S while waits
  fell.
- **The 2.10.1 signal could not see this.**  p is summed over threads and far
  above `p_high` at every size, so it says only "grow to the cap".  The
  marginal test (does the wait share fall when S doubles?) and rho
  distinguish "too small" from "collector too slow".

### 4.7 Several spaces

- The budget B stays process-wide, enforced as today: each enabled space's
  `maxHeapSize` is its heap plus what is left of the budget
  (`adaptive::recomputeCeilings`).
- Each space sizes its own S by 4.5.  Growth competes for the remaining
  budget first come, first served, as heap growth does today.
- A fairer split (for example in proportion to each space's wait reduction
  per cell) is an open question, not part of v1.

## 5. Part B, step 1: diagnose the per-cell cost before parallelising

### 5.1 What the sweep does per cell today

`gcThreadLoop`, Phase 5 (`core/ProtoSpace.cpp:918-1056`), for each segment of
`segmentsToProcess` and each cell of its `cellChain`:

| Event | Operation | Line kind |
|---|---|---|
| every cell | `getNext()` (seq_cst load) and `isMarked()` (load) of `next_and_flags` | read of the cell's only line (cells are 64-byte aligned, one cell per line) |
| dead cell | virtual `finalize()`: an indirect call through the vtable (word 0 of the same line).  Non-trivial only for `ProtoObjectCell` with `mutable_ref > 0` (push to `gcFinalizedMutableRefs`), `ProtoExternalPointer` (embedder callback), `ProtoExternalBuffer` (`alignedFree`) and `ProtoByteBuffer` (`delete[]`) | read, plus `free()` into the allocating thread's malloc arena |
| dead cell | `internalSetNextRaw(batchHead)` (relaxed store) | write: the line is taken exclusive |
| survivor | `unmark()`: `fetch_and`, a locked read-modify-write | write to a **live** cell's line |
| survivor | `setNext(survHead)`: load, then release store | second write to the same line |
| every segment (about 6 cells: "~5.86 cells avg", comment at line 985) | CAS push onto `survivorPen` (survivors) or onto `dirtySegmentFreePool` (none) | the pool head is the line that **every mutator** pops in `submitYoungGeneration` (`ProtoSpace.cpp:2255`), at each context destruction |
| every 8,192 cells (`CELL_CHUNK_SIZE`, `protoCore.h:2652`; the phase report says 4,096) | `publishFreeChunk` under `ProtoSpace::globalMutex`, a **process-wide static** recursive mutex (`protoCore.h:2834`) that every refill of every space also takes in `getFreeCells` | lock line, `freeChunks` head, `freeCellsCount` |

Around the sweep:

- Mark sets bit 0 with `fetch_or` on every reached cell
  (`ProtoSpace.cpp:862`), which is a write to every live line.
- Phase 6 runs `fetch_and` again on every entry of `markedList`
  (lines 1110-1114).  That includes survivors the sweep already unmarked, so
  each live candidate cell is written four times per cycle: mark, unmark,
  relink, unmark.

A dead cell's line was written last by the mutator that built it, possibly on
another core.  After the sweep, the cell goes into a free chunk that another
mutator will write.  A survivor's line is read by mutators, and every
collector write to it invalidates their copies.

### 5.2 Hypotheses and how to tell them apart

| # | Hypothesis | Predicts |
|---|---|---|
| H1 | **Coherence on swept cells.**  Dead cells are still Modified in the allocating core's private cache, and survivors are Shared with readers.  Each line costs a cross-core or cross-CCX transfer. | High cache-to-cache (HITM / remote-cache fill) counts on the collector, at the `next_and_flags` load (`ProtoSpace.cpp:936-937`) and the survivor writes; the cost scales with the number of mutators that touch the cells, not with their memory traffic. |
| H2 | **Loaded DRAM latency.**  The chains are scattered (recycled chunks are prepended in sweep order), so every cell is a dependent miss.  N mutators raise the loaded latency. | DRAM fills dominate.  The cost also rises when the extra threads only stream private memory, and falls with chain contiguity. |
| H3 | **Contended collector-shared lines.**  The per-segment CAS on `dirtySegmentFreePool` (pushed by the collector, popped by every mutator at every function return), `survivorPen`, `globalMutex`/`freeChunks`, and 16-byte `DirtySegment` structs sharing lines across threads (allocated with `new`, recycled between threads). | Samples concentrated on the CAS loops at lines 1012-1018, 1022-1028 and 1037-1042 and on `publishFreeChunk`; CAS retry counts that grow with N; HITM on `DirtySegment` lines.  At about 6 cells per segment, +100 ns per cell is about +600 ns per segment, the order of one contended line transfer with retries. |
| H4 | **Finalizer work.**  `free()` from the collector into the arena of the thread that allocated the memory, contending with that thread's `malloc`. | Cost correlated with the count of `ExternalBuffer`/`ByteBuffer` finalizations; samples in glibc `free` / `_int_free` lock paths.  Likely small on the phase report's workloads (to be verified). |
| H5 | **Writes to live lines.**  The survivor unmark and relink (sweep) and the Phase 6 unmark force line ownership away from readers. | Cost scales with the survivor share (`swept - freed`).  protoClojure frees 99 % of what it sweeps and still triples, so H5 cannot be the main cause there. |

Note on the phase report's argument against lock contention: the collector
was on the CPU 99-100 % of the sweep.  That excludes *blocking* on
`globalMutex`.  It does not exclude H3, because a contended CAS or a cache
line transfer stalls on the CPU.

**Measurements** (on branch `measure/gc-phases`, compiled out by default,
like the existing `[GC-PHASES]` fields):

- C1: segments swept per cycle, and cells per segment (mean and a
  power-of-two histogram).
- C2: CAS retries on `dirtySegmentFreePool` and `survivorPen` in the sweep,
  and on `dirtySegmentFreePool` pops in `submitYoungGeneration`.
- C3: time spent acquiring `globalMutex` in `publishFreeChunk`, and the
  number of publications.
- C4: finalizations by cell type, counting only the four non-trivial kinds.
- C5: chain contiguity, the fraction of `next` links within ±1 cell of the
  current cell (a proxy for H2's "scattered").
- C6: the wait histogram.  For each `reclaimWaitLocked` call, record whether
  it ended on the watchdog or on the predicate, and whether the freelist was
  non-empty when it returned.  This splits polling latency from missing cells
  (section 1).

**PMU** (single run per configuration, on an idle machine, never while
another agent builds or tests):

- `perf stat -t <collector TID>` for cycles, instructions, and L2/L3 misses.
  On Zen 2, also the `ls_refills_from_sys.*` (or `ls_mab_alloc` / data-source)
  events that split fills into local L2, other core / other CCX, and DRAM.
  Event names vary by kernel; pick them from `perf list` first.
- `perf record -t <collector TID>` with precise (IBS op) sampling, to
  attribute stall cycles to the instruction: the header load, the CAS loops,
  `fetch_and`, `free`.
- `perf c2c record` where the CPU and kernel support it (Intel; AMD via IBS
  on recent kernels).  If it is unavailable on DEV12, say so in the report and
  rely on the differential benchmarks.

**Differential microbenchmark** (`performance/sweep_contention_benchmark`,
protoCore only, self-verifying: it prints the cells it built, swept and freed,
and checks them against the expected numbers).  One space and a fixed garbage
set of G cells in segments of about 6 cells.  One cycle is measured under
each condition:

| Scenario | Other threads during the sweep | Separates |
|---|---|---|
| S0 | none | baseline |
| S1 | N threads streaming private memory (no protoCore cells) | H2 (load on memory), not H1 or H3 |
| S2 | N threads submitting and destroying empty contexts in a loop | H3 (`dirtySegmentFreePool`, `dirtySegments`) |
| S3 | N threads reading the survivors | H1/H5 on live lines |
| S4 | garbage built by N threads (then idle), against S0 | H1 on dead lines (last writer on another core) |
| S5 | S0 with garbage from fresh contiguous memory, against recycled memory | H2 (contiguity) |

Each scenario prints the sweep's ns per cell.  The pattern of the six numbers
identifies the dominant hypothesis without any PMU support.

### 5.3 Cheap fixes, each measured on its own

Ordered by expected value per line of code.  Each fix is a separate commit
with its own before/after numbers on S0-S5 and on two synthetic runtime
workloads (`clj_coll_t6`, `js10_records_n12`), reported as evidence.

- **F1 Batch segment recycling (H3).**  Chain the processed segments locally
  and push the chain onto `dirtySegmentFreePool` with one CAS: at the end of
  the sweep, or every 1,024 segments so that mutators are not starved of
  segments.  Do the same for `survivorPen`: one CAS per sweep, since the pen
  is folded only under a later stop-the-world.  This removes about one CAS
  per 6 cells from a line that every mutator hammers.  The push side stays
  lock-free.  The pops keep their spin lock (`popLocks`), which this does not
  change.
- **F2 One write per survivor (H5).**  Replace `unmark()` followed by
  `setNext(survHead)` with a single release store of
  `survHead | (old & 0x3E)`.  This is sound because the collector is the only
  writer of a candidate cell's `next_and_flags` during sweep: the invariant
  documented on `Cell::setNext`, `proto_internal.h:852`.  It saves a locked
  instruction, not a line transfer.  Small on its own, and free.
- **F3 Phase 6 tests before it writes (H5).**  `if (m->isMarked())
  m->unmark();`.  A survivor the sweep already unmarked is then only read, so
  the mutators' Shared copies stay valid.  Cells marked outside the segments
  (perennials, young chains of live contexts) are still cleared.  The token
  invariant is preserved: every mark bit is clear before
  `multispace::cycleActive = false`.
- **F4 Multi-cursor sweep (H1/H2: latency).**  Walk M segments (M = 4-8)
  in lockstep.  At each step, prefetch for write (`PROTO_PREFETCH` with write
  intent) the next cell of every cursor before processing the current ones.
  One chain is a dependent pointer chase with one miss in flight.  M chains
  give M misses in flight on one thread.  Each cursor keeps its own batch and
  survivor head, merged as segments end, so the output is unchanged.  This is
  the fix most likely to recover the 3x on one thread if the cost is latency.
  It is also the inner loop the helpers will run.
- **F5 Batched chunk publication (H3).**  Accumulate up to B = 4 chunks
  locally and publish them under one `globalMutex` acquisition.  If C6 shows
  waiting mutators, publish immediately instead (section 4.4).
- **F6 `DirtySegment` alignment (H3).**  If C2 or c2c shows false sharing on
  segment structs, align `DirtySegment` to 64 bytes.  It is internal and not
  installed.  It costs 48 bytes per segment.
- **F7 Finalizer frees (H4).**  Only if C4 and the profile show `free()`
  contention: collect the external pointers to free and release them after
  the sweep.  This is not proposed by default.

**Gate.**  The parallel sweep is implemented only after F1-F4 are measured.
If F1 + F4 bring the per-cell cost at N = 12 to within 1.5x of N = 1, the
report re-evaluates the Amdahl table (section 8) with the new sweep times
before helpers are built.

Out of scope here, recorded for completeness: a **side mark bitmap** would
remove every collector write to live lines (mark, unmark, Phase 6 becomes a
`memset`).  It needs an address-to-bitmap map for every heap block, plus a
fallback for perennial cells that live outside heap blocks (symbols from
`posix_memalign`).  It is a larger change, justified only if H1/H5 dominate
after F2/F3.

## 6. Part B, step 2: parallel sweep with collector helper threads

### 6.1 Shape

The sweep loop body becomes `sweepRun(SweepJob&, SweeperLocal&)`, with the
F4 multi-cursor inner loop.  The collector thread and up to K helpers each
run `claim -> sweepRun -> claim ...` until the segment list is exhausted.
With K = 0, the collector runs it alone and the cycle is today's cycle.
Everything before the sweep (Phases 1-4) and after it (5b, 6, 7, the token,
the cycle-end bookkeeping at lines 1203-1222) stays on the space's collector
thread, in today's order.

### 6.2 Partitioning `segmentsToProcess`

`segmentsToProcess` is a singly linked list, private to the collector after
the Phase-2 `exchange` (line 666) and the pen fold (line 781).
`DirtySegment` has no count, and segments average about 6 cells.

- **Chunked claiming.**  `SweepJob` holds `cursor` (the next unclaimed
  segment) and a claim lock (`std::mutex`).  A claim takes the lock, detaches
  the next R segments by walking R links (R = 128 by default, about 770
  cells), and releases the lock.  The walk under the lock costs R dependent
  loads of 16-byte structs.  At about 100 ns per cell of sweep work, that is
  a lock occupancy of about 2 % per sweeper.
- **No ABA.**  Nodes leave this list only by claims, and no node is ever
  pushed back onto it: a processed segment goes to `dirtySegmentFreePool` or
  to `survivorPen`.  The list's `next` fields are read only under the claim
  lock, so TSan sees ordered accesses.
- **No small-cycle overhead.**  The collector starts alone.  It wakes the
  helpers only if its first claim did not exhaust the list, so a cycle of a
  few thousand cells never touches the pool.
- **Load balance.**  Small fixed runs balance themselves; the tail is at most
  R segments per sweeper.
- **Considered and deferred.**  The collector could pre-split the list into
  stripes, at the cost of a serial pass over every segment.  Per-thread dirty
  stacks would give a natural partition and remove mutator contention on the
  single `dirtySegments` head.  They need a side array keyed by space (the
  `dirtySegments` field is in the ABI), and their merit depends on C2.  That
  is an open question (section 13).

### 6.3 Per-sweeper state and merge

| State | Today (single thread) | Per sweeper | Merge |
|---|---|---|---|
| free chunk under construction | `chunkHead/Tail/Count` | own | published under `globalMutex` at `CELL_CHUNK_SIZE` (or in batches, F5); the partial trailing chunk published by its owner at the end |
| `reclaimedThisCycle` | local | own counter | summed by the collector after the join |
| survivor segments | CAS per segment onto `survivorPen` | own LIFO chain | one CAS of the chain per sweeper (F1) |
| recycled segments | CAS per segment onto `dirtySegmentFreePool` | own chain | one CAS per sweeper (or per 1,024 segments) |
| finalized mutable refs | `space->gcFinalizedMutableRefs` | own `std::vector<proto_ulong>` | appended to `space->gcFinalizedMutableRefs` by the collector after the join, **before** Phase 5b sorts and deduplicates it |
| dead cells (several spaces) | `deadCells` | own vector | concatenated by the collector before the grace period |
| deferred embedder finalizations | none | own vector of `ProtoExternalPointer` cells (6.5) | run by the collector after the join |

Chunk order and pen order are irrelevant today (mutators take any chunk, and
the pen is a set), so a different interleaving changes nothing observable.

### 6.4 Cycle order with helpers

1. Phases 1-4 as today, on the collector thread.
2. The collector publishes the `SweepJob` (space, list head, `deferFree`)
   and sweeps.  The helpers join the job if they are woken (6.2).
3. **Join.**  The collector waits until `cursor == nullptr` and
   `activeSweepers == 0`.  It never waits for a helper to *arrive*, only for
   helpers that have claimed work to finish it.
4. The collector merges the per-sweeper state (6.3) and runs the deferred
   embedder finalizations.  It publishes those cells as one chunk, which
   counts towards `reclaimedThisCycle`.
5. Phase 5b (`releaseFinalizedMutableEntries`), on the collector thread,
   through `gcContext`.  It is unchanged: at most 256 CAS operations, and it
   allocates, so it must stay on the one thread the design allows to allocate
   inside a cycle.
6. Phase 6 on the collector thread with F3.  It parallelises trivially over
   index ranges of `markedList` through the same pool, but this is done only
   if, after F3, it exceeds 5 % of busy time on a workload of the section 9
   experiment.  It is below that today.
7. Phase 7, token release, grace period (several spaces), cycle end: as
   today.

### 6.5 Finalizers

The contract changes in one respect, which needs the maintainer's approval
(section 13): **built-in finalizers may run concurrently with each other**, on
the collector thread and its helpers.

- The built-in ones are safe:
  - `alignedFree` and `delete[]` are thread-safe;
  - `ProtoObjectCell::finalize` (`core/ProtoObject.cpp:515`) appends to a
    vector.  It appends to a thread-local sink set by `sweepRun` (the
    sweeper's own vector) instead of `space->gcFinalizedMutableRefs`, which
    is the fallback when no sink is set.  This change is internal: `finalize`
    is not inline and its signature is unchanged.
- **Embedder callbacks stay serial.**  A helper that meets a
  `ProtoExternalPointer` cell (`getType() == CellType::ExternalPointer`)
  records it instead of finalizing it.  The collector thread finalizes all
  recorded cells, one at a time, after the join (6.4 step 4).  Runtimes'
  callbacks were written for one finalizing thread, and this keeps them
  correct without an audit.  Allowing them to run concurrently is an open
  question.
- **Never on a mutator thread.**  Helpers are collector-owned threads
  (6.7).  No mutator ever runs `sweepRun`.  This rules out "sweep on the
  allocating core" through the mutators' refills, which the calibration
  report and the phase report mention as an option: it would move finalizers
  onto mutator threads and collection work onto the mutators.
- **Helpers never allocate.**  They do not call `getFreeCells` or
  `allocCell`.  Debug and instrumented builds assert this through a
  thread-local "is helper" flag checked in `getFreeCells`.

GarbageCollector.md § 7 gets one paragraph stating the above.  The sentence
"it runs on the single GC thread inside the sweep, so a wait there stalls
collection for the whole space" becomes "on the collector's threads".

### 6.6 Several spaces

- **One process-wide pool, not one per space.**  The cycle token
  (GarbageCollector.md § "Several spaces") already serialises cycles across
  the process, so at most one sweep inside a token runs at a time.  A pool
  per space would multiply idle threads by the number of spaces and would
  oversubscribe the machine when spaces collect back to back.
- **One job at a time.**  The pool runs a single job.  A collector that finds
  the pool busy runs its work alone (K = 0 for that phase) and never waits
  for the pool.  This can happen on the multi-space path, because the
  post-grace finalization of space A runs after A released the token, while
  B's sweep may hold the pool.  "Never wait for the pool" means no new wait
  edge exists between collectors, so no deadlock between spaces can come from
  the pool.
- **The `deferFree` path** (`ProtoSpace.cpp:915`, 1140-1175).  The sweepers
  push dead cells into their own vectors.  The grace period, the post-grace
  finalization and the publication follow today's order.  v1 runs the
  post-grace finalization serially on the collector thread.  It can use the
  pool when the pool is free, which is the same job shape over a vector
  instead of a list.
- **Grace periods.**  Helpers have no quiescence record
  (`multispace::ensureQuiescenceRecord` is never called on them), so
  `waitForGracePeriod` (`core/MultiSpace.cpp:383`) never waits for a helper.
  Helpers hold no cell of any space between jobs.  During a job they touch
  only candidate cells of the token holder's space, which no mutator can
  reach.

### 6.7 Threads

Helpers are `std::thread`s created and owned by protoCore.  This is the
precedent of the collector itself (`gcThread`, `ProtoSpace.cpp:1445`).

The "ProtoThreads, not native threads" rule binds runtimes: their threads run
user code and must take part in stop-the-world and grace periods.  It does
not fit collector threads, and a helper must **not** be a `ProtoThread`:
- a `ProtoThread` registers in `runningThreads` and in the quiescence
  records;
- a helper would then count in every stop-the-world quorum and every grace
  period, and the collector would wait for its own helpers.

### 6.8 Synchronisation

| Purpose | Primitive |
|---|---|
| idle sleep and job start | `std::mutex` + `std::condition_variable`; the predicate is `jobGeneration != seen || stopping`; `notify_all` after every change, made under the mutex |
| claims | the job's `std::mutex` (6.2) |
| completion | `activeSweepers` and `cursor`, changed under the pool mutex; the collector waits on a second `std::condition_variable` with predicate `cursor == nullptr && activeSweepers == 0`; a sweeper notifies when it decrements to 0 |
| publication | `ProtoSpace::globalMutex` for chunks (unchanged); CAS for the segment chains (unchanged algorithm, fewer operations) |

There is no spinning at idle, no timed wait, and no semaphore, latch or
barrier.  The happens-before edges are as follows:
- the mark bits written in Phase 4 are visible to the helpers through the job
  publication (mutex release, then acquire);
- the helpers' writes to cells and to per-sweeper state are visible to the
  collector through the completion handshake;
- the free chunks reach the mutators through `globalMutex`, as today.

### 6.9 Configuration

- `PROTOCORE_GC_SWEEP_THREADS=<K>`, read when the pool is first needed.
  - Proposed default: `min(3, hardware_concurrency / 4)`, which gives 3 on
    DEV12 (4 sweepers including the collector).
  - `0` gives today's behaviour, with the pool never created.
  - Invalid values are ignored, like the other `PROTOCORE_*` variables
    (README table).
- No API in v1.  A static `ProtoSpace::setCollectorHelperThreads(unsigned)`
  would be ABI-additive, and is an open question.
- The helpers take CPU from the mutators.  At N = 12 on 12 hardware threads
  that CPU is not free in general.  It is free exactly when mutators wait,
  which they do 17-63 % of the time at N = 12.  An option, as an open
  question: engage helpers only while a mutator waits for headroom, or while
  the backlog of unclaimed segments exceeds a threshold.

### 6.10 Lifecycle and teardown

- **Start:** lazily, at the first sweep that wants helpers.  Never in static
  initialisation, which on Windows runs under the loader lock in `DllMain`.
- **Stop:** when the live space count drops to zero.
  - `~ProtoSpace` of the last space stops the pool after its own
    `gcThread->join()`, inside the same `setQuiescenceOut(true)` bracket
    (`ProtoSpace.cpp:1503-1511`).
  - By then no collector exists, so no job is in flight.
  - The helpers wake on `stopping`, leave, and are joined.
  - A later space starts a new pool.
- **Never at static destruction.**  The pool object is allocated once and
  never destroyed by a static destructor.  A process that exits with a live
  space leaves parked helpers to the OS, as it does the collector thread
  today.  Destroying a joinable `std::thread` would call `std::terminate`.
- **Fork.**  Threads do not survive `fork`.  A `pthread_atfork` child handler
  marks the pool as having no threads.  Because the collector never waits for
  a helper to arrive (6.4), a pool that has lost its threads cannot deadlock
  a sweep.
- **The 2.10.0 teardown deadlock** (adaptive design § 6.10): a thread
  destroying a space joined its collector while that collector's grace
  period waited for the same thread.
  - The pool adds no edge of that kind.
  - Helpers never wait for mutators, grace periods, the token or the
    stop-the-world.
  - A collector waits for helpers only while they hold claimed work, which
    finishes without waiting on anything except `globalMutex` (held briefly,
    never across a wait).

### 6.11 Soundness

- The candidate set is fixed at Phase 2, as today.  Claims partition it, so
  each segment, and therefore each cell, is processed by exactly one
  sweeper.
- Mutators never touch candidate cells.  Dead cells are unreachable.
  Survivors' `next_and_flags` belong to the collector during sweep.  This is
  the argument of today's single-threaded sweep, unchanged; the helpers are
  the collector.
- Mark has finished before the job is published, so mark bits are read-only
  during sweep except for the owner's unmark.
- Phase 5b sees every finalized ref, because the merge precedes it.
- Phase 6 still precedes the token release.
- The grace period still precedes every finalization and reuse on the
  multi-space path.

### 6.12 What does not change

- Stop-the-world, roots, the mutable snapshot, mark, Phase 5b, the token, the
  grace period, the young-generation rules and the heap accounting.
- `getFreeCells` and the allocation fast path.
- Every public type and layout.
- With `PROTOCORE_GC_SWEEP_THREADS=0` the code path is the serial sweep, with
  F1-F4 applied.

## 7. Mark: not parallelised now

The data says mark is secondary:
- 1-50 % of busy time;
- every completed workload reaches its zero-wait bound with a faster sweep
  alone;
- mark throughput degrades much less under threads (11-22 M cells/s down to
  7-16 M).

Mark leads only near a full heap (`wordfreq` N = 12 and `graph` N = 1 at
10 M cells, and the runs that ended out of memory).

**Revisit when any of these holds:**
- with the parallel sweep in place, mark exceeds 40 % of busy time on a
  completed workload of the section 9 experiment;
- the collector is still more than 80 % busy at N = 12;
- the maintainer prioritises workloads whose live set is above half the
  limit;
- the maintainer keeps the 2.10.1 objective (minimise memory; section 13,
  question 1), which keeps the extra cycles and makes mark their dominant
  cost.

**What a parallel mark would need**, as a sketch only:
- per-thread work lists with stealing;
- `fetch_or` returning the previous bit, so that exactly one marker claims a
  cell;
- per-thread `markedList`s;
- the young-chain and pen walks split by chain.

No barrier and no change to the snapshot discipline: the graph traversed is
immutable.

## 8. How the parts combine, and what they can gain

| Situation | Part A (sizing) | Part B (throughput) |
|---|---|---|
| Regime 1, cycle starts at the ceiling (js40 N = 6, `wordfreq` N = 12, `core_fixed640`) | removes the wait: pacing with runway r x C, early wake | not needed |
| Regime 1 only because the heap is large enough | finds that heap (4.5) and keeps it within B | lowers the heap it needs (G* falls with rho) |
| Regime 2, total garbage fits in B (js40 `records` N = 12) | probes may grow S to B, removing the waits at the cost of memory | removes the waits at a smaller heap |
| Regime 2, garbage does not fit (`clj_coll_t6`) | memory only postpones the wait; growth stops (4.5) | the only remedy: raises T, lowers rho |

**Bounds** from the phase report's Amdahl table (synthetic workloads, one
run each).  They assume:
- the collector fully overlaps the mutators;
- the waits are spread evenly over the threads;
- the CPU a parallel sweep takes is free.

They are upper bounds, not predictions.

| Run | Wall s | Bound, sweep x2 | Bound, sweep x4 | Zero-wait floor |
|---|---:|---:|---:|---:|
| js40_records_n12 | 41.6 | 23.5 (-43 %) | 22.3 (-46 %) | 22.3 |
| js40_graph_n12 | 61.2 | 37.6 (-38 %) | 30.8 (-50 %) | 30.8 |
| js10_records_n12 | 20.1 | 11.2 (-44 %) | 8.9 (-56 %) | 8.9 |
| jsad_records_n12 | 46.3 | 23.0 (-50 %) | 17.1 (-63 %) | 17.1 |
| scala_tree_t6 | 89.6 | 49.3 (-45 %) | 49.3 (-45 %) | 49.3 |
| clj_coll_t6 | 129.2 | 61.6 (-52 %) | 43.4 (-66 %) | 43.4 |
| js40_*_n6 | 8.2-19.3 | (sizing case) | | -10 % to -22 % |

- **Whether the sweep scales is unproven.**
  - If the per-cell cost is latency (H1/H2 dependent misses), independent
    chains on K sweepers add memory-level parallelism, much as F4 does on one
    thread, well below the bandwidth limit.  At 12-17 M cells/s per sweeper
    the sweep moves under 1.1 GB/s of 64-byte lines.
  - If it is contention on shared lines (H3), more sweepers make it worse
    until F1/F5 remove it.
  - Section 5's gate exists for this reason.
- **The regime-1 rows** reach their floor through Part A alone, if pacing
  works as designed.  This is a hypothesis, checked by property P1.

## 9. Acceptance: properties, not benchmark targets

Acceptance is defined by properties of the policy and of the collector.  It
is checked with deterministic tests where possible (section 10) and with
controlled synthetic experiments otherwise.  The phase report's workloads are
synthetic; they are used as evidence of mechanisms and regimes, reported as
such, and **no threshold below is fitted to them**.

| # | Property | How it is checked |
|---|---|---|
| P1 | **No wait with spare capacity.**  When the collector has spare capacity (rho < 1 measured, u < 1) and the budget allows the headroom `L + G*`, mutators do not wait for headroom after convergence: wait share at the noise level, and no watchdog-ended waits. | Rate-controlled allocator test (10.2): a fixed allocation rate below a measured collector capacity, budget generous.  Then the memory-only experiment on the synthetic workloads classified as regime 1. |
| P2 | **The budget is never exceeded.**  The sum of the spaces' `heapSize` is at most B at every instant, with one space or several. | Assertion in instrumented builds at every heap growth; tests with one and two spaces. |
| P3 | **No out-of-memory while the live set fits.**  A live set with `L + one batch per thread <= B`, with any amount of garbage, completes. | Tests at 60 % and 90 % of B with heavy garbage. |
| P4 | **Out-of-memory when it does not fit.**  A live set above B fails with the callback and then the abort, with the documented message. | Existing OOM tests, unchanged. |
| P5 | **Bounded convergence.**  S changes at most `log2(B / S0)` times plus the re-arms, and never decreases. | Unit tests of the pure law over synthetic sequences of (L, r, T, w). |
| P6 | **Regime 2 does not spend memory for nothing.**  When doubling S twice does not lower the wait share, S stops growing. | Unit test of the law; rate-controlled test with the allocation rate above capacity and a garbage total larger than B. |
| P7 | **Single thread: no regression beyond noise.**  Wall time and peak RSS of single-threaded runs, median of 5, within the noise band measured on the same machine on the same day (the calibration report's "Noise" method). | Synthetic single-threaded runs: the phase report's N = 1 / t1 workloads, `core_fixed640`, CAD N = 1. |
| P8 | **The pause is unchanged.**  Stop-the-world per cycle stays bounded by threads and stack depth (at most 0.25 ms on the phase report's runs). | `[GC-PHASES]` `stw_max`. |
| P9 | **Parallel sweep equals serial sweep.**  K helpers free the same cells, release the same mutable entries and leave no mark bit, as K = 0 does.  K = 0 is today's code path. | Deterministic tests (10.1). |
| P10 | **The sweep does not get slower with helpers.**  On the differential microbenchmark (S0-S5), ns per cell with K helpers is at most that with K = 0, for every K up to the default. | Microbenchmark, median of 3.  The scaling curve over K = 0, 1, 2, 3, 5 is reported in full. |
| P11 | **Sanitizers and CI.**  The full ctest is green under TSan and ASan (sequential), and the Linux and cross-platform workflows are green, Windows included. | CI. |

**The memory-as-only-variable experiment.**  This is evidence, not a
criterion.
- **Setup.**  Run each synthetic workload (protoJS structures N = 1, 6, 12;
  protoScala `tree_alloc` t1/t6; protoClojure `coll_alloc` t1/t6; CAD 20k;
  `adaptive_heap_benchmark`) under fixed limits of 10, 20, 40, 100, 200 and
  400 M cells.  Collector, threads and binaries stay the same.
- **Classification:**
  - wait share falling to zero as the limit grows means regime 1 (or garbage
    that fits);
  - wait share flat while cycles lengthen means regime 2.
- **Then:**
  - run the same matrix with Part A (the law of 4.5) and with Part B (K = 3);
  - report, per workload, the regime, the wait share, wall time, peak RSS and
    cycles, labelled as synthetic;
  - the Part A run should land near the fixed-limit curve's knee for regime-1
    workloads (P1), and should stop growing for regime-2 workloads (P6).
- **Run rules.**
  - Each run in `systemd-run --user --scope -p MemoryMax=<cap>
    -p MemorySwapMax=0` under `/usr/bin/time`.
  - Sequential, with no concurrent build or test on the machine.
  - Median of 3.
  - Installed runtimes run against the development library through
    `LD_LIBRARY_PATH`, checked with `ldd`.
  - Every workload verifies itself (the `ok` lines and checksums of the phase
    report).  A run that does not verify is a failure, never a data point.
  - The 400 M-cell runs need a cap near 26 GB.  They run only with the
    maintainer's agreement on DEV12's memory.

## 10. Test plan

All tests are deterministic: they count, and never time.  Tests that need
threads assert invariants on counts, not on interleavings.

### 10.1 Parallel sweep (Part B)

1. **Claiming partitions the list.**  A unit test drives the claim function
   over synthetic `DirtySegment` lists of length 0, 1, R-1, R, R+1 and 10^5,
   with 1 and 8 concurrent claimers.  Every segment is claimed exactly once,
   and no claim returns an empty run while unclaimed segments remain.
2. **Same result as the serial sweep.**  A space with a root set holding a
   known structure builds a known amount of garbage.  One cycle with K = 0
   and one with K = 3 must give the same `reclaimedLastCycle`,
   `liveCellsLastCycle` and freelist count.  After the cycle no reachable
   cell carries a mark bit (the `GCMarkTests` pattern).
3. **Finalizer thread identity.**  `ProtoExternalPointer` finalizers record
   `std::this_thread::get_id()`.  The test records the ids of every mutator
   thread.  It asserts that no finalizer id is a mutator id, and that every
   external-pointer finalizer ran on the space's collector thread (the serial
   rule of 6.5).
4. **Embedder finalizers do not overlap.**  An in-flight counter inside the
   callback never exceeds 1, with K = 3 and 10^5 external pointers.
5. **Mutable refs are merged before Phase 5b.**  The
   `MutableRootReclaimTests` scenarios run with K = 3.  Dropped mutables'
   entries are released and their states freed one cycle later.
6. **Survivor pen.**  `GCSurvivorRechainTests` runs with K = 3, with
   `PROTOCORE_GC_SURVIVOR_STAGGER=1` and `=4`.
7. **Helpers never allocate.**  In debug builds, a helper calling
   `getFreeCells` aborts.  A death test confirms that the assertion fires.
8. **Teardown.**
   - Destroy a space while a K = 3 sweep is in progress (large garbage set,
     cycle requested, destructor called at once), 1,000 iterations.
   - Two spaces destroyed in both orders.
   - Destroying the last space stops the pool, and a new space restarts it.
   - A subprocess that exits with a live space exits with status 0, with no
     `std::terminate`.
9. **Several spaces.**
   - Two spaces collecting concurrently with K = 3: the pool's one-job rule
     holds, checked by a counter of concurrent jobs, at most 1.
   - The `deferFree` path frees the same cells as K = 0.
   - `MultiSpaceTeardown.DestroyingASpaceWhileItsCollectorWaitsForAGracePeriod`
     passes with K = 3.
10. **Fork.**  On POSIX only: a child forked after the pool started completes
    a cycle with K configured.
11. **Configuration.**  `PROTOCORE_GC_SWEEP_THREADS` values 0, 1, 64 and
    `abc`; invalid values fall back to the default.
12. **Stress.**  `GCStressTests` and `ConcurrentMarkSafetyTests` run with
    K = 3, under TSan and ASan.

### 10.2 Heap sizing (Part A)

1. **The law is a pure function.**  `adaptive::nextSoftLimit` (extended) and
   `pacing::runway` are tested over synthetic sequences of
   (L, r, T, w, S):
   - regime 1 reaches `L + G*` in one step and holds;
   - rho approaching 1 makes G* grow but never past B;
   - in regime 2, two non-improving probes stop growth;
   - a factor-2 change in L or r re-arms growth;
   - S never decreases and never exceeds B;
   - the number of changes stays within `log2(B / S0)` plus the re-arms (P5,
     P6);
   - the runway is 0 before the first cycle, monotone in r and C, and never
     above the headroom.
2. **Rate-controlled allocator** (new test helper).  A mutator allocates a
   fixed number of short-lived cells per step, with a safepoint per step and
   a step budget that sets the rate as a fraction of the collector's
   measured capacity.  This is the only way to impose a regime without
   depending on a benchmark.
   - At half capacity, with a generous B, the waits after the first two
     cycles end on the predicate or on a publication, never on the watchdog,
     and their count is zero once S has converged (P1).
   - Above capacity, with garbage larger than B, S stops growing within the
     bounded number of probes (P6) and the run completes.
   - The test asserts counts (wake reasons, waits, S changes), not durations.
3. **Budget.**  An instrumented-build assertion, `sum(heapSize) <= B`,
   checked at every growth with one and two spaces (P2).
4. **Out of memory.**  Live sets at 60 % and 90 % of B with heavy garbage
   complete (P3).  Existing OOM tests still abort with the same message (P4).
5. **Fixed limits.**  Under a fixed limit with a steady allocator, the second
   and later cycles start with `heapSize < maxHeapSize`, as shown by the
   `fixed` trace line.  `AllocationLimitTests` and `HeapHeadroomWaitTests`
   still pass.
6. **Early wake.**  A waiter blocked at the ceiling returns on a publication
   before the watchdog, asserted through a wake-reason counter.
7. **Controller regression set.**  `AdaptiveHeapTests` and
   `AdaptiveHeapControlTests` are updated to the new law.  Their expectations
   tied to `k_live`, `k_cap` and `p_high` are replaced, and each replacement
   is listed in the commit.

## 11. Implementation steps

Both parts are reviewed before any implementation.  After approval, the two
parts are independent and each step is a separate commit with its own
measurements.

**Part B.**
1. Instrumentation C1-C6 and the differential microbenchmark; diagnosis
   report (per-cell cost, `perf c2c` / stall attribution).
2. F1, F2, F3, F4 (and F5, F6, F7 only if the diagnosis calls for them).
3. Gate review with the maintainer.
4. The sweep refactored into `sweepRun` with per-sweeper state, K = 0 only.
   No behaviour change; P9.
5. The pool, claiming and the join; the finalizer sink and serial embedder
   finalizers; teardown and fork.  Tests 10.1.

**Part A.**
6. Signals r, C, T, w and u in the side state and in the trace.
   Measurement only.
7. Pacing (4.3) and early wake (4.4) for fixed limits and the controller.
8. The new soft-limit law (4.5); tests 10.2.

**Finally.**
9. The memory-as-only-variable experiment and the acceptance report.
10. Documentation: GarbageCollector.md (§ 7, Phase 5, Synchronisation,
    Several spaces, Adaptive heap controller), the controller design (a new
    section superseding § 7's law), the README variable table, and the
    CHANGELOG.

## 12. Risks

- **The sweep does not scale** (memory-bound or contention-bound).  The
  diagnosis gate addresses this.  F4 may deliver the latency part without
  threads.
- **The new sizing objective uses more memory.**
  - It does so by design, within B, and only where waits fall.
  - A host where B is set too high for its neighbours sees the process grow
    to it.  The remedy is the budget (operator or embedder), not the
    controller.
  - The release notes must say this plainly.
- **Signal noise.**  r and T are estimated from one or two cycles on a shared
  machine.  The "two non-improving probes" rule and the conservative inputs
  absorb single outliers, but a noisy probe can still stop growth early.
  The cost is the 2.10.1 behaviour (a smaller heap), not a failure.
- **CPU taken from mutators at saturation** by helpers.  The default K is
  small; engaging helpers only while mutators wait is an open question.
- **Finalizer concurrency.**  Built-in finalizers become concurrent with each
  other.  A future built-in finalizer must be thread-safe, and the contract
  says so.  Embedder callbacks stay serial.
- **Teardown and fork deadlocks.**  These are addressed by the "never wait
  for arrival or for the pool" rule and tests 10.1.  Run the teardown test
  1,000 times under TSan.
- **More cycles from early requests.**  The runway is `r x C`, so cycles are
  only as frequent as keeping up requires.  The quorum cost is measured
  (P8).
- **Windows.**  Pool creation must not happen under the loader lock (lazy
  start).  `pthread_atfork` is POSIX only.

## 13. Open questions for the maintainer

1. **Controller objective.**  Adopt "minimise time within the memory budget"
   (4.1), replacing 2.10.1's "minimise memory within a time tolerance", and
   with it drop `k_live`, `k_cap` and `p_high` for the measured law of 4.5?
2. **Regime 2 with garbage that would fit.**  The probes may grow S to B when
   waits keep falling.  Should the controller ever grow to B *speculatively*,
   betting that the run's garbage fits, when the probes say no?  The proposal
   is no: an embedder that knows its run fits can set S0 = B.
3. **Collector CPU as a cost.**  The objective counts mutator wait only.
   Should collector busy time (u) count too, when all cores are busy?
4. **Budget split between spaces.**  Is first come, first served (4.7)
   acceptable for v1?
5. **Finalizer contract.**  May built-in finalizers run concurrently with
   each other on helper threads (6.5)?  Should embedder
   `ProtoExternalPointer` callbacks stay serial on the collector thread
   (proposed), or become concurrent after an audit of the runtimes?
6. **Helpers.**  Should the default be `min(3, hw/4)` always available, or
   helpers engaged only while mutators wait or a backlog exists?  Is an API
   wanted, or the environment variable only?
7. **Pacing under fixed limits on by default?**  It changes when cycles
   start under `setHeapLimits` and `PROTOCORE_HEAP_LIMIT_CELLS`.
8. **Per-thread dirty-segment stacks** (6.2), if C2 shows contention on the
   single `dirtySegments` head at context destruction.
9. **Side mark bitmap** (5.3): out of scope unless H1/H5 dominate after F2
   and F3.  Agreed?
10. **Reproducibility.**  Commit the two new synthetic workloads
    (`tree_alloc.scala`, `coll_alloc.clj`) to their runtimes so that the
    experiment of section 9 can be repeated?
