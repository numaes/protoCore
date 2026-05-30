# Garbage Collector Implementation in protoCore

This document describes the design and implementation of the Garbage Collector (GC) in the `protoCore` project.

## Overview

The `protoCore` GC is a **concurrent Mark & Sweep collector** with a short cooperative **Stop-The-World (STW)** phase that captures the roots and a per-cycle **snapshot of the mutable-shard table**.  After the STW window closes, **mark, sweep, and bulk-unmark all run concurrent with user threads** — workers continue allocating, mutating, and progressing without any per-mutation write barrier.

The architectural reason this works without barriers is documented in
[`STW_ELIMINATION_RESEARCH.md`](./STW_ELIMINATION_RESEARCH.md) § 13 and in §
"Concurrent Mark Without Barriers" below: protoCore concentrates all
mutability in `MUTABLE_ROOT_SHARDS = 256` shards.  A 2 KB snapshot of those
shard roots is a complete snapshot of every mutable in the system, and the
marker traverses only that snapshot.  Workers may CAS-swap shards freely;
the marker never reads the live table during mark.

## Key Components

### 1. ProtoSpace
The `ProtoSpace` manages the global heap, the free list, the GC thread, the
mutable-shard table, and the per-cycle mutable-shard snapshot.  It
coordinates the Stop-The-World synchronization and the mark-and-sweep cycle.

### 2. ProtoContext
Each `ProtoContext` (representing a call stack frame) tracks objects allocated within its execution scope.  It maintains a linked list of "young" cells (`lastAllocatedCell`).  Young cells are pinned from collection until the owning context is destroyed (or until the context's allocator threshold submits the young chain), at which point they migrate to a `DirtySegment`.

### 3. DirtySegment
When a context is destroyed upon function return, its "young generation"
chain (`lastAllocatedCell`) is submitted to `ProtoSpace` as a
`DirtySegment`.  This ensures that objects remain safe and local to the
context while the method is executing, and only become candidates for
collection once the context is gone.  `space->dirtySegments` is a lock-free
LIFO stack.  Workers push segments; the GC drains the stack atomically at
the start of each cycle.

### 4. The mutable-shard table — protoCore's *concentrated mutability*
Every mutable in protoCore is identified by an integer `mutable_ref`.
Mutation is implemented by a CAS on `space->mutableRoot[mutable_ref %
MUTABLE_ROOT_SHARDS].root`, swapping the AVL `SparseList` that maps each
`mutable_ref` in that shard to its current immutable value.  See
`core/ProtoObject.cpp` `setAttribute` / `setAttributeIfEqual` (~11 sites)
for the runtime CAS discipline.

This design is **the** architectural pivot that allows concurrent mark
without write barriers.  See § "Concurrent Mark Without Barriers" below.

### 5. The per-cycle mutable-shard snapshot — `gcMutableSnapshot[]`
A plain-pointer array of `MUTABLE_ROOT_SHARDS` entries on `ProtoSpace`.
Captured atomically at STW Phase 2; consumed by the concurrent mark phase;
cleared at end of cycle.  Outside a cycle, every entry is `nullptr`.

Plain pointers (not atomics) because it is written and read **only by the
GC thread**: writes happen under STW (workers parked), reads happen during
mark (the only GC-thread-driven phase that traverses the heap).  No other
thread observes the snapshot.

## The GC Cycle

The GC runs in a dedicated background thread (`gcThreadLoop` in
`core/ProtoSpace.cpp`) and follows these phases:

### Phase 1 — Stop The World
- Sets the `stwFlag` to `true`.
- Waits for all application threads to reach a "parked" state (allocation
  safepoints, explicit `synchToGC` calls, or threads inside
  `UnmanagedScope`).
- Application threads check this flag in their allocation path and park
  on `stopTheWorldCV`.

### Phase 2 — Root collection + mutable-shard snapshot
While the world is stopped, the GC:

1. Scans every thread's context chain for roots (automatic locals, closure
   locals, return value, pending root, young-chain outgoing references).
2. Adds the per-process global roots (prototypes, root object, resolution
   chain, embedder-registered `ProtoRootSet` instances, the tuple interner,
   the string interner).
3. **Captures the mutable-shard snapshot** —
   `gcMutableSnapshot[s] = mutableRoot[s].root.load(acquire)` for each
   shard — and pushes each non-null shard root onto the worklist as a
   root.  This is the formal "snapshot at the beginning" that lets mark
   run concurrent.
4. Drains the lock-free `dirtySegments` stack into a local
   `segmentsToProcess` snapshot via atomic exchange.  Segments pushed by
   workers after this exchange are not in this cycle's snapshot and
   survive to the next cycle.

**Important.** The cells in each context's young chain are NOT promoted or
marked themselves during Phase 2 — only the objects they reference are
added to the worklist.  This "pins" the young objects while keeping them
unmarked, allowing them to be reclaimed in the first GC cycle after their
context is destroyed.

### Phase 3 — Resume The World (now *before* Mark)
- Once the snapshot and roots are safely captured, the `stwFlag` is
  cleared and `globalMutex` is released.
- Application threads are resumed.  They may allocate, mutate
  (CAS-swap shards), submit destroyed-context young chains — all
  concurrent with the mark and sweep that follow.

The STW window thus contains **only Phase 1 + Phase 2** — a fixed-cost
operation whose duration is bounded by the number of running threads and
the per-shard snapshot capture (256 atomic reads).  Independent of heap
size or live-object count.

### Phase 4 — Mark (concurrent with mutators)
- Performs a depth-first traversal starting from the roots collected in
  Phase 2.
- Uses the `processReferences` virtual method on each `Cell` to discover
  reachable cells.  Every `processReferences` implementation in the
  codebase traverses only `const`-qualified Cell fields, which are immutable
  after construction — so the marker walks a stable graph even though
  workers are running.
- Reachable cells are marked using a bit-flag in the
  `Cell::next_and_flags` atomic member (bit 0).  The mark bit is
  **GC-exclusive**: workers never call `mark()` / `unmark()` /
  `isMarked()`.
- The marker tracks visited cells in a contiguous `markedList` for
  bulk unmark in Phase 6 (replaces the older pre-mark unmark pass).
- **Snapshot discipline.**  Any GC code path that needs to dereference a
  `mutable_ref` to its current immutable value MUST consult
  `gcMutableSnapshot[shard]`, never `mutableRoot[shard].root.load()`.
  Today no `processReferences` implementation needs this — the marker
  reaches every mutable's current value transitively through the
  snapshot's per-shard root push in Phase 2 — but the snapshot table is
  the formal API for any future mark-time mutable-resolution path.

### Phase 5 — Sweep (concurrent)
- Iterates through `segmentsToProcess` (captured atomically in Phase 2).
- For each cell in a segment:
  - If it was **not** marked, it is finalized and chained into a free
    chunk; chunks are published to the global free pool in batches.
  - If it was marked, the mark bit is cleared.  The cell is either
    re-chained onto the next cycle's `dirtySegments` directly, or — when
    `PROTOCORE_GC_REINCLUDE_SURVIVORS` is on with `survivorStagger > 1`
    — pushed onto the survivor pen for delayed re-inclusion.
- New dirty segments pushed by workers after Phase 2's exchange are
  ignored this cycle and processed in the next.

### Phase 6 — Bulk unmark
- Walks the `markedList` from Phase 4 and clears the mark bit on every
  entry.
- This restores the tricolour invariant for the next cycle without
  paying the old pre-mark unmark pass's cost.
- Safe to run concurrent with mutators: `unmark()` is a single atomic
  `fetch_and(~0x1)`, and fresh cells allocated by workers have
  `mark=0` already.

### Phase 7 — Clear mutable-shard snapshot
- Zeros every entry of `gcMutableSnapshot[]`.
- The snapshot is per-cycle.  Outside the cycle, entries are `nullptr` so
  any stray read observes the absence of a snapshot instead of a stale
  shard root.

## Concurrent Mark Without Barriers

This is the architectural section that documents *why* protoCore can run
mark concurrent with mutators without the classical Dijkstra incremental-
update or SATB-by-mutation write barriers used by G1, Shenandoah, ZGC,
and similar collectors.

### The architectural insight
Most concurrent GCs assume **mutability dispersed across the heap**: every
object can hold mutable references, and any pointer slot can be CAS'd at
any time.  In that world, the marker must coordinate with every mutator
on every pointer write — hence write barriers.

protoCore takes the opposite path.  **All mutability is concentrated in
`MUTABLE_ROOT_SHARDS = 256` shards.**  Each Cell is immutable; the only
state that can change is `space->mutableRoot[s].root` — a `std::atomic<
ProtoSparseList*>` per shard, swapped by CAS in `setAttribute` and
`setAttributeIfEqual`.

A snapshot of those 256 shard roots is therefore **a complete snapshot of
all mutability in the system**.

### The mechanism
At STW Phase 2 (workers parked), the GC reads every shard atomically into
`gcMutableSnapshot[]`.  256 atomic loads, ~2 KB store, fits in 32 cache
lines.  The snapshot is taken once per cycle.

The mark phase then runs OUTSIDE the STW window, using only the snapshot.
Workers resume and may CAS-swap shards as they please — the marker
never reads the live `mutableRoot` table again during this cycle.

### Why this is sufficient
1. The graph reachable through the snapshot is fully **immutable**.  Every
   `ProtoObjectCell::processReferences` (and every other Cell's
   `processReferences`) traverses only `const`-qualified fields.  No
   worker can mutate the fields the marker reads.
2. The mark bit is GC-exclusive.  Workers never touch it.
3. Workers cannot free cells.  Sweep (GC-only) is the only path that
   frees, and sweep runs after mark.
4. `segmentsToProcess` is captured atomically under STW.  Post-STW
   submissions go to a fresh `dirtySegments` and survive to next cycle.
5. New cells allocated by workers post-STW live in their per-context
   young chain, never in this cycle's `segmentsToProcess`.  Sweep does
   not see them.
6. Mutable shard CAS by workers is invisible to the marker (snapshot
   discipline).
7. Per-thread `mutableValueCache` is worker-local; the GC doesn't touch
   it.

### Cost
- **STW pause:** O(threads + 256), independent of heap size or live-
  object count.  In absolute terms this is microseconds-to-low-
  milliseconds depending on how many threads must reach a safepoint.
- **Snapshot table:** `MUTABLE_ROOT_SHARDS * sizeof(ptr) = 2 KB`.  One
  allocation done at `ProtoSpace` construction; cleared between cycles,
  never freed.
- **Floating garbage:** objects that became unreachable post-STW but
  were reachable at STW survive this cycle (the snapshot still
  references them).  Reclaimed next cycle.  Bounded by one cycle.
- **Per-mutation cost:** ZERO.  No write barrier on `setAttribute`, no
  fence beyond the CAS itself.

### Comparison with classical SATB-by-barrier
G1, ZGC, and Shenandoah all use some flavor of snapshot-at-the-beginning,
but maintain the snapshot via a **write barrier** that fires on every
pointer store: the old pointer (or the overwritten field) is logged so the
marker eventually sees it.  Per-write cost; complex barrier code; entire
classes of subtle bugs (missed barriers, lost log entries) become
possible.

protoCore's approach is **SATB-by-snapshot** instead of SATB-by-barrier.
The snapshot is materialized once upfront in a tiny table; no per-write
coordination.  The mechanism is dramatically simpler at the cost of
floating garbage bounded by one cycle and a 2 KB cache-resident table.

The trade-off is favorable specifically because protoCore concentrates
mutability.  For a runtime where every object slot can be a mutable
reference, the snapshot would have to be O(num_mutables) and the
trade-off would invert.

### What about an extension that adds new mutable state?
The architectural contract: **any embedder that needs to add a new kind
of mutable state must route it through `mutableRoot[]`**.  Direct
mutation of a Cell field — outside the shard table — would break the
snapshot discipline and re-introduce the race the snapshot was designed
to close.

See [`STW_ELIMINATION_RESEARCH.md`](./STW_ELIMINATION_RESEARCH.md) § 11
(the "static root-discipline contract") for the dual rule that extension
authors must follow at the root side.

## Synchronization Mechanisms

- `globalMutex`: protects access to shared structures like `freeCells`,
  `dirtySegments`, the thread list, and the GC bookkeeping.  Held during
  STW only; released before Mark.
- `stwFlag` & `parkedThreads`: manage the cooperative Stop-The-World
  quorum.  Cleared as part of Phase 3, before Mark.
- `gcCV`: condition variable to trigger the GC thread and coordinate
  parked workers.
- `stopTheWorldCV`: condition variable workers wait on while parked.
- `memoryReclaimedCV`: condition variable workers wait on when blocked
  by `waitForHeapHeadroom` waiting for sweep to refill the freelist.

## Memory Allocation

- Threads request batches of cells from `ProtoSpace` (`getFreeCells`).
- If the free list is low, the GC is triggered.
- **Concurrent Allocation**: threads can continue to allocate memory from
  the OS (growing the heap) even if a GC cycle is currently running.
  This ensures that a high allocation rate does not stall the entire
  system.
- If no free cells are available and a GC cycle is not enough to satisfy
  the request, `ProtoSpace` allocates a new chunk of memory from the OS
  using `posix_memalign`.

### OS allocation cap (16 MiB)

Each request to the OS in `getFreeCells` is capped at **16 MiB** per call
(`kMaxBytesPerOSAllocation` in `core/ProtoSpace.cpp`).  The number of
blocks requested is computed from `blocksPerAllocation` and the current
policy (single-threaded vs multi-threaded batch size), then clamped so
that `blocksToAllocate * sizeof(BigCell)` does not exceed 16 MiB.  This
limits the size of a single `posix_memalign` call and avoids excessively
long critical sections when chaining cells under the global lock.

## Optimization Features

- **Inline Caching**: per-thread attribute caches
  (`ProtoThreadExtension::attributeCache`) speed up prototype chain
  traversals.
- **Per-thread mutable-value cache**: `MutableValueCacheEntry` short-
  circuits the "load `mutableRoot[shard]` + AVL `implGetAt(mutable_ref)`"
  path on the hot getAttribute path.  Cache invalidation is implicit:
  the cached entry is considered valid only while the cached
  `shard_root` pointer still equals the current shard root pointer; any
  CAS on the shard naturally invalidates stale entries on the next
  lookup.
- **Bit-Marking**: efficient marking using the low bit of aligned
  pointers in the cell chain.
- **Atomic References**: thread-safe `mutable_ref` generation using an
  atomic counter in `ProtoSpace`.
- **Prefetching in mark**: the mark loop prefetches the next cell pop
  to overlap cache-line misses with the current cell's work.

## How to use

The GC is mostly automatic.  However, threads must be "managed" by
`ProtoSpace` to participate in the STW protocol.  Use `ProtoThread` and
its synchronization methods to ensure proper GC behavior in custom
threading scenarios.

```cpp
// Explicit synchronization if needed
thread->synchToGC();

// Or trigger a cycle from any thread
space.triggerGC();

// Wrap a long blocking syscall so it does not stall the STW quorum
{
    proto::ProtoContext::UnmanagedScope u(ctx);
    // ... blocking I/O ...
}
```

## Future Research: Further bounding the STW pause

The current STW pause is already O(threads + 256), independent of heap
size.  The remaining cost is the per-thread root scan.  A separate
research note —
[STW_ELIMINATION_RESEARCH.md](./STW_ELIMINATION_RESEARCH.md) — explores
whether the residual root-scan pause can also be bounded by parallelizing
across worker threads or by deferring root collection to safepoint-
triggered checkpoints.  See § 13 of that note for the relationship
between snapshot-at-STW (implemented here) and the broader research
direction.

That note is marked **research only, not approved for implementation**.
The snapshot-at-STW step described here is implemented and tested; the
broader directions remain research.
