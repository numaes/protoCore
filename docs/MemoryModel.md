# protoCore memory model: sizing a process

This document states, explicitly, how much memory a process that embeds
protoCore needs, and where the kernel's responsibility for memory ends.  It is
written for two readers: the operator who has to decide how large a machine to
provision, and the embedder who has to decide who owns a buffer.

Everything below is a statement about the current code, with the file and line
that implements it.  The two design decisions it records — *the heap never
shrinks* and *external memory is the allocator's responsibility* — are
deliberate, and the sections that state them also state what they buy.

Related documents: [GarbageCollector.md](GarbageCollector.md) (the collection
cycle), [../DESIGN.md](../DESIGN.md) § 1 (the memory model as architecture,
including perennial allocation and `ProtoRootSet`).

---

## 1. The sizing rule

> **The working set of a process is the size of its perennials, plus the sum of
> every `ProtoSpace`'s *peak* working set, plus all memory not managed by
> protoCore.**

```
process working set  =  perennials
                      + Σ  peak(ProtoSpace_i)        over every live space
                      + memory not managed by protoCore
```

If that much memory is not available, the process does not fit.  The answer is
a larger machine, not a cleverer allocator: protoCore has no mechanism — and by
design will not acquire one — that makes a process fit into less than its peak.

The three terms are defined precisely in §2, §3 and §5.  §4 explains why *peak*
and not steady state is the term that counts, and why that is where sizing
estimates usually go wrong by an order of magnitude.

### Why the rule is exact, not an upper bound

protoCore never returns memory to the operating system.  Resident size
therefore rises to the high-water mark of the formula and stays there: there is
no sawtooth, no decay, no trimming.  Three facts in the code make this exact:

* `heapSize` — the count of `Cell`s obtained from the OS — is incremented in
  exactly one place, when `getFreeCells` grows the heap
  (`core/ProtoSpace.cpp:1797`).  Nothing ever decrements it.
* The base pointer of each OS allocation is not retained.  `getFreeCells`
  calls `posix_memalign` for `blocksToAllocate * sizeof(BigCell)`, chains the
  block into cells and keeps only the cell chain
  (`core/ProtoSpace.cpp:1766-1793`).  After that call returns, the allocation
  cannot be freed even in principle, because no code holds the pointer
  `free` would need.
* `ProtoSpace::~ProtoSpace` (`core/ProtoSpace.cpp:1325`) deletes bookkeeping —
  root sets, the GC and root contexts, the symbol table, the tuple interner,
  the `DirtySegment` nodes — and frees no cell memory.  Destroying a space does
  not give its heap back either.

Collection moves cells from live to free *inside* the heap; it does not shrink
it.  DESIGN.md states the same for the bounded case: "reclamation moves Cells
from live to free *within* the heap, it does not shrink `heapSize`"
([../DESIGN.md](../DESIGN.md), § "The Heap Allocation Limit and Out-of-Memory
Detection").

Measured, in a process that builds one 100,000-element `ProtoList` in a child
context, drops it, and then runs collection cycles to completion:

| | heap |
|---|---|
| at startup | 262,144 cells — 16.00 MiB |
| at the peak of the build | 2,097,152 cells — 128.00 MiB |
| after the list is unreachable and cycles have run | 2,097,152 cells — 128.00 MiB |

An allocator that returned pages would be kinder to a shared machine and
impossible to size, because the number an operator needs would depend on a
release policy interacting with an allocation pattern.  For protoCore's
targets — a fixed-capacity server, a digital twin, a long-running embedded
runtime — a resident size that converges to a computable constant is the
correct trade.  It is the property that makes the rule above arithmetic rather
than a guess.

### Fixing a ceiling

`ProtoSpace::setHeapLimits(softCells, hardCells)` fixes a space's ceiling in
`Cell`s (`headers/protoCore.h:2043`, implementation
`core/ProtoSpace.cpp:1820-1830`; also settable at startup with
`PROTOCORE_HEAP_LIMIT_CELLS`, see [../README.md](../README.md) § "Runtime
Configuration").  `heapSize` never crosses `maxHeapSize` for ordinary mutator
allocations; a thread that would cross it waits for reclamation, and two
consecutive cycles that reclaim nothing escalate to the out-of-memory path.
The full contract is in [../DESIGN.md](../DESIGN.md) § "The Heap Allocation
Limit and Out-of-Memory Detection".

A limit bounds one term of the rule and no other:

* it bounds that space's cell heap;
* it does **not** bound perennials (§2) — those bypass `getFreeCells`
  entirely and are never counted in `heapSize`;
* it does **not** bound the other spaces in the process (§3) — the limit is a
  `ProtoSpace` member (`headers/protoCore.h:2317-2321`), one per space;
* it does **not** bound memory not managed by protoCore (§5) — external bytes
  are invisible to it.

A ceiling also makes GC pressure deterministic, which is what makes a heap
limit the right tool for a test that has to exercise the collector rather than
the allocator.

---

## 2. Term 1 — perennials

A **perennial** allocation is monotone: never collected, never freed, never
counted.  It lives for the lifetime of the process.

The mechanism already exists and is used by the kernel itself.  An allocation
made with a **null `ProtoContext`** becomes perennial:

1. `Cell::operator new(size, context)` forwards to `context->allocCell()`
   (`core/Cell.cpp:57-60`).  With `context == nullptr` this is a call on a null
   pointer, which the allocator expects: `ProtoContext::allocCell` tests `this`
   at every branch.
2. Both branches that could place the cell in a heap are skipped, and control
   reaches the final `else`, which calls
   `posix_memalign(…, 64, sizeof(BigCell))` directly
   (`core/ProtoContext.cpp:502-506`).  The cell comes from the C library, not
   from any `ProtoSpace`.
3. `Cell::Cell` then declines to enrol it: the registration is guarded by
   `if (context)` (`core/Cell.cpp:38-40`), so `addCell2Context` is not even
   called.  `ProtoContext::addCell2Context` carries the same guard on its own
   receiver and simply clears the link for a null context
   (`core/ProtoContext.cpp:553-563`).

The consequences follow mechanically.  The cell belongs to no space; it is on
no thread freelist and in no context's young generation, so it never reaches
`dirtySegments` and the sweep never examines it; no root scan needs to reach
it; `heapSize` does not count it (`core/ProtoSpace.cpp:1797` is the only
increment, and it is on the `getFreeCells` path this allocation never takes);
and nothing frees it — not collection, not `~ProtoSpace`.

**Who allocates perennials.** Every interned string.
`SymbolTable::intern` builds the canonical `ProtoStringImplementation` and its
whole AVL tree with a null context, deliberately and without a collectible
variant, so that a name keeps its canonical pointer for the life of the process
and pointer-identity symbol comparison is sound
(`core/SymbolTable.cpp:8-17`, `core/SymbolTable.cpp:65-77`,
`core/SymbolTable.cpp:143-161`).  That includes
the candidate that loses an insert race: it is perennial too and simply becomes
unreferenced (`core/SymbolTable.cpp:160-161`).  `SymbolTable::~SymbolTable`
frees its bucket chains (`core/SymbolTable.cpp:27-36`) — the C++ nodes, not the
cells.

Interned tuples are perennial as well
([GarbageCollector.md](GarbageCollector.md) § "Phase 2", and the STW cost table
in that document lists `SymbolTable` as *perennial — never scanned*).

**Who else may.** An embedder, for the same kind of object: a per-type
prototype, a canonical constant, language vocabulary.  The contract — and in
particular the rule that *the entire reachable subgraph must be null-context
too* — is [../DESIGN.md](../DESIGN.md) § "Mechanism A — Perpetual allocation
via `ProtoContext* = nullptr`".

**Sizing.** Perennials are a separate term because no protoCore counter
reports them: they are outside `heapSize`, outside a heap limit and outside the
cycle statistics.  An operator estimates them from the vocabulary the process
interns — attribute names, keywords, literals, type prototypes — at 64 bytes
per cell, and treats the number as a constant the process reaches soon after
start-up and never gives back.

---

## 3. Term 2 — the sum of the peaks, not the maximum

`heapSize`, `maxHeapSize`, `freeCellsCount`, the freelists, the mutable-shard
table and the collector are all per-`ProtoSpace`
(`headers/protoCore.h:2315-2321`).  Two spaces in one process hold two heaps at
the same time, and each grows to its own peak independently.  The term is
therefore a **sum over spaces**, not a maximum.

This matters because each runtime in the family owns its own space:

* protoST: `proto::ProtoSpace space;` as a member of `STRuntime::Impl`
  (`protoST/src/runtime/STRuntime.cpp:184-185`);
* protoScala: `proto::ProtoSpace space_;` as the first member of `Session`
  (`protoScala/src/repl/Session.h:84`);
* protoPython holds a `proto::ProtoSpace*` in `PythonEnvironment`
  (`protoPython/include/protoPython/PythonEnvironment.h:1051`), and
  protoClojure constructs one per process entry point
  (`protoClojure/src/main.cpp:73`).

A polyglot process that embeds two of these runtimes has two spaces, two heaps
and two GC threads, and its cell budget is the sum of both peaks.  A single
space shared by two embedders is the other valid arrangement — root sets keep
them isolated ([../DESIGN.md](../DESIGN.md) § "Mechanism B") — and then there
is one term to size, not two.

---

## 4. Peak, not steady state

The term is each space's **peak**, and this is where sizing usually goes wrong
by an order of magnitude, because an immutable bulk build allocates far more
cells than the structure it leaves behind.

The resting structure is compact: `ProtoListImplementation` is one cell holding
one value plus two child pointers (`headers/proto_internal.h:1470-1478`), and
leaves carry null children, so an *n*-element list at rest is *n* cells — 64
bytes per element.

The build is not.  `ProtoContext::newList(n, items)`
(`headers/protoCore.h:1619`, implementation `core/ProtoContext.cpp:691-749`)
produces the AVL form by repeated `appendLast` over an empty list.  Each
`appendLast` is an `implInsertAt` (`core/ProtoList.cpp:185-187`) that
path-copies a new node at every level it descends and rebalances on the way
out, allocating further nodes for rotations (`core/ProtoList.cpp:164-182`,
`rebalance` at `core/ProtoList.cpp:58-82`).  The old spine is not mutated —
that is the immutability guarantee — so every superseded node is garbage the
instant the next one is built.  The cost is on the order of `n·log₂(n)`
transient cells to leave `n` live.

Measured on this build (protoCore 2.1.0, `build_release`), counting
`ProtoContext::allocatedCellsCount` across the call:

| n | cells allocated by `newList` | cells per element | `log₂(n)` |
|---|---|---|---|
| 1,000 | 11,958 | 11.96 | 9.97 |
| 10,000 | 153,590 | 15.36 | 13.29 |
| 100,000 | 1,868,896 | 18.69 | 16.61 |

So a 100,000-element list touches about **1.87 million cells against 100,000
resting — 19:1**.  The observed ratio grows like `log₂(n)` with a constant near
1.13–1.20 above it, which is the rebalancing allocations on top of the path
copy.  The analytic `n·log₂(n)` figure for n = 100,000 is 1.66 million; the
measurement is 13% above it, and `n·log₂(n)` should be read as the shape of the
curve rather than as the number to provision.

Two consequences for sizing:

* **Measuring at rest undersizes by an order of magnitude.** The 100,000-element
  list rests in 100,000 cells — 6.10 MiB — and drove the heap to 128 MiB (§1).  A measurement
  taken after the build, or from a steady-state snapshot, misses the term that
  actually determines resident size.
* **The amplification is transient, and it is still yours.** Those cells become
  garbage immediately and a cycle reclaims them, but the heap had to grow to
  hold them, and because the heap never shrinks (§1) that growth is permanent.
  Peak allocation, not live data, is what the machine must hold.

The same shape applies to any bulk immutable build — `ProtoString` rope
construction, `ProtoSparseList` and object-attribute inserts, `ProtoList`
`extend` — for the same reason: path copying leaves a copy of the path behind
on every step.  `ProtoContext::newList(n, items)` is the case measured here.

An embedder that cares about the peak of a bulk build has ordinary tools for
it: build in a child `ProtoContext` and destroy it, which submits the young
generation for reclamation; call `ProtoContext::safepoint()` inside long loops
so the collector can run at all; and, where the peak must be bounded rather
than merely reclaimed, set a hard heap limit so the builder waits for
reclamation instead of growing the heap.

---

## 5. Term 3 — memory not managed by protoCore

> **Conformance.** The obligations this section places on an embedder are rule 7
> of `docs/EMBEDDER-CONFORMANCE.md`, and they are partly executable: the case
> `external.finalizer_runs` asserts that a dropped wrapper's finalizer runs
> exactly once, and the static check `external_finalizer` reports a finalizer that
> blocks, that calls back into protoCore, or that is absent altogether. What this
> document says cannot be decided by the kernel — whether a declared byte total is
> accurate, and whether a null finalizer is correct because the embedder frees the
> memory elsewhere — stays a judgement item (checklist C7), for the same reason
> given below: the kernel has no way to detect the drift.

This section is a **design boundary, not an open question.**

### What protoCore holds, and what it does not

`ProtoExternalPointer` and `ProtoExternalBuffer` are each a single 64-byte cell
that refers to memory outside the cell heap.

* `ProtoContext::fromExternalPointer(void* pointer, void (*finalizer)(void*))`
  (`headers/protoCore.h:1599`, `core/ProtoContext.cpp:902-904`) wraps a pointer
  the embedder already owns.  The cell stores the pointer and the callback
  (`headers/proto_internal.h:2006-2020`).
* `ProtoContext::newExternalBuffer(unsigned long size)`
  (`headers/protoCore.h:1641`) allocates a contiguous segment with
  `std::aligned_alloc` and ties it to the descriptor cell
  (`core/ProtoExternalBuffer.cpp:18-28`).

The cell is 64 bytes.  What it refers to may be a gigabyte, and **that memory
is invisible to every protoCore mechanism**:

* it does not count toward a heap limit, because the only thing counted is
  cells obtained by `getFreeCells` (`core/ProtoSpace.cpp:1797`) and `size` is
  never added to anything;
* it creates no collection pressure — nothing about its size influences the GC
  trigger, which looks only at the cell free ratio
  (`ProtoSpace::triggerGC`, `core/ProtoSpace.cpp:1865-1879`);
* it does not appear in `heapSize` or in the cycle statistics
  (`reclaimedLastCycle`, `gcCycleCount`, `headers/protoCore.h:2251`,
  `headers/protoCore.h:2274`), which count cells;
* neither cell type reports any reference to the marker
  (`ProtoExternalPointerImplementation::processReferences` and its buffer
  counterpart are deliberately empty — `core/ProtoExternalPointer.cpp:37-47`,
  `core/ProtoExternalBuffer.cpp:52-58`), so the external region is not part of
  the traced graph at all.

The same is true of everything else the embedder allocates for itself:
bytecode, source text, JIT code, socket buffers, `std::` containers, a
mapped file.  It is one term of the sizing rule and protoCore does not measure
it.

### The most protoCore can offer is the finalizer

When the wrapping cell is found unreachable, the sweep calls its finalizer
(`core/ProtoSpace.cpp:843-847`).  For a `ProtoExternalBuffer` protoCore
performs the release itself — `finalize` frees the segment
(`core/ProtoExternalBuffer.cpp:60-65`).  For a `ProtoExternalPointer` it calls
the callback the embedder supplied (`core/ProtoExternalPointer.cpp:49-53`).
That is the whole of what the kernel can do.  Going further is impossible; the
next subsection says why.

The finalizer contract is the one in
[GarbageCollector.md](GarbageCollector.md) § 7 "Finalizer contract", repeated
at the declaration (`headers/proto_internal.h:680-695`):

> `Cell::finalize` runs on the GC thread during sweep, concurrently with the
> mutators.  A finalizer only **completes an action on an internal or external
> structure**: free an external buffer, run an external pointer's callback,
> record a number in collector bookkeeping.  It **never allocates cells, never
> publishes to a shared structure with compare-and-swap, never loops over
> protoCore data and never dereferences other `ProtoObject*`**.

A finalizer must also not **block**.  It runs on the single GC thread, inside
the sweep, so a finalizer that waits stalls collection for the whole space and
— with a heap limit configured — stalls every mutator waiting for reclamation
behind it.  The practical consequence: **a release that can block is not safe
in a finalizer.** A device or accelerator synchronisation, a `join`, a lock
that another thread may hold, an unmap of a network-backed mapping, an ordered
flush — none of these belong here.  Such a release is the embedder's to
schedule; the finalizer may at most hand the resource to a queue the embedder
drains elsewhere, and even then the enqueue must be non-blocking and must not
allocate cells.

Two further limits follow from *when* a finalizer runs:

* It runs when the sweep runs.  There is no promptness guarantee, and
  protoCore defers collection by design.
* It runs only for a cell that actually reaches a sweep as unreachable.  A
  perennial wrapper is never swept, so its finalizer never runs; and nothing
  runs finalizers at process exit — `~ProtoSpace`
  (`core/ProtoSpace.cpp:1325`) does not sweep.  External memory still held at
  shutdown is released by the operating system tearing the process down, not by
  protoCore.

### Why going further is impossible

A reader may reasonably ask protoCore to account external bytes — to add `size`
to a budget, or to let a gigabyte of foreign memory trigger a cycle.  It is not
a missing feature; it cannot be made to work:

* **The kernel would be deciding from a number it cannot verify.** External
  size is whatever the embedder declares.  For `fromExternalPointer` there is
  no size at all, only a pointer.  The embedder may wrap memory it shares with
  another owner, resize it, release it early, or double-count a region reached
  through two wrappers.  A collection policy driven by that number is a policy
  driven by a figure that can drift arbitrarily from reality, and the kernel
  has no way to detect the drift.
* **It would not even fix timeliness.** A finalizer runs when the sweep runs.
  Making external release prompt would mean triggering cycles from the foreign
  number — a stop-the-world pause, a mark and a sweep of the whole space
  ordered by an embedder's accounting.  That contradicts protoCore's deliberate
  deferral of collection: cycles are driven by cell pressure, which the kernel
  measures itself.

So the boundary is drawn where the kernel's knowledge ends.  protoCore offers
the one thing it can honour exactly — a callback at collection — and nothing it
would have to guess at.

### What the embedder does instead

The section is a division of labour, not a disclaimer.  The embedder knows its
own external total, and has three things to do with it:

1. **Count it.** The allocator of external memory is the only party that knows
   the size, the sharing and the lifetime.  Keep the total, and include it in
   the sizing rule as its own term.
2. **Advise the collector.** `ProtoSpace::triggerGC()`
   (`headers/protoCore.h:2021`, `core/ProtoSpace.cpp:1865-1879`) is advisory: it
   notifies the collector when fewer than 20% of the heap's cells are free (or
   when a cycle is already under way), and otherwise does nothing at all.
   An embedder that has just dropped many wrappers may call it to bring the
   sweep — and the finalizers — forward.  It is a hint, not a guarantee, and it
   is the right amount of influence to expose.
3. **Offer explicit release.** For a caller that cannot wait for a cycle, or
   for a resource whose release can block, provide an explicit
   `close`/`dispose` on the embedder side, make it idempotent, and leave the
   finalizer as the backstop that runs if the caller never calls it.

**The responsibility for bounding external memory is the allocator's, by
design.**

---

## 6. Using the rule

To size a process:

1. Estimate the **perennials**: the interned vocabulary and any null-context
   objects the embedder creates, at 64 bytes per cell.  Constant after
   start-up.
2. For **each `ProtoSpace`**, take its **peak** cell count — measured under the
   workload that builds the most, not at rest — at 64 bytes per cell, and sum
   across spaces.  Where the peak must be bounded rather than observed, fix it
   with `setHeapLimits` and size to the ceiling.
3. Add **everything the embedder allocates itself**, external buffers and
   wrapped pointers included, from the embedder's own accounting.
4. Provision for the total.  Resident size will rise to it and stay there.

If the total does not fit, the workload must be made smaller or the machine
made bigger.  There is no third option, and protoCore does not pretend to offer
one.

---

## Reproducing the measurements

The figures in §1 and §4 were taken on protoCore 2.1.0 from `build_release`
with two short probes linked against `libprotoCore.so`: one calling
`ProtoContext::newList(n, items)` for n = 1,000 / 10,000 / 100,000 and printing
the difference in `ProtoContext::allocatedCellsCount` across the call, the other
building a single 100,000-element list in a child context, destroying the
context, driving collection cycles with `ProtoSpace::triggerGC()` and printing
`ProtoSpace::heapSize` before, at the peak and after.  Both read public members,
add nothing to the build and are reproducible in a dozen lines.
