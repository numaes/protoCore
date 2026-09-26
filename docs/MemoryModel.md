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

§7 is the one section that is not about sizing: it states the single structural
reason a peak can contain a term that never comes back down — **a cycle among
mutable objects is never collected** — and the exact diagnostic that finds one.

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

## 7. Retention: the one thing the collector does not resolve

§1–§6 size a process from its **peak**.  This section states the one structural
reason a peak can contain a term that never comes back down, and it is not a
bug: it is a consequence of the mechanism that lets protoCore mark concurrently
without write barriers.

> **A cycle among mutable objects is never collected.**

The retention is **bounded** — it is the cycle's own cells, once, not growth —
and it is **permanent**: there is no monotone progress, so a later collection
does not fix it.  Some cycles are an oversight and can be removed; others are
what the program means, and for those the retention simply stands.  Both cases
are below, and `ProtoSpace::findMutableCycles` (§7.5) exists to make the
difference visible and countable rather than to imply that every cycle is a
defect.

### 7.1 Why — the two sites that combine

A mutable object is a pair.  The **handle** is a `ProtoObjectCell` carrying a
non-zero `mutable_ref` (`headers/proto_internal.h:849`); the **state** is an
ordinary immutable `ProtoObjectCell` with `mutable_ref == 0`, published into
`ProtoSpace::mutableRoot[mutable_ref % 256]` by a compare-and-swap
(`core/ProtoObject.cpp:1099-1101`).  The table entry is `(mutable_ref → V)` in a
sparse list **keyed by an integer**, so the table does not reference the handle
`H`; it references `V`.

Two places in the collector then combine:

* **Phase 2** (`core/ProtoSpace.cpp:519-524`) adds each shard's whole
  `ProtoSparseList` to the work list as a root, **unconditionally**.  So every
  mutable's current value is marked whether or not anything still references its
  handle.  The table **originates** marking; it does not merely preserve values.
* **Phase 5b** (`core/ProtoSpace.cpp:992-1000`, implementation
  `core/ProtoSpace.cpp:232-283`) removes an entry only once the sweep has
  **finalized** its handle — that is, only for a handle found unreachable.  The
  finalizer that records it is `ProtoObjectCell::finalize`
  (`core/ProtoObject.cpp:515-521`), and it fires only on an unmarked candidate.

So if `V` reaches `H`, then `H` is marked, so it is never swept, so it never
finalizes, so its entry is never removed — and the next cycle is bit-identical.

### 7.2 The acyclic case works, and this is what it relies on

The ordinary case is correct and must not be confused with the one above:
table → `V`, `V` does not reach `H`, nobody else holds `H`.  Then `H` is
unreachable, it is swept, it is finalized, Phase 5b releases its entry, and `V`
dies in the following cycle.

That holds for an acyclic **chain** of mutables too, one level per cycle.  If
`H1 → H2` and there is no cycle, `H2`'s liveness merely follows `H1`'s: when
`H1` dies its entry goes, `V1` dies next cycle, and `H2`'s entry goes on the
cycle after that.  **An acyclic handle-to-handle edge is therefore not a
violation**, which is why the detector does not report one.  The commonest shape
in this family is exactly that: a mutable instance whose birth prototype chain
points at a mutable class (`newChild(ctx, true)`), thousands of times over.

Measured, in `test/MutableCycleDetectorTests.cpp`: 400 acyclic mutables created
in a child context and dropped were released from the table down to **≤ 10% of
what the workload created** after 8 cycles; 200 two-handle cycles created the
same way retained **all 400 entries**, and the retained count was **identical**
after a second round of 8 cycles.

### 7.3 Two refinements the code contradicted a first reading of

Both were found by writing the detector and then running it.

* **An entry exists only after the first write, not at creation.**
  `ProtoContext::newObject(true)` (`core/ProtoContext.cpp:863-875`) allocates a
  handle with a fresh `mutable_ref` and publishes nothing; the first
  `setAttribute` is what CASes an entry into the shard.  A never-written mutable
  therefore originates no marking and cannot be in a cycle.  This is why a bare
  `ProtoSpace` reports **zero** handles even though `objectPrototype` is created
  mutable (`core/ProtoSpace.cpp:1228`): protoCore's own bootstrap never writes
  to it.  The embedder's first `setAttribute` on it is what puts it in the table.
* **A handle references other handles through the fields it was born with.**  A
  handle's `parent` chain and `attributes` are fixed at construction and are
  never written back to — `setAttribute` publishes a new state and returns the
  same handle — but they are still traced: `ProtoObjectCell::processReferences`
  (`core/ProtoObject.cpp:528-556`) reports both to the collector.  So an edge
  between two handles can exist that passes through no state at all, and a
  cycle can be closed by one.  A detector that stopped at a handle cell would
  miss those; this one traverses through them.

### 7.4 The rule, and where it does not apply

**Where a back-reference is incidental, store the current value — an immutable
snapshot — instead of the mutable, and make taking that snapshot an explicit
operation at the use site.**  This is the Clojure distinction between a
reference and `@ref`, which protoClojure already ships: the reader says, at the
point of reading, that it wants a value and not a cell.  An incidental
back-reference is one whose purpose is identification or diagnosis rather than
observation of later writes — protoPython's `co_name → fn` was a diagnostic
pointer, and removing it removed the cycle.

**Where the back-reference is the point of the program, the rule does not
apply and the retention stands.**  Three shapes where a snapshot would be the
opposite of what the code asked for:

* **A captured `var` that refers to itself.**  protoScala compiles a local
  captured by a closure to `MAKE_CELL`, a protoCore mutable
  (`protoScala/src/runtime/ExecutionEngine.cpp:962-964`) — it has to be, because
  sharing a `var` between closures is exactly what it is for.  So
  `var f: () => Unit = null; f = () => f()` — an ordinary recursive lambda
  defined through a `var` — is a two-handle cycle: the cell's current value is
  the closure, and the closure captured the cell.  A snapshot would freeze the
  cell at `null` and the program would be wrong.  This is the shortest and
  probably the most frequent instance in the family.
* **A genuinely cyclic object graph**: a doubly-linked list of mutable nodes, a
  graph with back-edges.
* **Two actors that reference each other**, which is how they talk.

For all three the honest statement is: the cycle's cells are retained for the
life of the space, the amount is bounded by the cycle, and the way to bound it
further is to bound how many such cycles the program creates — not to pretend
the collector will take them back.

#### Two fixes that were considered and rejected

Recording them, because a reader who does not see them will propose one of them.

* **An ephemeron pass in mark** — treat the table as weak in the key and iterate
  to a fixpoint, so an entry whose handle is unreachable stops marking its
  value.  Rejected.  It adds a **fixpoint to a concurrent mark phase**, and the
  failure mode on this side of the collector is not a leak, it is a
  **use-after-free**: marking runs with the world going
  ([GarbageCollector.md](GarbageCollector.md) § "Concurrent Mark Without
  Barriers"), and a pass that can decide *not* to mark something, iteratively,
  against a graph mutators are still reading, trades a bounded leak for a
  corruption risk.  That is the wrong direction for this kernel.
* **Splitting assignment by destination** — a field inside mutable state stores
  a snapshot, while a local variable stores the handle.  Rejected, twice over.
  It makes assignment **referentially non-uniform**: `x = y` would alias or
  freeze depending on what `x` is, which is not a property a reader of the code
  can see.  And it would **silently freeze legitimate cyclic structures** — the
  doubly-linked list, the graph with back-edges, the two actors — turning a
  bounded, measurable retention into **stale reads**.  A wrong answer is worse
  than retention.

### 7.5 The detector

`ProtoSpace::findMutableCycles(ProtoContext*, unsigned long cellBudget = 0)`
(`headers/protoCore.h:2245`, implementation `core/MutableCycles.cpp`) is a public
diagnostic, so a runtime does not need a conformance Host adaptor to ask the
question:

```cpp
const proto::MutableGraphReport r = space.findMutableCycles(ctx);
if (!r.cycles.empty()) std::cerr << r.summary();
```

**It is exact, not heuristic.**  The mutables table enumerates every written
handle in the space — there is nowhere for a mutable to hide — and every cell
field is `const` after construction, so the only edge in the whole heap that can
close a loop is the handle → state indirection the table implements.  The scan
therefore builds the **augmented cell graph** (each cell's ordinary references,
plus one synthetic edge `H_r → V_r` per handle with an entry) and runs one
Tarjan pass: a cycle in that graph is a cycle among mutables, and conversely.
O(cells + references), and the strongly connected component itself names the
participating handles.

It reports **cycles only** — a self-edge, or a component of two or more — and
names the path, because a cycle with no path is not actionable:

```
mutable-cycle scan: 10 handles in the mutables table, 12 references to a mutable handle, 38 cells walked, complete; 3 cycle(s)
  CYCLE refs {2,3}: #2 -[.cellValue]-> #3 -[.capturedCell]-> #2
  CYCLE refs {4}: #4 -[.f_locals]-> #4
  CYCLE refs {5,6,7}: #5 -[.__bases__]-> #6 -[.__subclasses_list__ > ListSmall]-> #7 -[.owner]-> #5
```

It allocates no `Cell`, so it cannot add a handle or trigger a collection while
it walks, and it holds a `ProtoContext::CriticalSection` for the duration — a
thread inside one does not park, so no new stop-the-world can begin and no new
sweep can free a cell under the scan.  The corollary is that it **stalls the
collector**, so it belongs at a quiescent point in a test or a diagnostic build,
not in a hot path.  `MutableGraphReport::truncated` says the cell budget ran out:
a cycle reported by a truncated scan is still real, but the **absence** of
cycles is not established, and `acyclicAndComplete()` is the only reading that
means "clean".

Conformance rule 13 in [EMBEDDER-CONFORMANCE.md](EMBEDDER-CONFORMANCE.md) makes
this normative for every embedder, and it turns on a **declaration verified
against a measurement** rather than on "no cycles": a runtime declares how many
structural cycles it has, and the case fails on the ones it did not declare.

### 7.6 Recorded history

protoPython's run-time function objects were once all immortal through
`fn → __closure_frames__ → frame → co_name → fn` — a cycle between two
mutables, of the *incidental* kind, since `co_name` was a diagnostic pointer.
The cost is recorded by protoPython's own work as roughly 62 marked cells per
function object; that figure is quoted here, not re-measured.  protoPython broke
the cycle on its side.

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
