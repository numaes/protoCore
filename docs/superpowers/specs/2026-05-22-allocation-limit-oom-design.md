# Allocation Limit & Reliable Out-of-Memory Detection — Design Spec

**Date:** 2026-05-22
**Component:** protoCore — Cell allocator (`ProtoSpace::getFreeCells`),
critical-section boundary (`ProtoContext`), and GC loop.

> **Implementation note.** This spec was revised after implementation. Two
> design points changed once the code met the GC's actual invariants and are
> documented here as-built: (1) critical-section allocations are *not* exempt
> from the ceiling — the limit is enforced at critical-section *boundaries*;
> (2) out-of-memory is detected from per-cycle *reclamation*, not from the
> mark-phase live count. The rationale for both is in the relevant sections.

## Problem

When a thread needs Cells and the freelist is empty, `ProtoSpace::getFreeCells`
allocates a fresh batch from the OS with `posix_memalign`. This is unbounded:
protoCore has no enforced ceiling on how much memory it takes. The field
`maxHeapSize` exists but is initialised to `0` and read nowhere. The only
failure handling is "`posix_memalign` returned non-zero" → `outOfMemoryCallback`
then process exit — i.e. protoCore only reacts once the OS itself is exhausted,
never against a configured budget.

The desired behaviour: a configurable ceiling; when a thread's allocation would
exceed it, the thread waits for the GC to reclaim unreferenced Cells and then
retries. Two hard sub-problems:

1. **Waiting without breaking stop-the-world (STW).** protoCore's GC is STW: a
   cycle proceeds only once `parkedThreads >= runningThreads`. A thread blocked
   on a condition variable while still counted in `runningThreads` — but unable
   to reach a safepoint — pins the quorum forever.

2. **Reliably detecting genuine OOM.** "A GC ran and freed nothing" is not, by
   itself, proof of OOM — a cycle may race the mutator. A trustworthy criterion
   is needed, and it must not be fooled by live Cells that never enter the
   mark-phase set (see Problem 2).

## Design

### Configuration — two watermarks

`ProtoSpace` gains two limits, both counted in **Cells** (`heapSize` units):

- `softHeapLimit` — above it, the allocator prefers reclamation over growth.
- `maxHeapSize` (existing field, now wired) — the hard ceiling; `heapSize`
  never crosses it for ordinary mutator OS allocations.

`0` means "no limit" for either. With both `0` (the default) behaviour is
**bit-identical to today** — the entire new path is gated on `maxHeapSize > 0`.
A new `ProtoSpace::setHeapLimits(int softCells, int hardCells)` sets them with
validation (`soft <= hard`, non-negative).

`heapSize` is the quantity capped: it counts Cells obtained from the OS and only
ever grows via `posix_memalign`. Reclamation does not shrink `heapSize`; it
moves Cells from "live" to "free" *within* the existing heap — which is exactly
why, at the ceiling, a thread can keep running on reclaimed Cells without the
heap growing.

### Problem 1 — GC-safe waiting (`reclaimWaitLocked`)

A thread that must wait for the GC **leaves the running set** before it sleeps,
so the STW quorum is computed without it:

```
reclaimWaitLocked(space, lock, ctx):        # `lock` holds globalMutex
    startCycle = gcCycleCount
    gcStarted = true ; gcCV.notify_all()        # ensure a cycle will run
    runningThreads.fetch_sub(1)                 # leave the running set
    gcCV.notify_all()                           # a GC parked on quorum re-checks
    memoryReclaimedCV.wait_for(lock, 50ms,      # releases globalMutex while asleep
        pred = gcCycleCount advanced past startCycle OR space ending)
    runningThreads.fetch_add(1)                 # rejoin the running set
    lock.unlock()                               # safepoint MUST run without globalMutex
    ctx->safepoint()                            # park now if an STW began while we slept
    lock.lock()                                 # re-acquire for the caller's re-check
```

Key invariants:

- While `runningThreads` is decremented, the waiter is excluded from the STW
  quorum — the GC and every other thread's STW proceed normally.
- The wait is on `globalMutex` via a `condition_variable_any`; the `wait_for`
  releases `globalMutex` for the duration of the sleep, so the GC can run.
- `globalMutex` is **recursive**, so `safepoint()`'s own lock would re-enter
  it; but `safepoint()`'s STW wait would then release only one recursion level
  and leave `globalMutex` held — wedging the GC. Therefore `reclaimWaitLocked`
  explicitly **releases `globalMutex` around `safepoint()`**.
- The wait is bounded (`wait_for` 50 ms): a missed `notify` costs latency, not a
  hang.

### Where the limit is enforced — critical-section boundaries

A critical section (`ProtoContext::CriticalSection`, `criticalSectionDepth > 0`)
guards a tree-builder that holds `Cell*` values in C++ locals across several
allocations and only attaches them to a GC root with a final atomic publish.
The cooperative STW poll in `allocCell()`/`safepoint()` deliberately **does not
park** while the depth is non-zero. That is not an accident of those two
functions — it is the mechanism that protects the builder: by staying in
`runningThreads`, the thread keeps the GC from ever reaching its STW quorum
while a builder is mid-flight, so the builder's un-anchored cells can never be
observed as garbage.

This rules out the obvious-looking design of waiting for the GC *inside*
`getFreeCells` when a critical section is open. `reclaimWaitLocked` removes the
thread from `runningThreads`; doing so mid-builder would let a STW cycle run and
sweep that builder's not-yet-anchored cells. Object construction
(`ProtoContext::newObject` and every mutable tree-builder) runs inside a
critical section, so this is the *common* allocation path — exempting it, as an
earlier draft proposed, would disable the limit entirely.

The resolution: enforce the ceiling at the **critical-section boundary**, where
`criticalSectionDepth` is still `0` and the thread holds no half-built tree and
is free to park.

- `ProtoContext::CriticalSection`'s constructor, when it is the *outermost*
  section (depth about to go `0 → 1`), calls `ProtoContext::heapLimitCheckpoint()`.
- `heapLimitCheckpoint()` is a cheap lock-free gate: if no hard limit is set, or
  `heapSize < maxHeapSize`, it returns after two relaxed loads — the only cost
  on the hot object-construction path. Otherwise it calls
  `ProtoSpace::waitForHeapHeadroom()`, which blocks (via `reclaimWaitLocked`)
  until the GC has freed room or OOM is confirmed.
- Nested critical sections (depth `> 0`) skip the checkpoint — a builder is
  mid-flight there.

`getFreeCells` keeps a depth-0 enforcement path too, for any context that
allocates without ever opening a critical section: when the heap is at the
ceiling and `criticalSectionDepth == 0`, it calls `waitForHeapHeadroom` and
retries.

### `getFreeCells` — clamping and bounded overshoot

`getFreeCells` (the OS-allocation path, reached when the freelist is empty)
enforces the ceiling arithmetically, with no waiting:

| Situation | Behaviour |
|-----------|-----------|
| `maxHeapSize == 0`, GC thread, or no context | Exempt — the historical unbounded allocator, bit-for-bit. |
| `headroom > 0` (`headroom = maxHeapSize - heapSize`) | Clamp the OS request to `headroom`; `heapSize` lands at or below `maxHeapSize`, never above. Soft zone (below) may additionally wait once. |
| `headroom <= 0`, `criticalSectionDepth == 0` | Call `waitForHeapHeadroom`, then retry the freelist. |
| `headroom <= 0`, `criticalSectionDepth > 0` | Cannot wait (would expose an in-flight builder). OS-allocate exactly one batch — a bounded overshoot. |

The only way `heapSize` exceeds `maxHeapSize` is the last row: an allocation
that drains the cell pool *inside* a critical section, when the heap is already
full. The critical-section entry checkpoint already blocked for the GC, so in
practice the pool is non-empty there; this row is reached only by a single
critical section whose own working set exceeds a batch. The overshoot is then
bounded by that one in-flight working set — and a working set that large cannot
be fragmented across a GC pause anyway. For all ordinary workloads
`heapSize <= maxHeapSize` holds exactly.

**Soft zone.** When `softHeapLimit > 0` and `softHeapLimit <= heapSize < maxHeapSize`,
a depth-0 caller does one bounded `reclaimWaitLocked` (once per `getFreeCells`
call) before OS-allocating, biasing steady state toward reclamation over
growth. The soft zone never hard-blocks.

### Problem 2 — reliable OOM detection (reclamation-based, 2-cycle)

The naive metric — the mark phase's live-set size (`markedList.size()`) — is
**wrong** for this purpose. The young generation of a *live* context is retained
but is *not* placed in `markedList`: those Cells are reachable (the context
holds them via `lastAllocatedCell`) yet they were never submitted to
`dirtySegments`, so the mark phase never enumerates them. A heap filled by a
long-running method's un-submitted young generation is genuinely out of memory,
yet a mark-based count would read near-zero and never escalate.

The authoritative metric is **reclamation**: how many Cells a completed GC cycle
swept back into the freelist.

- The GC sweep accumulates `reclaimedThisCycle` (sum of dead Cells across all
  published free chunks) and, at end of cycle, publishes it as
  `ProtoSpace::reclaimedLastCycle`.
- `gcCycleCount` (existing, one increment per cycle) is the cycle sequence.

`waitForHeapHeadroom` runs:

```
strikes = 0 ; oomCallbackUsed = false
lastSeenCycle = gcCycleCount
loop:
    if space ending: return
    if heapSize < maxHeapSize: return            # room to grow
    if freelist non-empty: return                # cells already available
    reclaimWaitLocked(lock, ctx)
    if freelist non-empty: return                # the cycle refilled it -> success

    cycle = gcCycleCount
    if cycle == lastSeenCycle: continue           # woke on timeout, no cycle yet
    lastSeenCycle = cycle

    if reclaimedLastCycle == 0:                   # a full cycle freed nothing
        strikes += 1
        if strikes >= 2:                          # confirmed across 2 cycles
            if not oomCallbackUsed and outOfMemoryCallback:
                outOfMemoryCallback(ctx)          # embedder frees its caches
                oomCallbackUsed = true ; strikes = 0
            else:
                escalate -> controlled abort
    else:
        strikes = 0                               # the GC freed cells: reclamation works
```

Why this is reliable rather than heuristic:

- The escalation path is reached only after `reclaimWaitLocked` returns *and the
  freelist is still empty* *and* `gcCycleCount` advanced — i.e. a full cycle
  genuinely completed and refilled nothing. A 50 ms timeout wake with no
  completed cycle does not count as a strike (the `cycle == lastSeenCycle`
  guard).
- A thread blocked here is not running the mutator, so it submits no new
  garbage. Residual garbage is drained by the first cycle or two; once it is
  gone, every further cycle reclaims exactly `0`. The **2-cycle** confirmation
  absorbs that initial draining cycle and any single-cycle timing race.
- If reclamation can free even one Cell, `strikes` resets — the loop only
  escalates when collection provably cannot help.

### Action on confirmed OOM

`outOfMemoryCallback(ctx)` is invoked first (the embedder releases caches); two
further reclaim cycles then get a chance to recover. If OOM still holds,
protoCore performs a **controlled abort**: a clear diagnostic to `stderr`
(`heap hard limit N cells reached; live set M cells, last cycle reclaimed 0 —
out of memory`) and `std::abort()`. `allocCell` keeps returning a valid Cell on
every other path — no `nullptr` propagation, no change to the thousands of
allocation call sites.

### The GC thread and contextless callers bypass the ceiling

Two callers are never blocked by the ceiling:

- The **GC thread** — blocking it deadlocks reclamation.
- A **contextless caller** (`ctx == nullptr`) — not a managed GC mutator; it has
  no critical-section state and cannot participate in the safepoint handshake.

Both OS-allocate unconditionally. The GC thread allocates its worklists on the
C++ heap, not via `getFreeCells`, so this is a guard, not a hot path.

### `getFreeCells` signature

`getFreeCells(const ProtoThread*)` → `getFreeCells(ProtoContext* ctx)`. The
`ProtoThread*` parameter was unused; the context is needed for the
critical-section depth check and for `reclaimWaitLocked` / `safepoint`. Both
call sites (`ProtoContext::implAllocCell`, `ProtoThreadImplementation::implAllocCell`)
have a context to pass.

`ProtoContext::implAllocCell` releases the per-context spinlock around the
`getFreeCells` call: the GC's Phase-2 root scan acquires that same spinlock to
read `lastAllocatedCell`, so holding it across a reclaim-wait would wedge the
collector.

## New / changed members

`ProtoSpace`:

- `int softHeapLimit` — new; soft watermark in Cells, `0` = unlimited.
- `int maxHeapSize` — existing; now the wired hard ceiling, `0` = unlimited.
- `std::atomic<unsigned long> reclaimedLastCycle` — new; Cells swept to the
  freelist by the last completed cycle. The authoritative OOM signal.
- `std::atomic<unsigned long> liveCellsLastCycle` — new; mark-phase reachable
  count, used only for the OOM abort's diagnostic message.
- `std::condition_variable_any memoryReclaimedCV` — new; end-of-cycle wake.
- `void setHeapLimits(int softCells, int hardCells)` — new; validated setter.
- `void waitForHeapHeadroom(ProtoContext*)` — new; the GC-safe blocking wait
  plus 2-cycle OOM escalation.
- `Cell* getFreeCells(ProtoContext*)` — signature change.

`ProtoContext`:

- `void heapLimitCheckpoint()` — new; lock-free gate run at every outermost
  critical-section entry; delegates to `waitForHeapHeadroom` when at the ceiling.
- `CriticalSection` constructor — now calls `heapLimitCheckpoint()` at the
  `0 → 1` depth transition.

## Testing

A `AllocationLimitTest` fixture (gtest), each test constructing a `ProtoSpace`
and calling `setHeapLimits` with small values:

1. **Reclamation under a hard limit** — a low ceiling, then a loop of
   short-lived contexts allocating far more *garbage* than the ceiling. All
   allocations succeed (the GC keeps reclaiming) and `heapSize` stays `<=`
   `maxHeapSize`.
2. **Soft watermark does not hard-block** — with `soft < hard`, a garbage
   workload still completes and stays within the hard ceiling.
3. **`setHeapLimits` validation** — a soft watermark above the hard ceiling is
   clamped; negatives become `0`.
4. **Genuine OOM is detected** (`AllocationLimitDeathTest`) — an
   `outOfMemoryCallback` that records (to `stderr`) it was called and frees
   nothing, plus a workload that retains every allocating context so its Cells
   can never be reclaimed. The heap fills, consecutive cycles reclaim `0`, the
   callback fires, and protoCore performs the controlled abort. An
   `ASSERT_DEATH` matches the callback's marker.

Regression: with no limits set, all pre-existing protoCore tests pass unchanged.

## Out of scope (follow-ups)

- Exposing `reclaimWaitLocked`'s enter/exit-running-set as a public protoCore
  primitive and having protoST's `GcSafeBlocking.h` forward to it.
- Per-context or per-thread memory accounting / limits.
- Mid-critical-section enforcement for a single builder whose working set alone
  exceeds the heap (currently a bounded one-batch overshoot per pool drain).
