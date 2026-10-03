# Adaptive heap controller

Status: **draft for review** (2026-10-02). Author: Gustavo Marino, with Claude.

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
