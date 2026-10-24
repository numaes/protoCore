# Several ProtoSpaces in one process: the global mutable table

Status: implemented on `feature/global-mutable-table` (2026-09-29).
Decision: the author, 2026-09-29 ("tabla global con ciclos serializados";
"hacer completo").

## Problem

A mutable object does not hold its state in its own cell. The cell carries a
`mutable_ref`, and the current state lives in a table of 256 shards, each an
immutable sparse list published by compare-and-swap. Until this change every
`ProtoSpace` had its own table and its own ref counter, both starting at 1.
In a process with two spaces, an object of space A read through a context of
space B was looked up in B's table: it answered the state of B's object with
the same number, or the object's birth state, and never an error. Writes from
B went into B's table and were invisible to A.

Making the table global exposed three further problems of multi-space
processes, which the design below also solves:

- the thread that constructs a space is that space's adopted main thread and
  counts in its stop-the-world quorum; a thread that builds two spaces is a
  member of both, but it only answered the stop-the-world of the space whose
  code it was running, so a collection of the other space waited for it
  indefinitely;
- two spaces could collect at the same time, and a marker that reaches the
  other space's cells shares their single mark bit with that space's
  collector;
- a sweep frees a dead cell at once and rewrites its header, while a thread
  of another space, not stopped by this space's stop-the-world, may still be
  reading it (a table node, a state, a value).

## Design

1. **One table per process.** `globalMutableShards` (core/MultiSpace.cpp)
   holds the 256 shard roots, constant-initialized; the first space installs
   an empty perennial list in each. Every read, write, snapshot and release
   uses it. `ProtoSpace::mutableRoot` stays in the class only to keep the ABI
   3 layout; nothing reads it.
2. **Refs name their space.** Each space gets a process-unique id at
   construction and its counter starts at `id << 40 | 1`
   (`kMutableRefSpaceShift`), so `generate_mutable_ref` is still one
   `fetch_add` on the space's own counter. The first space of a process has
   id 0 and keeps the refs it always had. A space that creates 2^40 mutables
   aborts with a message instead of spilling into the next id.
3. **Every space reads the whole table.** Phase 2 snapshots the global shard
   roots. Since 3.1.0 the states are not roots ("States are not roots"
   below): mark marks the table's own nodes, and a state only when it
   reaches the state's handle, whichever space allocated either. Until
   then mark traced every state of the table. The work is in the
   concurrent mark phase, not in the pause.
4. **A thread shared by several spaces behaves as one thread**
   (`multispace::parkForAnyStop`, `goOut`, `comeBack`). Membership is per OS
   thread: the spaces it constructed and the space whose `newThread` started
   it. At any safepoint it answers the stop-the-world of any member space; an
   unmanaged region or a heap-headroom wait takes it out of every member
   quorum (never out of a space in whose critical section it is). The
   allocation poll reads one process-wide counter, `multispace::attention`,
   instead of its own space's flag.
5. **Collection cycles are serialized.** A cycle holds a process-wide token
   from its stop-the-world request to the end of its bulk unmark, so two
   markers never run at once.
6. **Dead cells wait for a grace period when other spaces are live.** The
   sweep only collects them. After the token is released, the collector waits
   until every registered thread of the process (adopted main threads, threads
   started by `newThread`, and collectors inside Phase 5b) has passed a
   safepoint outside any critical section, or is parked or out of its quorum.
   That is the point where the stop-the-world already assumes a thread holds
   no cell reachable only from C++ locals, so no table operation needs to
   announce itself and nothing is added to the lookup paths. Only then are the
   dead cells finalized and returned to the freelist, and Phase 5b releases
   the entries of the finalized handles. No thread is stopped.
7. **The cache epoch is process-wide.** `multispace::gcEpoch` advances at
   every cycle's stop-the-world, whatever its space. A thread clears its
   attribute and mutable caches at its first quiescent point after a new
   epoch; the grace period guarantees that happens before any freed address
   is reused.
8. **A destroyed space's entries are purged.** `~ProtoSpace` records its id;
   the next Phase 5b of any live space releases every ref carrying it.

With one space in the process, 2, 4, 5, 6 and 7 reduce to the previous
behaviour: the id is 0, the only member space is the space itself, the token
is never contended, the grace period is skipped (dead cells are freed in
place), and the process epoch advances with the space's cycles.

## States are not roots (3.1.0)

Decision: the author, 2026-10-24. Until then every state in the table was a
root of the cycle that snapshotted it, whether or not anything still
referenced its handle, and an entry left the table only after its handle had
been swept. A chain of mutable objects in which each state points at the next
handle was therefore freed one link per cycle, and a cycle among mutable
objects was never freed.

**Rule.** A state is live only while the mark reaches its handle. When the
mark reaches a mutable handle (`Marker::scan`, core/ProtoSpace.cpp), it marks
the state that the handle's ref names in the table snapshot taken at the
pause (`gcMutableSnapshot`), never in the live table. The shard trees' own
nodes are marked without their values. A dead chain or cycle of mutable
objects is then entirely unmarked: every finalizer runs in the same cycle,
the handles' entries leave the table in that cycle's Phase 5b, and the chain
is freed in one cycle.

**Why it is sound without a barrier.**
- The table is immutable and the mark reads the pause snapshot, so the graph
  it walks (cells, plus one edge from each handle to its snapshot state) is
  frozen. Every state live at the pause is reached through a handle live at
  the pause. A write after the pause publishes a state built after the
  pause, which is young; a handle created after the pause is absent from the
  snapshot and is young too, and so are its states.
- A handle held only by a live context's young generation, or by the
  survivor pen on a non-fold cycle, is scanned like every young or pen cell,
  so its state is marked.
- Between the sweep and Phase 5b, an entry whose handle the sweep found dead
  names a freed state. Nothing can read it through the handle: a handle that
  no root reaches can never be obtained again. The walkers of the whole table
  do not read values: the node marking and the index build read keys and
  value pointers only, the purge of a destroyed space's refs reads keys, and
  `findMutableCycles`, which does start from the values, waits until no cycle
  is between its stop-the-world and the end of its Phase 5b
  (`multispace::mutableReleasesPending`).
- Without survivor re-inclusion (`PROTOCORE_GC_REINCLUDE_SURVIVORS=OFF`) a
  handle that survived a cycle is never swept again, so its entry would
  never be released and would name a freed state for ever. That
  configuration keeps the shard roots as roots, as before.

**Several spaces.** A cycle that starts with more than one live space in the
process keeps the table's states as roots, as before 3.1.0. A state is a cell
of the space whose context wrote it, and a handle is reached only by its own
space's roots: when a context of space B writes the state of a handle of space
A, only A reaches the handle, and a B cycle that did not root the states would
free a state of a live handle (shown by
`MutableStatesNotRoots.StateWrittenByAnotherSpaceLivesWhileItsHandleLives`).
The rule is chosen per cycle, at the pause (`multispace::liveSpaceCount()`):
with one space, states are not roots; with several, they are. A cycle that
roots the states reads every value of its snapshot, so it must not snapshot an
entry whose state an earlier cycle has freed: its token wait also waits until
no cycle that did not root the states is before its Phase 5b
(`multispace::mutableReleasesPending`). That is only ever the case when a space
was created during such a cycle, whose release then follows the grace period.
With several spaces live, chains of mutable objects are therefore freed one
link per cycle again, and cycles among them are retained, until the process is
back to one space.

**Implementation.** Before the trace, one walk of the snapshot's shard trees
builds an open-addressing index of the entries by ref (`MutableStateIndex`,
`indexSnapshot`), reading keys and value pointers only; the edge then costs one
probe, and the index slot is prefetched when the handle is shaded. After the
trace, a second walk marks the nodes no ordinary path reached, without their
values (`markTableNodes`). A node that an ordinary path did reach -- a young
path copy of a shard tree, an older version held by a live context's young
generation, reaches snapshot nodes -- was traced like any other cell, values
included: conservative, bounded by the young generation. Marking the nodes
before the trace instead, which stops those paths at the snapshot, was tried
and measured worse: protoJS `cli/promise-rejections-bounded-memory` and
protoPython `protopyc_gc_rooting` ran out of memory under their 1,500,000-cell
ceilings, with several times the live set, on every run; the mechanism was not
established, so the conservative order is the one kept.

**Cost.** The index is 32 bytes per table entry at most (two slots per entry,
power-of-two capacity), outside the cell heap, kept by the collector thread
for the next cycle. Measured with 500,000 live mutable objects held by a root
(a scratch benchmark outside the repository, five interleaved runs, medians of
nine cycles each, AMD Ryzen 5 5500U): the mark phase (resume to sweep start)
took 286.0 ms against 214.9 ms before (+33 %), the whole cycle 597.2 against
527.8 ms (+13 %), for 4.5 M marked cells in both; with the same handles linked
in one chain from a root, 325.2 against 203.2 ms (+60 %) and 602.0 against
476.7 ms (+26 %). At 50,000 handles: +27 % / +39 % mark, +10 % / +17 % cycle.
The chain is the costly shape: the old trace reached each state from its table
node, in table order, with many independent misses in flight; now a linked
chain is followed handle by handle, one dependent miss after another, as an
immutable linked list would be.

**Measured on the chain.** 300,000 protoClojure atoms, each pointing at the
next, only the newest held, under a 1,000,000-cell ceiling: 461 -> 14 cycles,
100.6 s -> 0.51 s, live set after the last cycle 1,802,173 -> 26,591 cells,
maximum resident set 157 -> 77 MB (medians of three runs each, base and new
interleaved for the base figures).

**Consequence for mutable cycles.** A cycle among mutable objects that
nothing reaches is now collected like any other garbage. A reachable one is
live data. `findMutableCycles` still reports the cycles present in the table;
[MemoryModel.md](MemoryModel.md) section 7 and conformance rule 13 describe
the earlier property.

## Limitations (documented, not solved)

- **Cells of one space held in another space's roots.** A space keeps alive
  what its own roots and the global table reach. If a thread of space B keeps
  a cell of space A across a safepoint in B's own roots (a context slot, a
  pin, B's own immutable structures) and nothing of A reaches it, A may free
  it. The grace period covers cells held in C++ locals between safepoints;
  a runtime that keeps another runtime's cells in its own structures must
  also keep them reachable in the owning space, from that space's roots.
  Since 3.1.0 the table's states are not roots, so a mutable object keeps
  them only while the owning space reaches its handle.
- **Threads that never reach a safepoint.** A registered thread that runs
  without reaching a safepoint delays another space's reclamation, as it
  already delays its own space's stop-the-world. Unregistered threads (a
  `ProtoContext` on a raw `std::thread`, EMBEDDER-CONFORMANCE rule 11) take
  no part in grace periods, exactly as they take no part in the stop-the-world.
- **Table nodes of a destroyed space.** Tree nodes allocated by a space that
  is later destroyed may stay shared in paths of live entries. No space
  sweeps them, so they are retained. This is safe because `~ProtoSpace` never
  frees cell blocks; that property must be kept while the table is global.
- **Collections wait for each other.** One space's cycle waits for another's
  to finish, and a multi-space cycle waits for the grace period before its
  freed cells can be reused. Pauses themselves are unchanged.
- **With several spaces live, the states are roots** ("Several spaces"
  above): chains of mutable objects are freed one link per cycle and cycles
  among them are retained while more than one space is live. Measured
  before 3.1.0: 300,000 protoClojure atoms in a chain took 461 cycles and
  101 s under a 1,000,000-cell ceiling.

## Tests

`test/MultiSpaceThreadTests.cpp` (shared thread, unmanaged regions, cycle
serialization) and `test/GlobalMutableTableTests.cpp` (cross-space reads and
writes, states allocated in another space, unique refs, purge, a value held by
another space's thread until its safepoint, and a stress run of registered
threads of two spaces sharing mutables under collection).

States are not roots: `test/MutableStatesNotRootsTests.cpp` (a live chain held
by a root loses nothing; a handle reached only through another mutable's state
stays live; a young handle keeps its state; an unreachable mutable cycle is
freed in one cycle and its entries released; a chain alternating handles of
two spaces is kept while held and freed when dropped; a state written by
another space), `ProtoLazyObject.MutableChainIsFreedInOneCycle` (3,000 links
freed by one cycle; none on the previous code) and
`MutableCycleDetector.UnreachableCyclicMutablesAreReleased` /
`ReachableCyclicMutablesAreKeptAndReported`.
