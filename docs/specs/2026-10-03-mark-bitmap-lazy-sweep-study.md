# A side mark-bit table and block sweeping: study and draft spec

Status: **study, draft for review**.  Nothing here is approved, built or
measured.  Date: 2026-10-03.  Base: protoCore 2.14.0 (master `01840bcc`); line
numbers refer to that commit.  Author: Gustavo Marino, with Claude.

Inputs: [../GarbageCollector.md](../GarbageCollector.md) (all of it),
[../MemoryModel.md](../MemoryModel.md),
[../GLOBAL_MUTABLE_TABLE.md](../GLOBAL_MUTABLE_TABLE.md),
[2026-10-03-collector-throughput-design.md](2026-10-03-collector-throughput-design.md)
("the throughput spec"),
[../reports/2026-10-03-gc-phase-breakdown.md](../reports/2026-10-03-gc-phase-breakdown.md)
("the phase report") and
[../reports/2026-10-03-collector-throughput.md](../reports/2026-10-03-collector-throughput.md)
("the throughput report").

The throughput spec put a side mark bitmap out of scope (§ 5.3, § 13 question
9), and decision 8 of its § 14 deferred it "only if the diagnosis calls for
it".  The maintainer then approved this study, after M4 and before the final
re-measurement, as the structural alternative to sweeping garbage cell by cell.
Copying or moving collection is excluded: protoCore is non-moving by model
(address-keyed caches, attribute order by address).

Every figure below that is not quoted from a report is an **estimate or a
hypothesis**, and is labelled as one.  Every figure quoted from a report comes
from synthetic workloads on one notebook-class CPU (AMD Ryzen 5 5500U, Zen 2,
6 cores, 8 MB L3, two DDR4 channels), except the noisy CI VM runs of the
throughput report's "Hardware class" section.

---

## 0. Summary

**Feasibility: yes**, without write barriers, without a change to the
concurrent-mark argument, without moving cells and without breaking ABI 3.
Every heap cell can be mapped to `(block, index)` with one AND if the OS
allocations of `ProtoSpace::getFreeCells` are carved into aligned blocks and
perennial cells move into blocks of their own.

**The structural change is the bitmap, not the laziness.**  Once liveness is
a bit in a side table, sweeping a block is arithmetic on its bitmap words
(about 16 words for a 64 KiB block) and never reads a dead cell.  That removes
the cost the reports measured (one dependent DRAM or cross-core miss per swept
cell, about 10-95 ns per cell on 2.14.0 with K = 0) whether the block arithmetic runs
eagerly on the collector's threads or lazily on the allocating thread.  The
remaining per-cell collection work is confined to dead cells with a
non-trivial finalizer.

**Recommended design (in brief):**

1. A **block-structured heap**: 64 KiB blocks aligned to 64 KiB, carved from
   the existing OS regions (still at most 16 MiB per request), each with a
   small in-block metadata header: a mark bitmap, a live bitmap (the result of
   the last sweep), a finalizable bitmap, and a header line with state, epochs,
   owner space id and allocation cursor.  About 0.7 % of the heap.
2. **Mark writes the bitmap**, with a plain store (one marker thread), never
   the live cell's line.  Cells of other spaces and perennial cells keep the
   header bit and a per-cycle list, as today, so the multi-space token
   invariant holds unchanged.  Phase 6 shrinks to those foreign cells.
3. **Allocation from blocks**: a thread owns one block at a time and takes its
   free cells in address order from the live bitmap.  A **block-epoch rule**
   replaces both "allocate black" and the dirty-segment capture: cells
   allocated after a stop-the-world are recognised from the block's epoch and
   cursor, so mutators never write a mark bit.
4. **Eager block sweep on collector threads** (the collector plus the 2.14
   helpers, partitioning blocks instead of segment runs).  Finalizable dead
   cells are found through the finalizable bitmap and finalized on collector
   threads exactly as the contract says.  Phase 5b is unchanged.
5. **Lazy sweep on the allocating thread** is specified (§ 3.8) but
   **recommended only as a later, gated stage**: once the sweep is a bitmap
   operation it saves little (a pass over 1/150 of the heap), and it needs a
   claim protocol, sweep termination and an approximate budget.  Finalizers
   stay off mutator threads in every variant.

Young chains stay: they are the pin that protects cells held only in C++
locals.  Dirty segments, the segment pool, the survivor pen, free chunks and the
per-thread freelists are not used in block mode.

**What could make it not worth it** (§ 8): an allocation fast path that gets
slower than today's freelist pop; mark slowed by metadata and TLB misses on
mark-dominated workloads; finalizable-heavy workloads (protoJS mutable style,
where every object handle is finalizable) gaining less; and the correctness
risk of the block-epoch protocol.  Each has a measurement or a test.

**Migration** (§ 7): six stages, each shippable and measurable, with
environment switches to compare against the 2.14 sweep; the losing path is
removed only after the decision.  No public layout changes; SOVERSION stays 3.

---

## 1. The cost this removes

### 1.1 What the header word does today

`Cell::next_and_flags` (`headers/proto_internal.h:796`) is one word: bit 0 is
the mark, bits 1-5 are reserved zero, bits 6-63 a link.  The link serves four
different chains, which is why the sweep has to walk them:

| Chain | Written by | Read by |
|---|---|---|
| a context's young chain (`lastAllocatedCell`) | the allocating thread, `ProtoContext::addCell2Context` (`core/ProtoContext.cpp:582`) | Phase 4's young walk (`core/ProtoSpace.cpp:836-849`); handed to a `DirtySegment` at context destruction (`ProtoContext::~ProtoContext`, line 287) or at a threshold safepoint (`ProtoContext::safepoint`, lines 389-401) |
| a dirty segment / survivor chain | the same links, unchanged on submission; survivors relinked by the sweep (`core/Sweep.cpp:236-249`) | the sweep (`sweepLoop`, `core/Sweep.cpp:172-258`) |
| a free chunk / freelist | the sweep (`internalSetNextRaw`, `core/Sweep.cpp:231`), the OS refill (`core/ProtoSpace.cpp:2241-2246`) | `getFreeCells` (line 1966) and every allocation: `ProtoThreadImplementation::implAllocCell` pops with `getNext()` (`core/Thread.cpp:568-571`) |
| the mark | the marker, `fetch_or` (`proto_internal.h:849`; mark loop `core/ProtoSpace.cpp:927-958`) | the marker (dedupe), the sweep, Phase 6 (`ProtoSpace.cpp:1081-1085`) |

### 1.2 Why the sweep touches every dead cell

The candidate set of a cycle is a set of chains (`segmentsToProcess`, captured
at `ProtoSpace.cpp:736`), so the only way to enumerate candidates is to follow
their links, and the only way to make a dead cell reusable is to write a link
into it.  Per candidate the 2.14 sweep does one acquire load of the header, a
virtual `finalize` on dead cells (a no-op for most types), one store to link the
dead cell into a free chunk, or one store to relink a survivor
(`core/Sweep.cpp:217-249`).  Later the allocating thread reads the link again to
pop the cell, and then `memset`s it (`core/ProtoContext.cpp:538`).

Measured (throughput report, M2 and M4): before 2.12.0 the collector took about
one DRAM miss per swept cell, one at a time (IPC 0.08, 1.11 demand DRAM fills
and 0.29 fills from other cores per cell on `clj_coll_t6`).  The multi-cursor
walk keeps several misses in flight (46 ns per cell on that workload); the
helpers divide the cells among threads.  Both are latency palliatives: the
number of dead lines touched by the collector is unchanged, and each dead line
is touched twice (by the collector, then by the allocating mutator).

### 1.3 What other collectors do, and what transfers

- **Generational copying** (HotSpot, V8 scavenger, .NET): cost proportional to
  survivors.  Excluded: it moves cells.
- **Go**: a span (block) of same-size objects with `gcmarkBits`, `allocBits`
  and a `freeindex`.  Sweeping a span swaps the bitmaps and counts; objects
  allocated during mark are marked black by `mallocgc`; spans are swept lazily
  by allocating goroutines and a background sweeper, and all must be swept
  before the next cycle ("sweep termination").  Finalizers are "specials" on a
  per-span list, so the sweep looks only at those objects.
- **Boehm**: per-block mark bits; lazy sweep builds free lists on demand (so it
  does touch dead objects, at reuse time).
- **V8 old space**: mark bitmap per page, concurrent and lazy sweeping into
  free lists.

What transfers to protoCore: protoCore has a single object size (one 64-byte
cell), so it needs no size classes, and every block is interchangeable.  What
does not transfer directly: Go's black allocation (a mutator writing mark bits
during mark) would break the "mark bit is GC-exclusive" invariant on which
`GarbageCollector.md` § "Concurrent Mark Without Barriers" point 2 rests; § 3.2
replaces it with a rule that keeps mutators out of the mark bitmap entirely.

---

## 2. Feasibility of a side bitmap (question 1)

### 2.1 Mapping a cell to (block, index)

Today `getFreeCells` requests `blocksToAllocate * sizeof(BigCell)` bytes with
64-byte alignment (`ProtoSpace.cpp:2226-2228`), where `blocksToAllocate` is the
batch (60,000-65,536 cells with several threads), 409,600 cells single-threaded,
clamped to `kMaxBlocksPerOSAllocation` (16 MiB, `ProtoSpace.cpp:98-100`) and, under
a hard limit, to the headroom (any number of cells, line 2210).  The base
pointer is discarded (MemoryModel.md § 1).  Nothing maps an address back to its
allocation.

**Proposal.**

- A **block** is 64 KiB = 1,024 lines, aligned to 64 KiB.  `blockOf(cell) =
  cell & ~0xFFFF`, `index = (cell & 0xFFFF) >> 6`.  One AND, one shift, no
  table.
- An **OS region** is a whole number of blocks, at most 16 MiB (256 blocks), as
  now.  The sizing policy is kept and rounded up to whole blocks; under a hard
  limit the headroom clamp rounds down to whole blocks, with a minimum of one.
  A heap limit is therefore honoured with a granularity of 1,017 usable cells
  (a limit of 500,000 cells, the README example, is 491 blocks).
- **Alignment.**  POSIX: `posix_memalign(65536, n * 65536)` (glibc serves large
  requests with `mmap` and aligns inside the mapping; the slack is virtual and
  not resident).  Windows: `VirtualAlloc` returns addresses aligned to the
  allocation granularity, which is 64 KiB, so a 64 KiB block needs no
  over-allocation there; `_aligned_malloc(size, 65536)` would also work but
  commits up to 64 KiB of slack per region (0.4 % of a 16 MiB region).
  `alignedArenaAlloc` (`proto_internal.h:117-125`) is the single place to
  change.  64 KiB is chosen because it is the Windows granularity, keeps the
  bitmap of one block at two cache lines, and bounds what a thread can hold
  unused (§ 3.1).  256 KiB is the alternative (§ 9, question 2).

### 2.2 Cells outside blocks: perennials

`ProtoContext::allocCell` with a null `this` allocates a **perennial** cell
with its own `alignedArenaAlloc(64, 64)` (`core/ProtoContext.cpp:511-535`):
symbols (`core/SymbolTable.cpp`), symbol candidates (`ProtoString::createSymbol`)
and the empty shard lists installed by the first space
(`ProtoSpace.cpp:1415`).  Such a cell is not inside any block, and
`blockOf()` on it would read unrelated malloc memory.  The marker does reach
perennial cells (a root or a live cell can reference a symbol), so it must be
able to classify them.

**Proposal: perennial blocks.**  The null-context branch takes cells from a
process-global chain of blocks tagged `PERENNIAL`, under a mutex (perennial
allocation is rare: interning).  Their cells are never swept.  Marking them
uses the header bit, as today (§ 2.6).  This is the only allocation site of
cells outside the heap: every `Cell` is created through
`Cell::operator new(size, ProtoContext*)` (`core/Cell.cpp:57-60`), which calls
`allocCell`; the code has no other placement into foreign memory.

An alternative that leaves perennials where they are is a page map (Go's arena
index: a two-level table on `address >> 16` that returns null outside the
heap).  It costs a dependent load per mark and a table; perennial blocks cost
nothing per mark and also remove one `posix_memalign` per symbol.

### 2.3 Block metadata

| Line(s) | Content | Writers | Readers |
|---|---|---|---|
| 0 (header) | state, `acquiredEpoch`, `markEpoch`, owner space id, cursor (written back at retirement), counters | owner at acquisition and retirement; sweeper; marker (`markEpoch`) | sweeper, marker, allocator |
| 1-2 | **mark** bitmap, 1,024 bits | marker only | sweeper |
| 3-4 | **live** bitmap: occupied as of the last sweep | sweeper only | the owner (to find free cells), sweeper |
| 5-6 | **finalizable** bitmap | owner (at construction of a finalizable cell), sweeper (clears) | sweeper |

Seven lines per block: 1,017 usable cells, 0.68 % of the heap.  The mark bitmap
alone is the 1/512 of the brief.  Indexes 0-6 are permanently occupied in the
live bitmap, so the allocator never hands them out.  The three bitmaps sit on
separate lines so the marker's writes never invalidate the lines the owner
reads.  `heapSize` and the budgets keep counting usable cells.

### 2.4 Where the bitmap lives

- **In the block** (the table above): mapping by AND; the metadata survives
  as long as the block, which matters because blocks are never freed and blocks
  of a destroyed space stay reachable through the global mutable table
  (GLOBAL_MUTABLE_TABLE.md, "Table nodes of a destroyed space").  Cost: each
  block's metadata is on its own 4 KiB page, so a mark that touches many blocks
  touches many metadata pages (TLB pressure; a hypothesis to measure with
  `dTLB-load-misses`; transparent huge pages via `madvise(MADV_HUGEPAGE)` on
  the regions would remove it on Linux).
- **Per region, side by side** (the 256 mark bitmaps of a 16 MiB region
  contiguous, 32 KiB): denser for mark, but `regionOf()` needs 16 MiB
  alignment (large virtual slack on POSIX, a reserve-then-commit dance on
  Windows) or a lookup table.

**Recommendation:** in-block metadata first (simplest, and stage A measures
it); move to per-region metadata only if the mark measurements show TLB misses
that matter.  Question 3 in § 9.

### 2.5 How mark sets a bit

The marker is one thread (throughput spec § 7: mark is not parallelised).
During mark no other thread writes a mark word: mutators never do (§ 3.2), the
eager sweep runs after mark, and markers of other spaces use header bits for
cells that are not theirs (§ 2.6).  So setting a bit is a **plain** relaxed load,
OR and store on the bitmap word, not a locked `fetch_or`, and it writes a
metadata line, not the live cell's line.

```
b = blockOf(c)
if b.ownerSpace != space.id:           # foreign or perennial: § 2.6
    header path, as today
else:
    if b.markEpoch != E: b.mark[] = 0; b.markEpoch = E      # lazy clear, § 3.6
    w = b.mark[i >> 6]
    if w & bit(i): skip                 # already marked: no read of the cell
    b.mark[i >> 6] = w | bit(i); marked++
    c.processReferences(...)            # the cell is read, as today
```

Effects, as hypotheses: (1) no write to a live line during mark, so the
mutators' shared copies of live cells are not invalidated (the throughput
spec's H5); (2) a cell reached a second time costs a metadata read instead of a
read of the cell's line, which matters for persistent structures, where
structural sharing makes the marker reach the same node many times; (3) the
check can move to `pushReportedReference` (`ProtoSpace.cpp:213`) so a marked
cell is never pushed.  Against: one more line (the block header) per first
visit, and the TLB point of § 2.4.

If mark is ever parallelised, the store becomes a `fetch_or` whose returned
value decides which marker owns the cell (the sketch in the throughput spec
§ 7); 64 cells share a word, so markers would contend on hot words.

`markEpoch` makes stale bits harmless: a bitmap is valid only for the cycle in
its tag, the first mark of a cycle in a block clears it, and nothing has to
clear bitmaps of blocks the cycle never touches.  This replaces Phase 6 for
own-space cells (§ 3.6).

### 2.6 Several spaces

The global mutable table makes every space's marker reach cells of other
spaces (GLOBAL_MUTABLE_TABLE.md, point 3).  The process-wide token exists so
"two collectors never share a cell's mark bit" (GarbageCollector.md, "Several
spaces in one process").  With block-local bitmaps that a lazy or eager sweep
consumes after the token, a foreign marker must not write them.

**Rule:** a marker uses the bitmap only for blocks whose owner id is its own
space's id.  For every other cell (another space's block, a destroyed space's
block, a perennial block) it uses the header bit and a `foreignMarked` list, and
clears those bits before it releases the token: today's Phase 6, restricted to
foreign cells.  With one space in the process, `foreignMarked` holds only the
perennial cells the mark reached.  Space ids are process-unique and never
reused (GLOBAL_MUTABLE_TABLE.md, point 2), so a block of a destroyed space can
never be mistaken for a live space's block.

Today a foreign marker can set a header bit on a cell the owner's mutators
read; it cannot set one on a cell the owner is reallocating, because a cell
dead in its own space is unreachable from the table snapshot of any later cycle
(the grace period covers C++ locals; § 4.5).  That argument is unchanged.

### 2.7 Concurrent mark without barriers still holds

The seven points of GarbageCollector.md § "Why this is sufficient", re-read for
block mode:

1. The traversed graph is immutable: unchanged.
2. The mark bit is GC-exclusive: **strengthened**.  Mutators never write a mark
   word or a header mark bit; there is no black allocation (§ 3.2).
3. Workers cannot free cells: unchanged.  Freeing is a change of a block's
   live bitmap, done by a sweeper of a block no mutator owns (§ 3.1).
4. The candidate set is fixed at stop-the-world: **re-stated**.  It is no
   longer a captured list but "every occupied cell of a block that is not
   owned when it is swept, except cells allocated after the stop-the-world",
   and § 3.2 shows the exception is computable without any capture.
5. Cells allocated after stop-the-world are not candidates: by the block-epoch
   rule (§ 3.2).
6. Mutable-shard CAS is invisible to the marker: unchanged.
7. Per-thread caches are not roots and are cleared after each stop-the-world:
   unchanged; the block retirement of § 3.2 can run at the same point.

The stop-the-world pause gains no term: nothing in § 3 runs under the pause
except the collector's own block retirement, O(1).

---

## 3. Sweeping by block (question 2)

### 3.1 Block states and allocation

A block is in one of four states:

| State | Meaning | Who may touch the live / finalizable bitmaps |
|---|---|---|
| `READY` | swept, has free cells, in the space's pool | nobody (it is published) |
| `OWNED` | an allocator takes cells from it | its owner (reads live, writes finalizable) |
| `FULL` | retired by its owner, awaiting a sweep | nobody until a sweeper claims it |
| `SWEEPING` | claimed by exactly one sweeper | that sweeper |

Transitions: `READY -> OWNED` (acquisition, one CAS or under the pool's
mutex); `OWNED -> FULL` (retirement by the owner, a release store); `FULL ->
SWEEPING` (claim by a sweeper; with an eager sweep the claim is an atomic
index into the block table, so no CAS is needed); `SWEEPING -> READY` (has free
cells) or `-> FULL` (none).  A fresh region enters as `READY` blocks with an
empty live bitmap.  This is the claim discipline the maintainer asked for: two
sweepers never sweep one block, and a block may hold cells of many threads and
epochs without that mattering, because the bitmap does not care who allocated a
cell.

**Allocation fast path** (replaces the freelist pop of
`ProtoThreadImplementation::implAllocCell`, `core/Thread.cpp:554-573`, and the
per-context pop of `allocCell`, `ProtoContext.cpp:487-499`):

```
cache = {block, cursor, bits}      # bits: inverted live word from cursor, shifted
if bits == 0: refill bits from the next live word, or acquire a new block
i = ctz(bits); cursor = word_base + i + 1; bits >>= i + 1 (Go's allocCache)
cell = block + 64 * index;  memset(cell, 0, 64)
```

No load from the free cell precedes its use.  Today the pop reads the free
cell's link, a dependent miss per allocation on recycled memory; here the next
free addresses are known from the bitmap, so the allocator can prefetch them
for write several cells ahead.  On hardware with a whole-line zero instruction
(AMD `CLZERO`, which Zen 2 has; Arm `DC ZVA`), the line need not be read from
memory at all, because nothing in it is needed.  Both are hypotheses and
hardware-sensitive (§ 6.3).

**Refill.**  An exhausted block is retired (`FULL`) and a `READY` block is
acquired; with an empty pool the space grows by a region, under the existing
limit, pacing and controller paths (`getFreeCells`'s structure stays: limit
checks, waits, `adaptive::pace`, OS growth).  A thread holds at most one block
per space (at most 1,017 cells, against 8,192-65,536 in a batch today), so the
`kLimitBatchFraction` cap (`ProtoSpace.cpp:2011-2026`) is no longer needed in
block mode.  The pool is a per-space structure under a mutex: one acquisition
per up to 1,017 cells.  With many threads it can be sharded (per NUMA node,
§ 6.3).

**Sparse blocks.**  A `READY` block with very few free cells costs a refill for
few cells.  The sweep may withhold blocks below a threshold of free cells from
the pool (they stay `FULL` and are re-swept next cycle); the cells withheld are
bounded by threshold x blocks.  A tunable, not a design point (§ 9, question 8).

**Where the cache lives.**  For threads, beside the per-thread caches in the
internal `ProtoThreadExtension` (one per thread per space; its
`freeCells` field is internal, `proto_internal.h`, so its layout may change).
For thread-less contexts, which today keep `ProtoContext::freeCells` (a public
field of ABI 3), a `thread_local` cache per (OS thread, space) in `core/`; the
public field stays and is unused in block mode (question 9).

### 3.2 Cells allocated during a cycle: the block-epoch rule

Today a cell allocated after the stop-the-world is safe because it is in a
young chain, never in `segmentsToProcess`.  In block mode a sweeper sees only
bitmaps, so it must recognise those cells another way.  Go marks them black at
allocation, an atomic write by the mutator into the mark bitmap.  The proposal
instead uses two facts protoCore already has: `gcCycleCount` advances only
under stop-the-world (`ProtoSpace.cpp:719`), and a thread resumes from
every stop-the-world through a point that synchronises with it (the cache-clear
points of GarbageCollector.md "Concurrent Mark Without Barriers", point 7).

Rules (E is the cycle whose stop-the-world just happened):

- **R1, ownership.**  Only a block's owner allocates from it; only an unowned
  block (`FULL`, claimed) is swept.
- **R2, retirement.**  Before its first allocation after a stop-the-world of
  its space, an allocator retires a block it acquired in an earlier cycle
  (`acquiredEpoch < E`).  The simplest robust form is a check on the
  allocation path itself: compare the cache's epoch with `gcCycleCount` (a
  relaxed load of a read-mostly line, ordered after the resume by the mutex or
  condition variable every resume path passes through), and retire on a
  mismatch.  The collector thread retires its own cache (for `gcContext`'s
  Phase 5b allocations) in Phase 2.
- **R3, liveness at the sweep of cycle E:**
  `live_new = mark_E | (acquiredEpoch == E ? (~live_old & below(cursor)) : 0)`.
  A block acquired in E came out of the pool after the stop-the-world, and a
  pooled block receives no allocation, so every slot it handed out since its
  last sweep (`~live_old` below the cursor: the allocator takes every free slot
  in order) was allocated after the stop-the-world and is kept.  A block
  acquired earlier received no allocation after the stop-the-world (R2), so
  its occupants are all candidates and liveness is the mark alone.
- **R4, mark hygiene.**  A mark bitmap is valid only when its `markEpoch` is
  the cycle being swept (§ 2.5).
- **R5, finalizable cells** (§ 4).
- **R6, foreign cells** (§ 2.6).

**Soundness sketch.**  A cell freed by the sweep of E is in a block that no one
owns, is not marked in E, and is not in the R3 exception, so it was allocated
before the stop-the-world of E.  Such a cell is either unreachable from every
root captured at that stop-the-world (stack roots, young chains, the mutable
snapshot, the global roots), in which case it is garbage by the existing
argument, or reachable, in which case mark E reached it and set its bit.
Nothing allocated after the stop-the-world is freed: R2 keeps it out of
pre-E blocks and R3 keeps it in E blocks.

**What it costs.**  No atomic per allocation and no mutator write to a mark
word.  A block that a thread still owns when the sweep runs is skipped; its
dead cells wait one cycle (it is swept after its retirement).  That is at most
one block per allocating thread per space per cycle of floating garbage, the
same order as today's per-thread batch.  A thread that sits in an
`UnmanagedScope` for many cycles keeps its block, and its dead cells, until it
returns: bounded by one block per thread.

**The `ProtoMPSCQueue` protocol** (GarbageCollector.md, "Lock-free queues
without barriers") relies on "nodes pushed after the pause are young cells,
not candidates".  Under R2/R3 they are allocated after the stop-the-world and
are kept.  Its two orderings are unaffected.

### 3.3 What happens to young chains, dirty segments and the freelist

- **Young chains stay.**  They are the pin that keeps cells held only in C++
  locals alive (the reason for `ReturnReference` and the order in
  `~ProtoContext`, `ProtoContext.cpp:240-288`).  Phase 2 still records one head
  per context (`ProtoSpace.cpp:485-498`).  Phase 4 pushes the young cells
  themselves as roots instead of only their references (today's walk,
  `ProtoSpace.cpp:836-849`): in block mode an unmarked young cell would be
  freed.  The walk already reads every young cell; marking it is a bitmap
  store.  Timing is unchanged: a mark does not outlive its cycle (R4), so a
  young cell of a context destroyed after the stop-the-world of E is
  reclaimed in E + 1 if unreachable, as today.
- **Submitting a young chain becomes dropping it.**  `submitYoungGeneration`
  (`ProtoSpace.cpp:2350`), the threshold submission in `safepoint()` and the
  Phase 5b hand-over (`ProtoSpace.cpp:316-321`) only clear the context's head:
  no segment pop, no CAS push.  Every allocated cell is a candidate at every
  cycle unless something pins it, so "submission" no longer gates candidacy.
- **Not used in block mode:** `dirtySegments`, `dirtySegmentFreePool`,
  `survivorPen`, `freeChunks`, `freeCells` / `freeCellsTail` of `ProtoSpace`,
  `ProtoContext::freeCells`, the segment runs and multi-cursor walk of
  `core/Sweep.cpp`.  They stay in the public layouts (ABI 3) until the losing
  path is removed (§ 7).

### 3.4 Survivors

A survivor is a set bit; it is never relinked, never re-chained, never
written.  The survivor pen and `survivorStagger` exist to limit the cost of
re-sweeping survivors; in block mode a survivor costs the sweep nothing beyond
its share of a bitmap word, so both have no purpose.  Consequences to accept:
`survivorStagger` is ignored in block mode (the public field stays), and the
`PROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` build option, in which a survivor is
never a candidate again, has no block-mode equivalent (question 6).

### 3.5 Cells kept alive by omission today

Some cells survive today only because their young chain is never submitted.
`ProtoSpace::newThread` drops the scratch context's chain on purpose
(`ProtoSpace.cpp:1719-1730`: "Dropping the young chain keeps them out of every
cycle"): a `ProtoThread`'s two cells stay alive although, after the thread
leaves `space->threads`, only C++ memory the collector does not scan holds the
handle.  In block mode an unreachable cell is freed whether or not its chain was
submitted, so such cells would be freed under a live handle.

So block mode needs an inventory of "immortal by omission" cells, and an
explicit pin for each.  Known site: `newThread`.  Candidates to audit: every
place that clears `lastAllocatedCell` without submitting
(`ProtoContext.cpp:396`, `ProtoSpace.cpp:318`, `ProtoSpace.cpp:1729`; the first
two do submit), contexts never destroyed, and embedder threads that violate
EMBEDDER-CONFORMANCE rule 11 (already unsupported, but today some of them
"work").  The cleaner pin is to make the cells reachable: an internal root
entry held from `newThread` until `ProtoThread::join` releases the handle,
which would also end today's leak of those two cells.  A "pinned" bitmap is the
alternative (question 5).  A debug **heap verifier** (§ 8.3) is how the
inventory is checked rather than trusted.

### 3.6 The eager block sweep

After mark (and, with several spaces, after the grace period for the
publication step only), the collector and the helpers sweep the space's block
table, partitioned by an atomic index in runs of blocks (no linked list to walk
under a claim lock, so 2.14's claim-walk regression cannot recur):

```
for each block b claimed:
    if b.state != FULL: continue                 # OWNED or READY: R1
    occupied = b.live | below(b.cursor)     # every slot below the cursor is occupied
    keep     = markValid(b, E) | (b.acquiredEpoch == E ? (~b.live & below(b.cursor)) : 0)
    dead     = occupied & ~keep
    finDead  = dead & b.fin                       # § 4: finalize these
    b.fin   &= keep
    freed   += popcount(dead)
    b.live   = keep (+ the 7 metadata slots);  b.cursor = 0
    b.state  = has_free(b) ? READY (published to the pool) : FULL
```

Per block: one header line, two mark lines, two live lines, two finalizable
lines read; two live lines and the header written.  About 450 bytes per 64 KiB.
The dead cells themselves are never read, except the finalizable ones.  The
bulk unmark (Phase 6) has nothing to do for own-space cells: R4 retires the
bitmap.  `liveCellsLastCycle` comes from the marker's counter.

Helpers are useful only for large heaps (§ 6.2); the 2.14 pool, its engagement
rule and the embedder-finalizer hand-back apply as they are.

### 3.7 Cells that are never reallocated

In the eager design, nothing: a free cell is a zero bit, and it costs no work
until it is allocated.  It is counted free from the end of the sweep.

### 3.8 The lazy variant

Lazy sweeping moves the block arithmetic of § 3.6 to the thread that needs a
block.  Specified here so the maintainer can weigh it; **not recommended as the
first step** (§ 0, § 6.2).

- **Publication.**  At the end of mark E (and of the finalization pass of
  § 4.2), the space publishes `sweepEpoch = E`.  A `FULL` block whose
  `sweptEpoch < E` needs sweeping.
- **Claim.**  A refill that finds the pool empty claims an unswept `FULL` block
  by CAS `sweptEpoch: E-1 -> SWEEPING`, sweeps it with § 3.6's arithmetic
  (finalizable cells already handled), and, if it has free cells, owns it
  directly (`acquiredEpoch = E`).  The CAS is the only synchronisation: two
  sweepers never sweep one block.  To find candidates without scanning, the
  claimant takes indices from a per-space atomic cursor over a list of the
  blocks that were `FULL` at publication.
- **Background finishing.**  The collector (and helpers, when engaged) sweeps
  the remaining blocks after publication, in the same claim order.  Before the
  next stop-the-world every block `FULL` at publication must be swept ("sweep
  termination", as in Go); the collector finishes them before it requests the
  stop-the-world.  Blocks never claimed by a mutator are thus finished by the
  collector, at the same cost as the eager sweep.
- **Accounting.**  A cell is free for budget purposes when its block is swept.
  Between publication and termination the reclaimable count is an estimate:
  `occupied(FULL blocks at publication) - marked`, which needs per-block mark
  counts (one more counter write per first mark, in the block header the marker
  already touches).  The out-of-memory rule ("cells not reclaimable after a
  completed cycle", GarbageCollector.md, "Out of memory") needs exact numbers,
  so it is evaluated only after termination.
- **Mutators do collection work.**  Bitmap arithmetic only (about 100-200 ns
  per block of up to 1,017 cells, an estimate), never finalizers.  The
  throughput spec § 6.5 ruled out "sweep on the allocating core" because it
  would move finalizers onto mutator threads; this variant does not, but it does
  move collection work onto mutators.  That is the maintainer's decision
  (question 1).

What lazy buys over eager: the cycle's collector work ends at mark (plus the
finalization pass); the metadata lines are touched on the core that will
allocate from the block (temporal, per-core locality, NUMA-friendly); and very
large heaps avoid one pass over 0.7 % of their size.  What it costs: the claim
protocol, termination, approximate accounting, and the eager pass anyway for
finalizable cells.

### 3.9 Pacing and the adaptive controller

- **When is a cell free?**  Eager: at the end of the block sweep, as today at
  the end of the cell sweep.  Lazy: § 3.8.
- **`freeCellsCount`** becomes the free cells in `READY` blocks; cells of an
  `OWNED` block count as handed out, as a batch does today.  The controller's
  out-of-memory rule subtracts "one refill batch per running thread"; in block
  mode that is one block (1,017 cells) per thread, smaller than today.
- **T** (cells freed per second of collector busy time) should rise sharply,
  because busy time loses the sweep; **C** (cycle duration) should shrink to
  about mark plus Phase 5b; **r** is unchanged in definition (allocation now
  carries the reuse miss it already carried).  The control law and pacing need
  no change in form; their inputs move (hypothesis).
- **L** (`liveCellsLastCycle`) now includes the young cells of live contexts,
  which are marked (§ 3.3); today it excludes those not otherwise reachable.  A
  small, systematic change of a controller input (question 7).

---

## 4. Finalizers and Phase 5b (question 3)

### 4.1 The constraint

GarbageCollector.md § 7: finalizers run on the collector's threads, never on a
mutator; built-in ones may run concurrently on the collector thread and its
helpers; embedder (`ProtoExternalPointer`) callbacks run on the collector
thread, one at a time.  A lazy sweep on a mutator that met a dead finalizable
cell would violate this.

Today `finalize` is a virtual call on every dead cell.  Non-trivial
implementations at `01840bcc`: `ProtoObjectCell::finalize` for a mutable handle
(`mutable_ref > 0`, `core/ProtoObject.cpp:518-528`: records the ref),
`ProtoExternalPointerImplementation::finalize` (embedder callback,
`core/ProtoExternalPointer.cpp:49`), `ProtoExternalBufferImplementation::finalize`
(`alignedFree`, `core/ProtoExternalBuffer.cpp:67`),
`ProtoByteBufferImplementation::finalize` (`delete[]` when `freeOnExit`,
`core/ProtoByteBuffer.cpp:70`).  The others (`LargeInteger`, `Double`,
`ProtoMethodCell`, the thread cells, tuples, strings and iterators) are empty.

### 4.2 Options

- **(a) Finalizable cells handled by the collector.**  Each block has a
  finalizable bitmap; the constructor of a cell with a non-trivial finalizer
  sets its bit (the owner's write, § 2.3).  The sweep, eager or the lazy
  variant's finalization pass, computes `fin & dead` and finalizes exactly those
  cells on collector threads, then clears their bits.  The addresses come from
  bits, so they are independent loads and prefetch perfectly (no pointer chase).
  The contract is unchanged.
- **(b) A finalization queue.**  A lazy mutator sweeper pushes dead finalizable
  cells to a queue drained by the collector, and keeps them occupied until
  finalized.  Phase 5b then runs on refs that arrive asynchronously, so a
  mutable's state is released later, and mutators do collector bookkeeping.
- **(c) Finalizable cells in separate blocks.**  The allocator would have to
  know the type, and for `ProtoObjectCell` a run-time value (`mutable_ref`),
  before construction.  A type-aware allocation path is a special case that
  exists only for performance.

**Recommendation: (a).**  It keeps the contract word for word, works for both
the eager and the lazy variant, and confines per-cell collection work to the
cells that actually need it.  (b) and (c) are rejected for the reasons given.

### 4.3 Setting and checking the finalizable bit

The bit is set in the constructors of the four types above (for
`ProtoObjectCell` only when `mutable_ref > 0`; for `ProtoByteBuffer` only when
`freeOnExit`), through one internal function, by the thread that constructs the
cell, which is the block's owner.  Perennial cells never get it.

The risk is a future type with a non-trivial `finalize` that forgets the bit:
its finalizer would never run.  Two guards: a test that constructs one cell of
every `CellType` and checks that "has a non-trivial finalizer" and "sets the
bit" agree (a table in the test, reviewed when a type is added), and a debug
mode that calls `finalize` on every dead cell of a swept block and asserts
that the cells without the bit do nothing (§ 8.3).  GarbageCollector.md § 7
gets one sentence: a built-in finalizer must also register its cell as
finalizable.

### 4.4 Phase 5b and `gcFinalizedMutableRefs`

Unchanged.  The refs of dead mutable handles are recorded by the same
`ProtoObjectCell::finalize`, into the sweeper's sink (`sweep::finalizedRefSink`)
or `gcFinalizedMutableRefs`; the collector merges them before
`releaseFinalizedMutableEntries` (`ProtoSpace.cpp:265`, called at line 1041 or,
with several spaces, line 1147).  Phase 5b still allocates through `gcContext`
on the collector thread; that context's block is retired in Phase 2 (R2), and
its young chain is dropped at the end of Phase 5b.

In mutable-style protoJS every JavaScript object is a mutable handle, so a
large share of dead cells may be finalizable there; in the paradigm's own style
(protoScala, protoClojure: persistent structures, few mutables) a small share.
The counter C4 of the throughput spec (finalizations by type) measures which.

### 4.5 Several spaces: the grace period

With more than one space live, today's sweep only collects dead cells, and
finalization and reuse wait for the grace period (`ProtoSpace.cpp:1003-1004`,
1112-1148).  In block mode the sweep computes the bitmaps and the list of dead
finalizable cells; the blocks are published `READY` and the finalizers run only
after `multispace::waitForGracePeriod` (`core/MultiSpace.cpp:393`).  Because the
sweep never writes a dead cell (today it must not "even rewrite their header"),
a reader of another space that still holds a dead cell in C++ locals sees it
untouched until reuse, which the grace period already orders.

---

## 5. Several spaces, the process budget and teardown (question 4)

- **Ownership.**  Each block records its owner's space id; each space has its
  own block table and pool.  A cell never moves between spaces, and neither
  does a block (memory never moves between spaces today either).
- **Budget.**  H stays the sum of the spaces' heaps in usable cells; growth is
  by whole regions, rounded to blocks; `adaptive::afterHeapGrowth` is called per
  region as now.  The out-of-memory rule's "whole heaps of the other spaces"
  term is unchanged.
- **Teardown.**  `~ProtoSpace` frees no cell memory today (MemoryModel.md § 1)
  and must not while the table is global (GLOBAL_MUTABLE_TABLE.md, "Table nodes
  of a destroyed space").  In block mode its blocks stay, with their metadata
  and owner id; other spaces' markers treat them as foreign (§ 2.6).  The block
  table itself (a side structure in `core/`) is freed.  Exiting threads retire
  their block (`releaseExitingThread`, `core/Thread.cpp`), which replaces
  `returnUnusedCellBatch`; the unused cells come back at the next sweep.
- **Issue #2** (a destroyed space does not return memory): blocks make it
  possible in principle to adopt a destroyed space's fully free blocks into a
  later space, but cells of a destroyed space can stay shared in live table
  entries, so this is out of scope (question 12).
- **Fork.**  No new thread; nothing changes.

---

## 6. Cost model (question 5)

### 6.1 Work per cycle, before and after

Symbols: L live cells marked; Y young cells of live contexts; G dead candidates;
S survivors among candidates; B blocks in the space (heap / 1,024); F dead
finalizable cells; A cells allocated between cycles.

| | 2.14 (segments) | Block mode, eager | Block mode, lazy |
|---|---|---|---|
| Stop-the-world | O(threads x depth + pins + 256) | same | same |
| Mark | L visits; a locked write to each live line; young walk reads Y | L visits; a plain write to a metadata line; Y pushed as roots; re-reached cells cost a metadata read | same as eager |
| Sweep (collector threads) | (G + S) cells, each a dependent read of the cell and a write to it; about 10-95 ns per cell at K = 0, 2.3-2.6x less with helpers where lines are far, little less on the t6 workloads (throughput report, M4) | B blocks x about 450 B of metadata, plus F finalizable cells with independent addresses | F cells (finalization pass) plus the blocks no mutator claimed before termination |
| Bulk unmark | L tests, some writes | foreign cells only | foreign cells only |
| Allocation (mutators) | A pops, each reading the free cell's link (a dependent miss on recycled memory), then a `memset`; a refill per 8,192-65,536 cells under `globalMutex` | A bit scans with known addresses (prefetchable), then the `memset`; an acquisition per <= 1,017 cells | same, plus the block sweep on the claiming thread |
| Writes to live lines by the collector | mark (L), survivor relink (S) | none (own space) | none (own space) |

### 6.2 Estimates (hypotheses, to be measured)

- **Collector sweep time per cycle.**  A 10 M-cell heap has about 9,830 blocks,
  about 4.4 MB of metadata reads.  At an assumed 100-200 ns per block (several
  lines from DRAM, a TLB miss per block with 4 KiB pages), 1-2 ms on one
  thread.  The 2.14 sweep of the aged `js10_records_n12` run costs 36.7 ns per
  swept cell at K = 0 (throughput report, M4), that is several hundred
  milliseconds for a cycle that sweeps most of a 10 M-cell heap.  For
  `clj_coll_t6` (2 M-cell limit, 99 % of swept cells freed, sweep 98 % of
  collector time in the phase report) the collector's remaining work would be
  mark and Phase 5b.  So the expectation is that **collector busy time becomes
  mark-bound** on every workload of the reports; how much wall time that buys is
  bounded by the phase report's Amdahl table, not predicted here.
- **Very large heaps.**  128 GiB of cells is about 2 M blocks, about 900 MB of
  metadata reads per eager sweep: on the order of 100 ms on one thread, divided
  by the helpers.  This is where the lazy variant or per-region metadata would
  start to matter; mark of a live set that size is far larger.
- **Allocation.**  On recycled memory, the per-allocation miss moves from a
  dependent pop to an independent, prefetchable address; on fresh memory both
  are sequential.  Expected: allocation no slower on recycled memory, and
  possibly faster; on fresh memory a few more instructions per allocation
  (the bit scan) against a pop.  The single-threaded `perf stat` set of the
  throughput report is the regression check.
- **Memory.**  +0.7 % metadata; up to one block (64 KiB) per thread per space
  held by allocators; the sparse-block threshold's withheld cells; one cycle of
  extra floating garbage in blocks owned at sweep time (§ 3.2).  Fewer cells
  held per thread than today's batches.

### 6.3 Hardware class

Following the throughput report's classification:

| Element | Class | Notebook (DEV12: 6 cores, 8 MB L3, two channels) | Server / NUMA (hypotheses) |
|---|---|---|---|
| Side bitmap, block sweep (no dead-cell reads by the collector) | **hardware-robust**: removes work and memory traffic | removes the sweep's share of a saturable two-channel memory (S1 in the throughput report: loaded DRAM quadrupled the per-cell sweep cost) | removes cross-node reads of dead lines by the collector; only metadata crosses nodes |
| No mark writes to live lines | hardware-robust | small (H5 measured at about 10 ns per survivor in S3 - S3b) | larger across sockets: fewer invalidations of readers' copies |
| Block size, sparse-block threshold | hardware-sensitive (TLB, L2 size) | measured here | measure there |
| Allocation prefetch distance, whole-line zeroing (`CLZERO`, `DC ZVA`) | **hardware-sensitive** | `CLZERO` exists on Zen 2; measure | Intel has no user-mode equivalent; Arm servers have `DC ZVA` |
| Helpers for the block sweep | hardware-sensitive, but small work | probably unneeded below tens of millions of cells | useful for very large heaps; partition by node |
| Lazy sweep | adapts per core | little expected | per-core, per-node metadata locality; with per-node pools, reuse stays node-local (first-touch placement is preserved because the collector no longer touches dead cells) |

As before, every hardware-sensitive parameter gets an environment variable and
a static setter, with defaults from what is cheap to detect.

### 6.4 Fresh against recycled memory

The maintainer's refinement applies: a cell is one line, so contiguity buys only
pages (TLB) and possibly hardware stream prefetch.

- **Collector side:** the difference disappears.  The eager sweep reads
  metadata in block-table order whatever the heap's age; only finalizable dead
  cells are read, at addresses known in advance.
- **Allocation side:** today fresh chunks are allocated in address order and
  recycled chunks in sweep order (scattered).  In block mode allocation is in
  ascending address order within a block for fresh and recycled blocks alike;
  recycled blocks have gaps where live cells sit.  Hypothesis: an aged heap
  allocates closer to a fresh one, because order is restored per block at every
  sweep.  What ageing does create is sparse blocks (few free cells each), so the
  aged-heap measurement must report acquisitions per thousand cells.

---

## 7. Migration path (question 6)

Each stage is a release that can ship alone, with its own measurements and a
switch to compare against the previous behaviour.  Switches are read when the
first `ProtoSpace` is constructed (the allocator is process-level) and have
ABI-additive static setters on `ProtoSpace`, like the 2.14 sweep parameters.

| Stage | Content | Switch | Expected change | Gate to the next stage |
|---|---|---|---|---|
| 0 | Instrumentation only: mutator allocation cost (cycles per refill, cells per refill, by origin), TLB counters in the mark, finalizations by type (C4), an aged-heap harness (an hour, not minutes) | `PROTOCORE_GC_INSTRUMENT` | none | baselines recorded on 2.14 |
| A | Block-structured regions: aligned 64 KiB blocks with metadata headers, block table, `blockOf()`, perennial blocks.  Cells of blocks are still chained into free chunks; segments, sweep and freelists unchanged | none (expected neutral; reverted if not) | RSS +0.7 %; nothing else | neutral within noise on the reports' set and the single-threaded set; Windows and macOS CI green |
| B | Side mark bitmap for own-space cells, header bit for foreign and perennial ones.  The segment sweep reads the bitmap; Phase 6 only for foreign cells | `PROTOCORE_GC_MARK=header\|bitmap` | mark time and live-line writes measured on their own (S3, mark-dominated runs) | mark not slower beyond noise on mark-dominated runs (near-full heap) |
| C | Allocation from blocks, block-epoch rule, finalizable bitmap, eager block sweep (collector + helpers), young cells as roots, explicit pins (§ 3.5), heap verifier for tests | `PROTOCORE_GC_SWEEP=segments\|blocks` | collector busy time mark-bound; allocation cost measured | § 8.5 decision rules |
| D | Hardware-sensitive allocation options: prefetch distance, whole-line zeroing, sparse-block threshold, per-node pools | one variable each | per hardware class | each kept only where it pays |
| E | Lazy variant (§ 3.8), only if the stage-C measurements show the eager pass or metadata locality matter (§ 8.5) | `PROTOCORE_GC_SWEEP=blocks-lazy` | collector work ends at mark | the maintainer's decision |
| F | Removal of the losing path: segments, segment pool, pen, chunks, multi-cursor walk (or of the block allocator) | removed | simpler code | after the decision |

Both allocators coexist only from C to F.  That is a deliberate, temporary
duplication to measure against the 2.14 sweep; keeping two collectors
permanently would be the kind of model fragmentation the project rejects.

**ABI.**  No public layout changes: `Cell` keeps its header (`next_and_flags`
is still the young-chain link and the foreign mark bit), `ProtoSpace` and
`ProtoContext` keep every field (some unused in block mode), no virtual is added
(the `Cell` vtable is internal anyway), `allocCell` and `getFreeCells` are
out-of-line already, and new state lives in side structures in `core/` (the
`AdaptiveHeap` pattern).  New static setters are additive.  SOVERSION stays 3.
Removing the dead fields in stage F would be a SOVERSION 4 change for no
functional gain, so they stay as documented reserved fields.

**Visible behaviour changes** (release notes): `survivorStagger` ignored in
block mode; `PROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` unsupported in block mode;
heap growth and limits in whole 64 KiB blocks; L counts young cells; a
`ProtoThread`'s cells released at join (if question 5 chooses the root pin).

---

## 8. Risks, what could make it not worth it, and how to decide (question 7)

### 8.1 Risks

- **Soundness of the block-epoch rule.**  A missed retirement (R2) frees live
  cells.  Mitigation: the check sits on the allocation path, not on a list of
  resume points; deterministic tests for every resume shape (allocation poll,
  `safepoint`, `synchToGC`, heap-headroom wait, return from `UnmanagedScope`,
  `parkForStopTheWorld` in `newList` and `takeAll`, a thread shared by two
  spaces, `gcContext` in Phase 5b); the heap verifier; ThreadSanitizer runs.
- **Immortal-by-omission cells** (§ 3.5): an incomplete inventory frees live
  cells.  The verifier and the existing suites of every runtime are the check.
- **Allocation fast path.**  A bit scan plus a cursor against a pop: if the
  single-threaded allocation-heavy benchmarks lose more than noise, the gain
  must come from elsewhere.
- **Mark slower** on mark-dominated workloads (near a full heap) through
  metadata lines and TLB misses.
- **Finalizable-heavy workloads** gain less: in mutable-style protoJS many
  dead cells are handles and are still read (with independent addresses).
- **Two allocators during evaluation**: complexity, more test matrix.
- **Granularity**: very small heap limits in tests (below a few blocks) behave
  differently.
- **Windows**: alignment through `VirtualAlloc`; commit charge.

### 8.2 What would make it not worth it

- Stage C shows collector busy time falling but wall time not moving, because
  the waits that remain are mark, Phase 5b or the mutators' own allocation cost
  (the throughput report already saw wall gains far below sweep gains with
  helpers).  Then the change is still a reduction of CPU and memory traffic,
  and the decision is about code size against that.
- Allocation becomes measurably slower on single-threaded or fresh-memory
  workloads by more than the collector saves on them.
- Mark-dominated workloads lose more than sweep-dominated ones gain.

### 8.3 Correctness checks (deterministic, run everywhere)

- **Heap verifier** (test builds): after a sweep, under a test-only
  stop-the-world, re-mark from the same kinds of roots into a scratch bitmap and
  assert that no cell the sweep freed is reachable, and that every allocated
  cell outside the freed set is either reachable, young, or allocated after the
  stop-the-world.
- **Finalizer registration** test over every `CellType` (§ 4.3), and the debug
  mode that runs `finalize` on every dead cell and checks the unregistered ones
  are no-ops.
- Existing suites: the GC, multi-space, MPSC queue, context-return-anchor,
  thread-exit, adaptive heap and conformance tests, in both modes while both
  exist; ThreadSanitizer.
- Every runtime's suite against the stage-C library (with `ldd` checked).

### 8.4 Measurement plan (for the agent that runs timings)

Run each stage against 2.14.0 in the same session, interleaved, median of 3,
self-verifying workloads only (their checksums), as in the throughput report.

- **Micro:** `sweep_contention_benchmark` S0-S5, extended to print collector
  busy per freed cell and mutator ns per allocation (its "sweep ns per cell"
  means something else in block mode); an allocation micro-benchmark on fresh,
  recycled-quiet and aged memory; the six single-threaded benchmarks with
  `perf stat -r 3` (instructions and cycles).
- **Runtime set:** `js10_records_n12`, `js40_wordfreq_n12`,
  `jsad_records_n12`, `clj_coll_t6`, `scala_tree_t6`, their aged variants,
  `core_fixed640_live1M`, `core_adaptive_live1M`, and the t1 runs.  Plus one
  finalizable-heavy run (mutable-style protoJS) and one mark-dominated run (a
  live set near the limit).
- **Per run:** wall, collector busy, mark and sweep time, wait share, process
  CPU, peak RSS, cycles, block acquisitions per 1,000 cells, fresh against
  recycled share; on the collector and on one mutator thread, `perf stat`
  cycles, IPC, demand DRAM and cross-core fills (Zen 2 `ls_refills_from_sys`),
  `dTLB-load-misses`.
- **Hardware:** DEV12 (notebook class) when idle; the CI `sweep-hardware.yml`
  VMs as a direction only; a multi-socket server when available (question 13).
- **Aged heap:** at least one run of an hour on DEV12, recording the
  sparse-block distribution and acquisitions per cell over time.

### 8.5 Decision rules (proposed)

Adopt block mode (stage C) as the default if, on DEV12 and at least one other
hardware class:

1. no single-threaded benchmark regresses beyond its measured noise
   (instructions within +2 %, cycles within noise);
2. collector busy time falls on every multi-threaded workload, and wall time
   does not rise beyond noise on any;
3. peak RSS within +5 % (metadata, owned blocks, floating garbage);
4. the verifier and every suite pass, including ThreadSanitizer and the
   runtimes' suites.

Proceed to stage E only if, with stage C, the eager block pass exceeds 5 % of
collector busy time on some measured workload, or the server measurements show
remote metadata traffic that per-node pools do not remove.

---

## 9. Open questions for the maintainer (question 8)

1. **Direction.**  Accept the bitmap with an eager block sweep on collector
   threads as the structural change, with the lazy variant as a gated later
   stage?  If lazy is wanted from the start: may mutators do bitmap sweeping
   (never finalizers), revising the throughput spec § 6.5?
2. **Block size**: 64 KiB (Windows granularity, two-line bitmaps) or 256 KiB
   (fewer acquisitions, more held per thread)?
3. **Metadata placement**: in-block header (proposed) or per-region side
   arrays (denser for mark, harder alignment)?
4. **Perennial blocks**: move perennial cells (symbols) into process-global
   blocks marked through the header bit, replacing one `posix_memalign` per
   symbol?
5. **Cells alive by omission**: pin a `ProtoThread`'s cells through an internal
   root released at `join` (proposed; also ends today's two-cell leak per
   thread), or keep immortality through a pinned bitmap?
6. **Survivor knobs**: accept that `survivorStagger` and
   `PROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` have no meaning in block mode?
7. **Controller input**: accept that L includes the young cells of live
   contexts?
8. **Sparse blocks**: a threshold below which a swept block is withheld from
   the pool, and its default?
9. **Thread-less contexts**: a `thread_local` allocation cache per (OS thread,
   space) in place of `ProtoContext::freeCells`; is a thread-less context ever
   used from two OS threads, which would break it?
10. **Coexistence**: both allocators in one library from stage C until the
    decision, then removal of the loser (proposed)?
11. **Whole-line zeroing** (`CLZERO`, `DC ZVA`) as a hardware-sensitive,
    detected option?
12. **Out of scope, to confirm**: returning fully free blocks to the OS (the
    heap never shrinks: MemoryModel.md § 1), and adopting a destroyed space's
    blocks (issue #2).
13. **Server access** for the measurements of § 8.4.

---

## Appendix: code map at `01840bcc`

| What | Where |
|---|---|
| Cell header, mark, links | `headers/proto_internal.h:772-890` (`next_and_flags` 796, `mark`/`unmark`/`isMarked` 849-851, `setNext` 854-872, `internalSetNextRaw` 884-888) |
| `BigCell`, 64-byte bound | `headers/proto_internal.h:2488-2529` |
| `DirtySegment` | `headers/proto_internal.h:2566-2569` |
| Arena allocation | `alignedArenaAlloc`, `headers/proto_internal.h:117-125` |
| Allocation | `ProtoContext::allocCell`, `core/ProtoContext.cpp:457-570` (perennial branch 511-535); `addCell2Context` 582; `ProtoThreadImplementation::implAllocCell`, `core/Thread.cpp:554-573` |
| Refill and OS growth | `ProtoSpace::getFreeCells`, `core/ProtoSpace.cpp:1966-2289`; `kMaxBytesPerOSAllocation` 98; batch caps 103-114 |
| Young-chain submission | `ProtoSpace::submitYoungGeneration`, `core/ProtoSpace.cpp:2350`; `~ProtoContext`, `core/ProtoContext.cpp:240-300`; threshold in `ProtoContext::safepoint`, 372-402 |
| Collector loop | `gcThreadLoop`, `core/ProtoSpace.cpp:324`: roots and young heads 462-500, shard snapshot 622-647, cycle count 719, pen and segments capture 724-736, resume 771, young walk 836-849, pen walk 851-880, mark 927-958, sweep call 1011, Phase 5b 1041, Phase 6 1081-1085, token release 1107-1113, grace path 1114-1148, cycle end 1212-1230 |
| Sweep | `core/Sweep.cpp`: `sweepLoop` 172, `claimRun` 280, `sweepCycle` 562; `core/Sweep.h` |
| Phase 5b | `releaseFinalizedMutableEntries`, `core/ProtoSpace.cpp:265-322` |
| Finalizers | `ProtoObjectCell::finalize`, `core/ProtoObject.cpp:518`; external pointer, buffer, byte buffer as cited in § 4.1 |
| Cache clear after stop-the-world | `ProtoThreadExtension::clearCachesAfterStopTheWorld`, `core/Thread.cpp:288`; `multispace::clearMemberCaches`, `core/MultiSpace.cpp:180-191` |
| Grace period | `multispace::waitForGracePeriod`, `core/MultiSpace.cpp:393` |
| `newThread` scratch context | `core/ProtoSpace.cpp:1702-1730` |
| Public fields that become unused in block mode | `headers/protoCore.h`: `ProtoSpace::freeCells`, `freeCellsTail`, `freeChunks`, `freeChunkPool`, `dirtySegments`, `dirtySegmentFreePool`, `survivorPen`, `survivorStagger` (2717-2793); `ProtoContext::freeCells` (1990) |
