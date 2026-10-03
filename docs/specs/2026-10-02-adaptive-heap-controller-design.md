# Adaptive heap controller

Status: **approved 2026-10-02**; implemented in protoCore 2.10.0 (section 6
records the decisions taken during implementation). Author: Gustavo Marino,
with Claude.

## 1. Problem

protoCore collects only when it has to: a cycle starts when the heap reaches a
configured limit. That is the right design. Persistent structures create
garbage on every modification (path copies), and deferring collection until
memory is needed keeps the mutators free of collection work. The difficulty
is choosing the limit:

- **Too tight** a limit gives a collection storm: cycles back to back, and
  mutators stalled waiting for headroom.
- **Too small** a limit gives a fatal out-of-memory although the program's
  data would fit the machine.
- **Too large** a limit lets a program with a small working set grow its RSS
  up to the limit before the first cycle. A 100k-object protoJS probe used
  598 MB under a 640 MB limit; its live set was 64–96 MB.

Each runtime picks a fixed limit today: protoST `defaultHardCells`, protoJS
640 MB, and others. None of them knows the program's working set.

## 2. Goal

The working set is discovered at run time, the same way for every runtime:

- the **soft limit** that triggers collections starts small;
- it **rises while collections are too frequent**;
- it **stops rising** once they become rare;
- its stable value is the working set plus the headroom the allocation rate
  needs.

The **hard limit** is only a safety cap. A program fails with out-of-memory
only when its live data, after a full cycle, does not fit under the hard
limit.

**Non-goal: returning memory to the operating system.** The heap never
shrinks; a transient that needed memory keeps it for the next one.
Therefore:

- the soft limit **never decreases**: lowering it would only add collections,
  because the memory is already the process's;
- the working set is the soft limit's high-water mark.

## 3. Design

### 3.1 Limits

- **Hard limit H**, in cells.
  - Default: 75 % of the smaller of physical memory and the process's memory
    limit (a cgroup v1/v2 limit on Linux, a job object on Windows), divided
    by the 64-byte cell.
  - Override: `PROTOCORE_HEAP_LIMIT_CELLS`, or the API (§3.5).
  - It is clamped to `INT_MAX` cells (128 GiB), because `heapSize` and the
    limits are `int` in the current ABI; widening them is an ABI change,
    deferred to the next SOVERSION.
- **Soft limit S**, in cells, adaptive.
  - Initial value `S0`: 32 MiB worth of cells (524,288), or `H` if smaller.
  - Invariant: `S0 ≤ S ≤ H`, and `S` is non-decreasing.

### 3.2 What is measured each cycle

At the end of each full cycle the collector records:

- **`L`**: live cells after the cycle (`liveCellsLastCycle`, already measured).
- **`T`**: wall time since the end of the previous cycle.
- **`P`**: time mutators spent unable to run because of collection in that
  interval. This is the sum of:
  - stop-the-world pauses;
  - time threads spent blocked in the soft-zone wait (`reclaimWaitLocked`)
    or the hard-zone wait (`waitForHeapHeadroom`).

  It is measured with `steady_clock` at those three places. Each costs one
  timestamp pair per wait, never on the allocation fast path.

**Pressure** is `p = P / T`, the fraction of the interval the program lost
to collection. It is the cost the program feels.

### 3.3 Control law (after each cycle)

```
floor   = ceil(k_live * L)               // k_live = 1.5: never below live + 50 %
if p > p_high:                           // p_high = 0.05: >5 % of time lost
    S = min(H, max(floor, ceil(S * g)))  // g = 1.5: grow multiplicatively
else:
    S = min(H, max(S, floor))            // only the floor can raise S
```

Collections are triggered as today. When `heapSize` reaches `S` a cycle is
requested, and allocation proceeds within the soft zone as currently
implemented (one cycle's wait, then growth), which is what produces `P` when
`S` is too tight. Because `S` never decreases, the controller converges after
at most `log_g(H / S0)` growth steps — about 20 for 64 GiB.

### 3.4 Out-of-memory

Out-of-memory is declared only when **both** hold:

1. a full cycle has just completed and `L > H − margin`, where margin is one
   refill batch per running thread: the live data cannot fit;
2. the pending allocation still cannot be satisfied after one more cycle.

This replaces the current heuristic ("two cycles reclaimed nothing"), which
can fire while the live set is far below the machine's capacity if the hard
limit was set too low. The embedder callback (`outOfMemoryCallback`) and the
controlled abort stay as they are.

### 3.5 API (additive, no ABI change)

```cpp
struct AdaptiveHeapConfig {
    proto_ulong hardCells = 0;           // 0: automatic (75 % of memory, see 3.1)
    proto_ulong initialSoftCells = 0;    // 0: 32 MiB worth
    double      highPressure = 0.05;     // p_high
    double      growthFactor = 1.5;      // g
    double      liveHeadroom = 1.5;      // k_live
};
void ProtoSpace::enableAdaptiveHeap(const AdaptiveHeapConfig& = {});

struct AdaptiveHeapStats {
    proto_ulong softCells, hardCells, liveCellsLastCycle, heapCells;
    uint64_t cycles;
    double lastPressure;
};
AdaptiveHeapStats ProtoSpace::adaptiveHeapStats() const;
```

- **State:** the controller's state lives outside `ProtoSpace`'s data layout,
  in a side structure owned by `ProtoSpace.cpp` and keyed by space, so the
  ABI is unchanged.
- **Compatibility:** `setHeapLimits(soft, hard)` keeps its meaning (fixed
  limits) and disables the controller. `PROTOCORE_HEAP_LIMIT_CELLS` sets `H`
  when the controller is enabled.
- **Tracing:** `PROTOCORE_HEAP_TRACE=1` prints one line per cycle (`L`, `T`,
  `P`, `p`, `S` before and after, `heapSize`).

### 3.6 Several spaces in one process

Each space has its own controller and its own `S`. `H` is a **process**
budget: a space's growth is refused when the sum of all spaces' `heapSize`
would exceed it. This is the process sizing rule already documented: the sum
of each space's peak working set. The out-of-memory condition (§3.4) uses the
sum of the spaces' live sets.

### 3.7 Runtimes

Every runtime replaces its own default limit with one call,
`space.enableAdaptiveHeap()`, at startup:
- protoST's `configureHeap`;
- protoJS's new 75 % helper;
- protoScala, protoClojure and protoPython, where they set limits.

An explicit `PROTOCORE_HEAP_LIMIT_CELLS` keeps working.

## 4. Tests

- **Controller unit tests (deterministic).** The control law is a pure
  function of `(S, H, L, p)`, tested with synthetic sequences:
  - a steady working set converges and stays;
  - a growing working set grows `S` in bounded steps;
  - a collection storm (high `p`) raises `S` until `p` falls;
  - `S` never decreases and never exceeds `H`;
  - the floor follows `L`.
- **Integration**, on a small real space:
  - **Steady workload:** RSS ends near `k_live · L` plus `S0`, not near `H`.
    The protoJS probe should drop from 598 MB to roughly 100–150 MB.
  - **Storm workload:** a high allocation rate on a large live set makes the
    trace show `S` rising and pressure falling below `p_high` within a bounded
    number of cycles.
  - **True OOM:** a retained live set larger than `H` fails with the
    out-of-memory callback.
  - **Not OOM:** a live set at 60 % of `H` with heavy garbage completes,
    whereas today's fixed small limits would abort it.
- **Multi-space:** two spaces under one `H` grow independently, and the
  process budget is enforced.
- **Sanitizers:** TSan and ASan jobs green, since the controller runs on the
  GC thread and reads mutator wait times.
- **Benchmarks:** the six usual benchmarks show no regression; RSS and cycle
  counts are reported before and after for protoJS's structure benchmarks.

## 5. Open points

1. **Pressure threshold and growth factor.** `p_high = 0.05` and `g = 1.5` are
   starting values. JVM ergonomics targets ~1–5 % GC time; Go's default lets
   the heap reach 2× the live set. They are to be calibrated on the runtimes'
   benchmarks.
2. **Concurrent collection time.** Concurrent mark and sweep run on the
   collector's own thread and are not counted in `P`: they cost CPU but do not
   stall mutators. On a machine whose cores are all busy that CPU is not free.
   A second signal, collector CPU time over interval, can be added if
   measurements show it matters.
3. **The 128 GiB ceiling** from `int` cell counters, until the next ABI
   version.

## 6. Implementation decisions (2.10.0)

The design above is implemented as written except where this section says
otherwise.  Each item gives the reason.

1. **Pacing replaces "a cycle is requested when `heapSize` reaches S".**
   Implemented literally, every cycle began when the freelist was already
   exhausted, so the mutators waited for the whole cycle.  The sweep is
   proportional to the garbage, and the garbage per cycle to S, so the stall
   grew with S, the pressure never fell (p = 0.2-0.4 on
   `adaptive_heap_benchmark`) and S ran away towards H (20 M cells for a
   100,000-cell live set).  A cycle is now requested at each refill when the
   cells left before S (global freelist plus the room between `heapSize` and
   S) fall below half of the headroom `S - L` the last cycle left: the
   mutators use the other half while the collector runs concurrently.  The
   same benchmark then settles at S = 2.6-4.0 M cells with p = 0.0003.  The
   request when a growth reaches S is kept.
2. **The soft zone, under the controller.**  The existing soft zone waited
   only for a refill at critical-section depth 0, and nearly every allocation
   happens at depth 1 (inside `newObject` and the other constructors), so it
   almost never waited.  Under the controller a refill that finds the heap at
   or above S and the freelist empty waits for the cycle requested or running
   when it can (depth 0, once per refill); otherwise it takes one batch and
   marks a wait pending, and the thread waits at its next outermost
   critical-section checkpoint.  With no cycle pending it grows by one OS
   block ("one cycle's wait, then growth").  The checkpoint pays one relaxed
   load of a process-wide counter, only for spaces with a hard limit.
3. **A wait ends when a cycle completes.**  `reclaimWaitLocked` waited for a
   change of `gcCycleCount`, which counts cycle *starts*; a wait that began
   inside a cycle lasted the 50 ms watchdog, because the request it made was
   cleared at that cycle's end.  The controller read that as pressure.
   Controller spaces now wait for a completed cycle; fixed limits keep the
   historical predicate.
4. **The stop-the-world pause in P runs from the quorum to the resume.**
   Measured from the raised flag it included the time the last threads took
   to reach a safepoint, during which they were running.
5. **P sums each thread's waits.**  With n threads waiting, P can exceed T
   and p can exceed 1.  Normalising by the thread count was not done: the
   design defines P as the sum, and a high p only makes S grow sooner.
6. **The out-of-memory measure is the cells a cycle left unreclaimed**
   (`heapSize - freeCellsCount` right after the sweep), not
   `liveCellsLastCycle`: young generations of live contexts and cells in
   threads' batches are never marked, yet cannot be reclaimed (the reason the
   fixed-limit rule used reclamation).  With several spaces, the other spaces
   count with their whole heap rather than their live set, because a space
   cannot use another's free cells: memory is never moved between spaces.
   The margin is the batch cap times the running threads,
   `threads x clamp(H / (8 x threads), 512, 65536)`.  Two consecutive strikes
   call the callback; two more abort, as with fixed limits.
7. **Configuration precedence.**  `PROTOCORE_HEAP_LIMIT_CELLS` takes
   precedence over `AdaptiveHeapConfig` (an operator's override beats an
   embedder's default); its soft part, when given, is S0.
   `PROTOCORE_ADAPTIVE_HEAP=0` makes `enableAdaptiveHeap` apply H as a fixed
   hard limit with no soft watermark.  Invalid `highPressure`,
   `growthFactor` or `liveHeadroom` values are replaced by the defaults.
   The controller is opt-in: a space without the call behaves as before.
8. **Limits detection.**  On Linux the cgroup v2 `memory.max` and the v1
   `memory.limit_in_bytes` of the process's cgroup and of every ancestor up
   to the mount point are read, and the smallest wins; walking up also finds
   the limit in a container whose cgroup namespace hides the host path.
   `memory.high` is not used (it throttles, it does not kill).  A v1 value of
   2^60 bytes or more means unlimited.  With nothing detected, H is 8 GiB
   worth of cells.  `AdaptiveHeapStats` also reports `enabled`.
9. **The process budget** is the sum of the live spaces' `heapSize`; a
   destroyed space returns its share (its memory stays with the process, as
   before).  It is enforced by setting each enabled space's `maxHeapSize` to
   its heap plus what is left of the budget whenever a heap grows, so every
   existing ceiling path applies unchanged.  With one space the cost is one
   addition per OS allocation.
10. **A pre-existing teardown deadlock** surfaced in the multi-space test: a
    thread destroying a space joined its collector while that collector's
    grace period waited for the same thread.  `~ProtoSpace` now marks the
    thread out of grace periods for the join (separate commit, test
    `MultiSpaceTeardown.DestroyingASpaceWhileItsCollectorWaitsForAGracePeriod`).
11. **Expectation corrected.**  Section 4 expected a steady workload to end
    near `k_live * L + S0`.  That holds when the allocation rate is modest.
    For a fast allocator S settles where the runway covers a cycle, about
    `L + 2 x allocation rate x cycle time`: on `adaptive_heap_benchmark`
    (about 30 M cells/s) S settles at 4.0 M cells for a 100,000-cell live
    set, 237 MB of RSS against 646 MB under the 640 MB fixed limit, and at
    20.2 M cells for a 1,000,000-cell live set built by path-copying appends
    (1.2 GB against 672 MB, 35 % faster), the build phase having lost
    13-58 % of its time to waits.  Calibration of `p_high` and `g` (open
    point 1) is where this trade is set.

