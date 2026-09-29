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
3. **Every space marks the whole table.** Phase 2 snapshots the global shard
   roots; mark traces every state, whichever space allocated it. The extra
   work is in the concurrent mark phase, not in the pause.
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

## Limitations (documented, not solved)

- **Cells of one space held in another space's roots.** A space keeps alive
  what its own roots and the global table reach. If a thread of space B keeps
  a cell of space A across a safepoint in B's own roots (a context slot, a
  pin, B's own immutable structures) and nothing of A reaches it, A may free
  it. The grace period covers cells held in C++ locals between safepoints,
  and the table covers cells reachable from mutable state; a runtime that
  keeps another runtime's cells in its own structures must also keep them
  reachable in the owning space, for example through a mutable object of
  that space.
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

## Tests

`test/MultiSpaceThreadTests.cpp` (shared thread, unmanaged regions, cycle
serialization) and `test/GlobalMutableTableTests.cpp` (cross-space reads and
writes, states allocated in another space, unique refs, purge, a value held by
another space's thread until its safepoint, and a stress run of registered
threads of two spaces sharing mutables under collection).
