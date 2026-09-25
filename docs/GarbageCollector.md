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

Stop-the-world collects roots only: each thread's stack roots (automatic
locals, closure locals, return value, pending root, and one handle per
context for its young generation), the root of the mutables tree (the shard
snapshot) and the roots of the global structures.  Everything reachable
from those roots is immutable, so the depth traversal, the references of
young cells included, runs in the concurrent mark.  The pause therefore
depends on the number of threads and their stack depth, not on the size of
the heap, the live set or the number of young cells.  This is why protoCore
is soft real time.

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

### 6. The collector's allocation context — `gcContext`
`ProtoSpace::gcContext` is a `ProtoContext` built with
`ProtoContext::GCOwnedTag`.  It has no thread and no previous context, and
it allocates from its own freelist under its own spinlock, so the GC thread
never touches a mutator thread's freelist or critical-section counter.
Unlike every other context it does not register itself as
`ProtoSpace::mainContext` and belongs to no thread's context chain, so the
stop-the-world root scan never visits it.

Only the GC thread allocates through it, and only after sweep, when it
releases mutables-tree entries (Phase 5b).  No stop-the-world can start
while it holds a half-built structure, because the GC thread is the only
thread that starts one.  When the release ends the collector submits the
context's young generation, so every cell allocated through it is a
candidate of the next cycle.

### 7. Finalizer contract
`Cell::finalize` runs on the GC thread during sweep, concurrently with the
mutators.  A finalizer only **completes an action on an internal or
external structure**: free an external buffer, run an external pointer's
callback, record a number in collector bookkeeping.  It **never allocates
cells, never publishes to a shared structure with compare-and-swap, never
loops over protoCore data and never dereferences other `ProtoObject*`**.
The `ProtoExternalPointer` callbacks passed to
`ProtoContext::fromExternalPointer` are finalizers and follow the same
contract.  Work that needs allocation runs in a collector phase with its own
context instead (see Phase 5b).

A finalizer must also not **block**: it runs on the single GC thread inside the
sweep, so a wait there stalls collection for the whole space.  The finalizer is
the most protoCore can offer for memory it does not manage, and
[MemoryModel.md](MemoryModel.md) § 5 states that boundary — what an external
buffer or wrapped pointer costs, why protoCore cannot account it, and what the
embedder does instead.

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
   locals, return value, pending root) and records one root handle per
   context for its young generation: the head of its young chain
   (`lastAllocatedCell`), read under the context spinlock.  The chains are
   walked in Phase 4.
2. Adds the per-process global roots (prototypes, root object, resolution
   chain, embedder-registered `ProtoRootSet` instances).
3. **Captures the mutable-shard snapshot** —
   `gcMutableSnapshot[s] = mutableRoot[s].root.load(acquire)` for each
   shard — and pushes each non-null shard root onto the worklist as a
   root.  This is the formal "snapshot at the beginning" that lets mark
   run concurrent.
4. Records the **tuple interner snapshot**: `TupleInterner::captureForGC`
   stores each of its 64 shards' published entry count (O(shards)).
   Interned tuples are perennial and the table is a root, but the entries
   are pushed in Phase 4 mark, NOT here — they live in append-only chunks
   that never move, so mark walks exactly the captured entries while
   mutators keep interning.  A tuple interned after the snapshot is a
   young cell of its creating context and protected by it.
5. Drains the lock-free `dirtySegments` stack into a local
   `segmentsToProcess` snapshot via atomic exchange.  Segments pushed by
   workers after this exchange are not in this cycle's snapshot and
   survive to the next cycle.
6. With `PROTOCORE_GC_REINCLUDE_SURVIVORS`, captures the survivor pen in
   O(1).  On a fold cycle it takes the whole pen (`exchange`); its segments
   join this cycle's `segmentsToProcess` after the world resumes.  On other
   cycles (`survivorStagger > 1`) it records the pen head, and Phase 4 walks
   the references of the pen cells.

**Not scanned during Phase 2:**

- **`SymbolTable` (canonical interned strings, 64 shards).**  Every
  interned string is **perennial**: `SymbolTable::intern` allocates with
  a null `ProtoContext`, so symbol Cells live via `posix_memalign`
  directly — never in a freelist, never in a context young chain.  The
  GC's mark/sweep machinery never sees a symbol Cell as a candidate,
  there is nothing to protect, and iterating every shard on every cycle
  would be pure overhead.  See `SymbolTable` in `headers/proto_internal.h`.
- **`stringInternMap` (legacy, dead).**  Existed for content-keyed string
  dedup; abandoned because `computeContentHash` walks the entire rope
  O(N), making `s += 'x'` loops O(N²).  `internString()` is no longer
  called from any path (verified by grep).  The field stays for ABI
  stability but is held empty and never iterated by the GC.

**Important.** The cells of a context's young chain are not candidates of
the cycle and are never marked for it.  Phase 4 walks each chain captured
in Phase 2 and adds only the objects its cells reference to the worklist.
This pins the young objects while keeping them unmarked, so they can be
reclaimed in the first cycle after their context is destroyed.  The walk
runs while mutators allocate, over a stable view: a chain grows only by
prepending, so no link behind the captured head changes; submitting a
chain hands its head to a segment of the next cycle without touching
links; and this cycle's sweep, which runs after mark, touches only the
segments captured in Phase 2.

### Phase 3 — Resume The World (now *before* Mark)
- Once the snapshot and roots are safely captured, the `stwFlag` is
  cleared and `globalMutex` is released.
- Application threads are resumed.  They may allocate, mutate
  (CAS-swap shards), submit destroyed-context young chains — all
  concurrent with the mark and sweep that follow.

The STW window thus contains **only Phase 1 + Phase 2** — root collection,
whose duration is bounded by the number of running threads, their stack
depth, the embedder root-set pins and the per-shard snapshot capture (256
atomic reads).  Independent of heap size, live-object count and the number
of young cells.

### Phase 4 — Mark (concurrent with mutators)
- Walks the young chains captured in Phase 2 and, on non-fold cycles, the
  survivor pen, pushing the references of their cells; on a fold cycle it
  links the captured pen in front of `segmentsToProcess`.
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
- Finalizers follow the finalizer contract (§ 7 above).  The only finalizer
  with collector work is `ProtoObjectCell::finalize`: for a mutable
  object's handle (`mutable_ref > 0`) it appends the `mutable_ref` to
  `ProtoSpace::gcFinalizedMutableRefs`, a GC-thread-only
  `std::vector<unsigned long>` of the same kind as `workList` and
  `markedList`.  It does not allocate and does not touch `mutableRoot`.

### Phase 5b — Release of mutables-tree entries (concurrent)
A mutable object keeps its current state in `mutableRoot`, keyed by its
`mutable_ref` (§ 4).  The handle cell does not reference its state, and the
entry keeps the state, and everything the state references, reachable.  So
the entry must be removed once the handle is garbage.  This phase is where
that happens.

- The GC thread sorts the refs recorded during sweep by shard.
- For each shard it loads the live root, removes each recorded ref that is
  present (`sparseListGetRaw` probe, then `removeAt`; absent refs are
  skipped), and publishes the result with **one** compare-and-swap.  If a
  mutator published to the shard meanwhile, the CAS fails; the GC reloads
  the root and redoes that shard's batch.  At most 256 publications per
  cycle, however many mutables died.
- The path copies are allocated through `gcContext` (§ 6), whose young
  generation is then submitted.
- The list is cleared; its capacity is kept for the next cycle.
- Instrumented builds report the time of this phase as `REL=` in the
  `PROTOCORE_GC_PROFILE` line (cumulative, like `P5` and `P6`).

**When memory is returned.**  A handle that is unreachable at the
stop-the-world of cycle N is finalized in cycle N's sweep, and its entry is
released at the end of cycle N.  The state and the objects only it
references were reached in cycle N through the mutable snapshot, so they
survive cycle N and are freed in cycle N+1 (with
`PROTOCORE_GC_REINCLUDE_SURVIVORS`; with `survivorStagger > 1`, on the next
fold).

**Per-thread caches.**  The per-thread caches are not GC roots, and each
thread clears them when it resumes after a stop-the-world (see "Concurrent
Mark Without Barriers", point 7).  An entry that names a released state or an
older shard root therefore keeps nothing alive: the state is freed in the next
cycle regardless of what the caches held.

**Without survivor re-inclusion.**  With
`PROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` a cell that survives one cycle is
never a candidate again.  A mutable handle that survives a cycle is
therefore never finalized and its entry is never released; the release
covers only mutables that become unreachable before their first cycle, and
their states, which survive that cycle through the snapshot, are not freed
either.  This is the expected behaviour of that configuration, the same as
for every other cell.

**Why the release is sound.**
- *Nothing live can reach a finalized handle's `mutable_ref`.*  Finalize
  runs only on unmarked candidates.  A mutator can only obtain cells that
  were reachable at stop-the-world (marked) or allocated after it (young,
  not candidates), so an unmarked handle is never used again.  Exactly one
  cell carries a given ref: `clone(true)`, `newChild(true)` and
  `newObject(true)` take a fresh value from `nextMutableRef`, an atomic
  counter that is never decremented or reset, and every state cell is built
  with `mutable_ref = 0`, so only the handle records its ref.
- *The snapshot.*  `gcMutableSnapshot[]` still holds the entry, and mark
  already traced the state through it; the release changes only the live
  table, after mark.
- *Concurrent writers.*  Every writer derives its new root from the root it
  passes as the CAS's expected value, and so does the release.  Neither can
  publish a root that was not derived from the currently published one, so
  no update is lost on either side.  The removal changes the shard root,
  which invalidates the per-thread cache entries for that shard.
- *No ABA.*  Only the GC thread frees cells, and a root loaded after the
  stop-the-world is marked or young, never a candidate of this cycle, so no
  root can be freed and its address reused while a CAS is pending.

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
7. The per-thread `attributeCache` and `mutableValueCache` are not GC
   roots, and the marker never reads them
   (`ProtoThreadExtension::processReferences` reports nothing from them),
   so their owning threads may rewrite entries at any time without racing
   the mark.  Instead, **each thread clears both of its caches when it
   resumes after a stop-the-world**, before it can look anything up again:
   on leaving the allocation poll's park, `safepoint()`, `synchToGC()`, a
   heap-headroom wait, and on returning from an unmanaged region
   (`ProtoThreadExtension::clearCachesAfterStopTheWorld`, which clears only
   when `gcCycleCount` changed since the thread's last clear).  A thread
   inside a critical section delays the stop-the-world, so it cannot miss a
   clear.  Every entry a lookup can see was therefore written after the last
   stop-the-world: the cells it names were marked or young then, and none is
   freed or reused before the next stop-the-world clears the entry.  The
   lookup path itself does no extra work.

### Cost
- **STW pause:** O(threads × stack depth + root-set pins + 256),
  independent of heap size, live-object count and young cells.  In absolute terms this is microseconds-to-low-
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

There is exactly one sanctioned alternative, and it is not an exemption
from the discipline but a different way of earning the same property: see
"Lock-free queues without barriers" below.

### Lock-free queues without barriers

`ProtoMPSCQueue` (protoCore 2.1.0, `core/ProtoMPSCQueue.cpp`, spec
`protoScala/docs/platform/PMQ-SPEC.md`) is the first protoCore type whose
cell holds state that changes after publication without going through
`mutableRoot[]`.  It is allowed to, because it does not ask the collector
to trust a lazily-read mutable word: it makes that read provably a
**superset** of what the pause saw.

**Why a lazily-read mutable word is normally unsound here.**  Suppose the
queue cell simply reported `head.load()` from `processReferences`.  The
mark loop runs *after* the world resumes (Phase 3 precedes Phase 4).  A
`takeAll` that runs before the marker reaches the queue cell moves the
items into a fresh `ProtoList` `L`.  `L` was allocated after the pause, so
it sits in front of the young-chain head captured in Phase 2 and **its
references are not walked in this cycle**.  `L` itself is not a candidate
and survives — but the items are older cells inside the segments captured
at the pause, now unmarked and unreachable from any root the marker will
read.  Sweep frees them under a live `ProtoList`.

**The two orderings that close it.**

1. `takeAll` publishes a *retain cell* carrying the chain onto the queue's
   `retained` stack **before** it detaches that chain from `head`.
2. `processReferences` loads `head` **before** it loads `retained`.

Let `S` be the instant the world resumed and `H_S` the chain at `head` at
`S`.  Producers only ever CAS-*prepend*, so the chain is fully linked at
every instant and every node of `H_S` stays in `chain(h)` for the `h` any
later `takeAll` loads.  A node therefore either is still under `head` when
the marker reads it, or was removed by a `takeAll` that had already put it
under `retained` — and `retained` only grows during a cycle.  The union of
the two reads, taken in that order, covers `H_S`.  Swap either ordering
and an item pushed before the pause and consumed during the mark is freed
under a live `ProtoList`.

Nodes pushed *after* `S` need no protection at all: they are young cells of
the pushing context, and a young cell is not a candidate of the running
cycle.

**What this costs the collector: nothing.**  No write barrier, no card
marking, no new phase, no registry, no capture.  The only addition to the
stop-the-world window is one `addRootObj` for the queue prototype — O(1),
a global-structure root, the same addition `ProtoMap` made.  The queue's
`push` is one cell and one CAS; `takeAll` adds one retain cell per *batch*
and one relaxed read of `gcCycleCount`.

**Reclamation of the retain chain.**  The single consumer releases
`retained` at its first `takeAll` in a new GC cycle, inside a
`CriticalSection` window that contains no allocation and no safepoint —
so the cycle number it read cannot go stale before the publish.
`gcCycleCount` is bumped under the pause at the *start* of a cycle and the
collector is one sequential thread, so observing `C` proves the mark and
sweep of cycle `C - 1` finished.  The price is that a consumed chain's
nodes stay reachable for at most one extra cycle.

This release is deliberately **not** done by `processReferences`: the
Phase 4 young-chain walk calls `processReferences` as well, so a
destructive read there would be a second, silent consumer — and any future
heap dumper or diagnostic that called it would drop untraced chains.

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

- The heap never shrinks: reclamation moves cells from live to free *within*
  the heap, and no memory is returned to the OS.  Resident size therefore
  converges to the process's high-water mark, which is what makes it
  computable; [MemoryModel.md](MemoryModel.md) states the sizing rule and its
  terms.
- Threads request batches of cells from `ProtoSpace` (`getFreeCells`).
- Allocation alone does not start a collection unless a heap limit is
  configured with `ProtoSpace::setHeapLimits`.  Without a limit (the
  default), a cycle starts when `ProtoSpace::triggerGC()` is called and
  fewer than 20% of the heap's cells are free; the comment on the GC
  trigger sources in `core/ProtoSpace.cpp` lists every path.
- **Concurrent Allocation**: threads can continue to allocate memory from
  the OS (growing the heap) even if a GC cycle is currently running.
  This ensures that a high allocation rate does not stall the entire
  system.
- If no free cells are available, `ProtoSpace` allocates a new chunk of
  memory from the OS using `posix_memalign`, within the heap limit when
  one is set.
- **Waiting for heap headroom parks without submitting.**  With a hard
  limit, a thread that reaches the ceiling with an empty freelist waits in
  `ProtoSpace::waitForHeapHeadroom`, called from the heap checkpoint at the
  entry of an outermost critical section.  The wait requests a cycle,
  leaves the stop-the-world running set, and when the cycle has finished
  rejoins it and parks if another stop-the-world is pending.  That park
  uses the thread's park-only entry (`ProtoThread::synchToGC`; a context
  without a thread parks the same way).  It never runs
  `ProtoContext::safepoint()`, whose per-context threshold hands the
  context's young generation to the collector.  The wait happens inside
  native code, where the caller may hold a half-built structure only in
  C++ locals and in that young chain; submitting the chain there would
  make those cells candidates while nothing references them, and a later
  cycle would free them.  Young generations are submitted only when a
  context is destroyed or at a `safepoint()` the embedder calls.
- **Refill batches adapt to a heap limit.**  A thread allocates from a
  private freelist that `getFreeCells` refills in batches.  Those cells
  count against the heap limit as soon as they are handed out, but no cycle
  can reclaim cells a thread holds.  Without a limit a batch is
  `blocksPerAllocation` (8,192) cells, or 60,000–65,536 with several running
  threads, and a recycled chunk hands out up to 8,192 cells.  With a hard
  limit, each refill is capped at
  `maxHeapSize / (8 × runningThreads)` cells, never below 512: all running
  threads' batches together use at most one eighth of the limit.  A free
  chunk larger than the cap is split, and an OS request keeps its usual size
  with the surplus published as free chunks.  Without a limit the sizes and
  the code path are unchanged.  A smaller batch means more refills, each
  taking `globalMutex`; the constants are `kLimitBatchFraction` and
  `kMinLimitedBatchCells` in `core/ProtoSpace.cpp`.

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
  lookup.  Neither per-thread cache is a GC root: each thread clears
  both of its caches when it resumes after a stop-the-world (point 7 of
  "Concurrent Mark Without Barriers"), which is what makes the
  pointer comparison safe against a freed and reused address.
- **Bit-Marking**: efficient marking using the low bit of aligned
  pointers in the cell chain.
- **Atomic References**: thread-safe `mutable_ref` generation using an
  atomic counter in `ProtoSpace`.
- **Prefetching in mark**: the mark loop prefetches the next cell pop
  to overlap cache-line misses with the current cell's work.

## How to use

Collection runs on the GC thread; embedders request cycles with
`triggerGC()` or configure a heap limit (see "Memory Allocation"
above).  **Without a heap limit, no cycle starts by itself.**  A limit can
be set in code with `ProtoSpace::setHeapLimits(soft, hard)`, or without
changing code through the environment variable
`PROTOCORE_HEAP_LIMIT_CELLS=<hard>` or `<soft>,<hard>` (cells), read when
the `ProtoSpace` is constructed; for example
`PROTOCORE_HEAP_LIMIT_CELLS=500000` runs an embedder's tests under about
30 MiB of cells.  The README's "Runtime Configuration" table lists every
environment variable protoCore reads.  Threads must be "managed" by
`ProtoSpace` to participate in the STW protocol.  Use `ProtoThread` and
its synchronization methods to ensure proper GC behavior in custom
threading scenarios.

```cpp
// Explicit synchronization if needed
thread->synchToGC();

// Request a cycle: one starts when fewer than 20% of heap cells are free
space.triggerGC();

// Wrap a long blocking syscall so it does not stall the STW quorum
{
    proto::ProtoContext::UnmanagedScope u(ctx);
    // ... blocking I/O ...
}
```

## STW Pause Anatomy and Latency Profile

The combined effect of (a) concurrent mark via per-cycle mutable-shard
snapshot and (b) Phase 2 trim (tuple interner walk moved to mark,
stringInternMap not scanned) is that **no component of the STW window
has a cost that grows with the live heap**.  This section breaks down
exactly what STW does — and what it costs — so the runtime's latency
behaviour can be reasoned about quantitatively.

### Per-component cost breakdown

| Component | Typical cost | Worst-case driver | Bound |
|---|---|---|---|
| Thread quorum wait (slowest mutator to reach a safepoint) | 10–100 μs | safepoint distance in the embedded interpreter | mitigable by instrumenting the interpreter loop |
| Per-thread context-chain scan (automatic locals + closure locals + one young-chain head per context) | 10–50 μs/thread | call depth × locals per context | bounded by stack depth and typical local count |
| Global roots (~30 prototypes + literalData symbols) | < 1 μs | constant | O(1) |
| **`mutableRoot[256]` snapshot** | **< 1 μs** | constant | **O(256) atomic loads, 32 cache lines** |
| Embedder root sets | < 50 μs typical | number of pinned objects | O(num\_pins) |
| **Tuple interner** | **< 1 μs** | constant | **O(64) published-count reads; entries walked in mark, not STW** |
| **`SymbolTable`** (canonical interned strings) | **0** | n/a | **perennial — never scanned** |
| **`stringInternMap`** (legacy, dead) | **0** | n/a | **not iterated; field retained for ABI** |
| `dirtySegments.exchange()` | < 1 μs | constant | O(1) atomic |

**Estimated total for a typical workload** (e.g. protoPython running
pyperformance, protoST with a moderate actor count, protoJS
interactive): **30–250 μs**, from the per-component estimates above.
No measured pause distribution is recorded yet; see "Real-time
positioning" below.

One point measurement (September 2026, instrumented build,
`PROTOCORE_GC_PROFILE=1`): a thread one context below the root context,
holding 20,000 or 20,000,000 young cells, paused 10–98 μs per cycle
(P1 + P2) in both cases.  While Phase 2 still walked the young chains,
the same probe paused 244–508 μs and 299–373 ms.

The single architectural property that delivers this: **every term in
the table is either constant or scales with thread/stack quantities
that the application controls**, not with the size of the live heap or
the rate of mutation.  This is what "pause time decoupled from heap
size" means concretely.

### Comparison with other production GCs

The four columns map the **whole trade-off**, not just the pause.
Every system in the table pays a non-trivial cost somewhere — at the
pause, at every memory access, or in what the runtime is allowed to
look like.  Reading the rows left-to-right gives the visible win
(typical pause); reading them right-to-left gives what the system
gave up to get it.

| GC | Typical pause | Decoupling mechanism | Per-access cost | Architectural constraint |
|---|---|---|---|---|
| **protoCore (post-snapshot)** | **30–250 μs (estimate)** | **snapshot of 256 shard roots; immutable Cells** | **zero** | **all mutability routed through `MUTABLE_ROOT_SHARDS` shards (no per-Cell mutable slots; Cells are `const`-only after construction)** |
| ZGC (Java) | 100 μs – 1 ms | concurrent mark + relocation + load barriers + multi-mapping (colored pointers) | load barrier on every reference read | 64-bit multi-mapped virtual address space; JVM-specific; difficult to embed |
| Shenandoah (Java) | 100 μs – 1 ms | concurrent mark + Brooks pointers + concurrent compaction | indirection + write barrier on every reference | +1 forwarding word per object (Brooks pointer); every dereference pays one indirection |
| Go | 100 μs – 1 ms | tricolor concurrent mark + hybrid (Yuasa + Dijkstra) write barrier | hybrid write barrier on every pointer store | compiler-emitted barriers; runtime not separable from Go semantics; not embeddable as a library |
| G1 (Java) | 10–100 ms | regional, evacuation-pause | SATB write barrier on every pointer store | regional layout; objects > ½ region bypass the collector ("humongous"), creating permanent old-gen pressure |
| CMS (Java, deprecated) | 10s of ms | concurrent mark + remark STW | SATB write barrier on every pointer store | heap fragmentation (no compaction); deprecated upstream |
| V8 / SpiderMonkey | 1–10 ms | generational + incremental + concurrent | write barrier on every pointer store | tightly coupled to JS object model + tiered compiler; not a general-purpose GC |

Reading the table:

- **Rows 5-7 (G1, CMS, V8)** — classic SATB + write barrier. Mature,
  well-understood, widely deployed.  Pause does not scale below the
  ~ms range because mark runs partly under STW (G1, CMS) or because
  fragmentation forces evacuation passes.
- **Rows 2-4 (ZGC, Shenandoah, Go)** — the modern low-pause band.
  All three buy sub-ms pause by paying **at every memory access**
  (load barriers, forwarding indirection, hybrid barriers).  The
  cost shifts from "occasional ms-scale pause" to "small constant
  added to every read or write".  All three also impose runtime-
  level constraints (Brooks header, multi-mapping, compiler
  integration) that make them difficult or impossible to embed.
- **Row 1 (protoCore)** — same pause band as ZGC/Shenandoah/Go, but
  with the per-access cost held at zero.  The cost is paid **once,
  at language-design time**: the runtime cannot have mutable slots
  per object; all mutability lives in `MUTABLE_ROOT_SHARDS` shards.
  Once that constraint is accepted, the snapshot mechanism makes
  every other GC mechanism (write barriers, load barriers,
  forwarding pointers, colored pointers) unnecessary.

The table is not "protoCore is better than X".  It is "protoCore makes
a different trade".  The pause-time win is real and measurable; the
constraint it costs is also real.  Runtimes that need per-object
mutable slots (a typical Java/C# semantics) cannot accept protoCore's
constraint and would not implement this design.  Runtimes that can
route mutability through a small shard table (protoCore, the proto*
language runtimes built on it, any embedder willing to adopt the
discipline) get the simpler mechanism.

See § "Concurrent Mark Without Barriers" above and
[`STW_ELIMINATION_RESEARCH.md`](./STW_ELIMINATION_RESEARCH.md) § 13
for the architectural argument in full.

### Real-time positioning

The pause profile above places protoCore in the **soft real-time**
category as the term is commonly used in the literature.  Concretely:

**Well-suited for:**
- Interactive UI and 60 fps games (a frame budget of about 16 ms)
- Web servers with p99 SLA in the millisecond range
- General server workloads (REST, microservices, message handlers)
- `protoST` as a digital-twin demonstrator at realistic actor counts
- `protoPython` GIL-free parallel workloads
- `protoJS` interactive scripting and serverless function dispatch

**Conditionally suitable** (achievable with documented mitigations):
- Professional audio at typical buffer sizes (≥ 256 frames / ~5 ms at
  48 kHz).  Requires safepoint instrumentation in the embedded
  interpreter loop to bound the safepoint-wait component.
- Low-latency web with p99 < 1 ms.  Achievable at moderate scale;
  requires controlling thread count and pin count.

**Not suitable** (no GC system in this latency class is):
- Hard real-time (deadline misses = system failure).  Use a real-time
  GC or stack-only allocation; protoCore is not a hard-RT GC.
- Audio at sub-millisecond buffers (e.g. 64-frame buffers, ~1.3 ms).
- HFT control loops (which typically forbid GC entirely).
- Sub-millisecond robotics control loops.

**The honest framing** — consistent with the project's no-overstatement
discipline (see `STW_ELIMINATION_RESEARCH.md` honesty disclaimer):
sub-ms pauses are *typical* on realistic workloads, not *guaranteed*
in a hard-bound sense.  Two pieces of follow-up work, both well-
understood, would convert the typical case into a verifiable p99
guarantee:

1. **Safepoint instrumentation** in the embedded interpreter loops
   (protoJS, protoPython, protoST) — a safepoint check every N opcodes
   bounds the worst-case thread-park latency.
2. **Latency benchmark harness** — measure the actual STW window
   distribution under representative workloads with
   `PROTOCORE_GC_PROFILE=1` (the per-cycle instrumentation already
   exists in `core/ProtoSpace.cpp`).  Convert the table above's
   estimates into measured p50/p99/p999 figures.

Neither is required for correctness; both would let the project make
a numerical p99 claim with the same rigour the rest of the codebase
operates under.

## Known issues

- **An exiting thread's unused cell batch is not returned.**  A
  `ProtoThread` allocates from a private freelist
  (`ProtoThreadExtension::freeCells`) that `getFreeCells` refills in
  batches of up to 65,536 cells when several threads run.  When the thread
  exits, the unused part of its last batch is returned to no freelist and
  belongs to no young generation, so no cycle reclaims it: each thread
  that exits can leave up to one batch of cells unusable.

## Future Research: Further bounding the STW pause

The current STW pause is already O(threads × stack depth + root-set pins
+ 256), independent of heap size.  The remaining cost is the per-thread
root scan.  A separate
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
