# Allocation Limit & Reliable Out-of-Memory Detection — Design Spec

**Date:** 2026-05-22
**Component:** protoCore — Cell allocator (`ProtoSpace::getFreeCells`) + GC loop.

## Problem

When a thread needs Cells and the freelist is empty, `ProtoSpace::getFreeCells`
allocates a fresh batch from the OS with `posix_memalign`. This is unbounded:
protoCore has no enforced ceiling on how much memory it takes. The fields
`maxHeapSize` and `blockOnNoMemory` exist but are initialised to `0` and read
nowhere. The only failure handling is "`posix_memalign` returned non-zero" →
`outOfMemoryCallback` then `exit(-1)` — i.e. protoCore only reacts once the OS
itself is exhausted, never against a configured budget.

The desired behaviour: a configurable ceiling; when a thread's allocation would
exceed it, the thread waits for the GC to reclaim unreferenced Cells and then
retries. Two hard sub-problems:

1. **Waiting without breaking stop-the-world (STW).** protoCore's GC is STW: a
   cycle proceeds only once `parkedThreads >= runningThreads`. A thread blocked
   on a condition variable while still counted in `runningThreads` — but unable
   to reach a safepoint — pins the quorum forever. `getFreeCells` today
   side-steps this by *never* parking on the no-memory path (`// DO NOT park`),
   at the cost of the unbounded OS allocation.

2. **Reliably detecting genuine OOM.** "A GC ran and freed nothing" is not, by
   itself, proof of OOM — a cycle may race the mutator, or another thread may
   drop a large graph an instant later. A trustworthy criterion is needed.

## Design

### Configuration — two watermarks

`ProtoSpace` gains two limits, both counted in **Cells** (`heapSize` units):

- `softHeapLimit` — above it, the allocator prefers reclamation over growth.
- `maxHeapSize` (existing field, now wired) — the hard ceiling; `heapSize`
  never exceeds it for managed mutator allocations.

`0` means "no limit" for either. With both `0` (the default) behaviour is
**bit-identical to today** — the entire new path is gated on `maxHeapSize > 0`.
A new `ProtoSpace::setHeapLimits(int softCells, int hardCells)` sets them with
validation (`soft <= hard`, non-negative). The dead `blockOnNoMemory` field is
removed.

`heapSize` is the quantity capped: it counts Cells obtained from the OS and only
ever grows via `posix_memalign`. Reclamation does not shrink `heapSize`; it
moves Cells from "live" to "free" *within* the existing heap — which is exactly
why, at the ceiling, a thread can keep running on reclaimed Cells without the
heap growing.

### Three zones (evaluated when the freelist is empty)

Let `headroom = maxHeapSize - heapSize` (`+∞` when `maxHeapSize == 0`).

| Zone | Condition | Behaviour |
|------|-----------|-----------|
| Normal | `heapSize < softHeapLimit` | OS-allocate the full batch — unchanged. |
| Soft | `softHeapLimit <= heapSize`, `headroom > 0` | One bounded reclaim-wait; re-check the freelist; if still empty, OS-allocate a batch **clamped to `headroom`**. The heap grows only when reclamation genuinely cannot keep up. |
| Hard | `headroom <= 0` | No OS allocation. Reclaim-wait loop until the freelist is served or OOM is confirmed. |

### Problem 1 — GC-safe waiting (`reclaimWait`)

A thread that must wait for the GC **leaves the running set** before it sleeps,
so the STW quorum is computed without it:

```
reclaimWait(lock, ctx):                 # `lock` holds globalMutex
    startCycle = gcCycleCount
    gcStarted = true ; gcCV.notify_all()        # ensure a cycle will run
    runningThreads.fetch_sub(1)                 # leave the running set
    gcCV.notify_all()                           # a GC parked on quorum re-checks
    memoryReclaimedCV.wait_for(lock, 50ms,      # releases globalMutex while asleep
        pred = gcCycleCount advanced past startCycle OR space ending)
    runningThreads.fetch_add(1)                 # rejoin the running set
    lock.unlock()                               # safepoint MUST run without globalMutex
    ctx->safepoint()                            # park now if an STW began while we slept
    lock.lock()                                 # re-acquire for the freelist re-check
```

Key invariants:

- While `runningThreads` is decremented, the waiter is excluded from the STW
  quorum — the GC and every other thread's STW proceed normally.
- The wait is on `globalMutex` via a `condition_variable_any`; the `wait_for`
  releases `globalMutex` for the duration of the sleep, so the GC can run.
- `globalMutex` is **recursive**, so `safepoint()`'s own lock would re-enter
  it; but `safepoint()`'s STW wait would then release only one recursion level
  and leave `globalMutex` held — wedging the GC. Therefore `reclaimWait`
  explicitly **releases `globalMutex` around `safepoint()`**.
- The wait is bounded (`wait_for` 50 ms): a missed `notify` costs latency, not a
  hang.

This is the same "blocking safe region" pattern protoST built privately in
`GcSafeBlocking.h`; this spec puts the mechanism where it belongs — in the
allocator itself.

### Problem 2 — reliable OOM detection (authoritative, 2-cycle)

The GC's **mark phase is the reachability oracle**: after a STW mark, the set of
marked Cells *is* exactly the set reachable from the roots. The GC already
collects this set in `markedList`; `markedList.size()` is the authoritative live
count.

At the end of every cycle the GC publishes `liveCellsLastCycle` (=
`markedList.size()`) and notifies `memoryReclaimedCV`. `gcCycleCount` (existing,
incremented once per cycle) is the cycle sequence.

A thread in the **Hard** zone runs:

```
strikes = 0 ; oomCallbackUsed = false
loop:
    reclaimWait(lock, ctx)
    re-check the freelist  ->  if it can be served, return the Cells   # success
    live = liveCellsLastCycle
    if live + 1 > maxHeapSize:           # the reachable set alone fills the heap
        strikes += 1
        if strikes >= 2:                 # confirmed across 2 consecutive cycles
            if not oomCallbackUsed and outOfMemoryCallback:
                outOfMemoryCallback(ctx) # embedder frees its caches
                oomCallbackUsed = true ; strikes = 0   # give the freed memory a chance
            else:
                escalate -> controlled abort
    else:
        strikes = 0                      # live set fits: reclamation will catch up
```

Why this is reliable rather than heuristic:

- If `live < maxHeapSize`, then `maxHeapSize - live` Cells are non-live; after a
  full cycle they are reclaimed and published to the freelist, so the
  re-check **will** succeed. The loop cannot spin forever in this case.
- If `live >= maxHeapSize`, the reachable working set by itself meets or exceeds
  the budget — no amount of further collection can help. That is genuine OOM.
- The **2-cycle** confirmation absorbs the one imprecision: Cells allocated
  after a cycle's mark snapshot are not in that cycle's `liveCellsLastCycle`. A
  transient over-reading is corrected by the next cycle (whose snapshot is
  taken later); a real OOM reads `live >= maxHeapSize` every cycle.

### Action on confirmed OOM

`outOfMemoryCallback(ctx)` is invoked first (the embedder releases caches);
one further reclaim cycle then gets a chance to recover. If OOM still holds,
protoCore performs a **controlled abort**: a clear diagnostic to `stderr`
(`heap hard limit N cells reached; live set M — out of memory`) and a defined
exit. `allocCell` keeps returning a valid Cell on every other path — no
`nullptr` propagation, no change to the thousands of allocation call sites.

### Critical sections and the GC thread bypass the ceiling

Two callers cannot wait and must never be blocked by the ceiling:

- The **GC thread** — blocking it deadlocks reclamation. (It allocates its
  worklists on the C++ heap, not via `getFreeCells`, but the guard is kept
  explicit.)
- A thread inside a **critical section** (`ctx->criticalSectionDepth > 0`) — it
  holds a half-built tree in C++ locals; if it left the running set and a GC
  cycle ran, the collector would not see those Cells as roots and would free
  them. It also cannot park.

Both **bypass the limit and OS-allocate unconditionally**. The resulting
overshoot of `maxHeapSize` is bounded by the combined working set of in-flight
critical sections (a few cells per section — a sparse-list / mutable rebuild is
`O(log n)` Cells) — small and self-limiting. No separate reserve pool is
needed; the exemption *is* the reserve.

### `getFreeCells` signature

`getFreeCells(const ProtoThread*)` → `getFreeCells(ProtoContext* ctx)`. The
`ProtoThread*` parameter is currently unused; the context is needed for the
critical-section check and for `reclaimWait` / `safepoint`. Both call sites
(`ProtoContext::implAllocCell`, `ProtoThreadImplementation::implAllocCell`) have
a context to pass. `ctx == nullptr` (the rare contextless fallback) skips the
managed path — correct, since a thread with no context is not a GC mutator.

## New / changed `ProtoSpace` members

- `int softHeapLimit` — new; soft watermark in Cells, `0` = unlimited.
- `int maxHeapSize` — existing; now the wired hard ceiling, `0` = unlimited.
- `std::atomic<unsigned long> liveCellsLastCycle` — new; published per cycle.
- `std::condition_variable_any memoryReclaimedCV` — new; end-of-cycle wake.
- `int blockOnNoMemory` — **removed** (dead field).
- `void setHeapLimits(int softCells, int hardCells)` — new; validated setter.
- `Cell* getFreeCells(ProtoContext*)` — signature change.

## Testing

A new `AllocationLimitTest` fixture (gtest), each test constructing a
`ProtoSpace` and calling `setHeapLimits` with small values:

1. **Reclamation under a hard limit** — set a low ceiling, then allocate far
   more *garbage* than the ceiling in a loop of short-lived contexts. The
   allocations must all succeed (the GC keeps reclaiming) and `heapSize` must
   stay at or below `maxHeapSize`. Proves the reclaim-wait works and the heap
   does not grow past the hard limit.
2. **Soft watermark damps growth** — with `soft < hard`, a steady-state garbage
   workload keeps `heapSize` near `softHeapLimit` rather than running to
   `maxHeapSize`. Proves the soft zone prefers reclamation over growth.
3. **Genuine OOM is detected** — install an `outOfMemoryCallback` that records
   it was called, then build a genuinely *retained* graph (rooted, not garbage)
   larger than the hard ceiling. The callback must fire. The abort path is
   verified with an `EXPECT_DEATH`-style death test (or the callback returning
   control and the test asserting the callback ran, to avoid aborting the test
   binary — see the test file for the exact mechanism).
4. **Regression** — with no limits set (`0/0`), every existing protoCore test
   still passes unchanged; `getFreeCells` behaviour is bit-identical.

## Out of scope (follow-ups)

- Exposing `reclaimWait`'s enter/exit-running-set as a public protoCore
  primitive and having protoST's `GcSafeBlocking.h` forward to it. The mechanism
  is implemented here inside `getFreeCells`; promoting it to public API and
  migrating protoST is a separate change.
- Per-context or per-thread memory accounting / limits.
