# The process-global mutable table

Status: implemented on `feature/global-mutable-table` (2026-09-29).
Decision: the author, 2026-09-29 ("tabla global con ciclos serializados").

## Problem

A mutable object does not hold its state in its own cell. The cell carries a
`mutable_ref`, and the current state lives in a table of 256 shards, each an
immutable sparse list published by compare-and-swap. Until this change every
`ProtoSpace` had its own table and its own ref counter, both starting at 1.
In a process with two spaces, an object of space A read through a context of
space B was looked up in B's table: it answered the state of B's object with
the same number, or the object's birth state, and never an error. Writes from
B went into B's table and were invisible to A.

## Design

1. **One table per process.** `globalMutableTable()` (core/MutableTable.cpp)
   holds the 256 shard roots. Every read, write, snapshot and release goes
   through it. `ProtoSpace::mutableRoot` stays in the class only to keep the
   layout (ABI 3); nothing reads it.
2. **Refs are unique in the process.** Each space receives a process-unique
   id at construction and its counter starts at `id << 40 | 1`, so a ref names
   its space in the high bits and `generate_mutable_ref` is unchanged (one
   `fetch_add` on the space's own counter). The first space of a process has
   id 0, so a single-space process uses exactly the refs it used before.
   A space may create 2^40 mutables; the 2^40-th creation aborts with a
   message instead of spilling into the next space's range.
3. **Every space marks the whole table.** Phase 2 snapshots the global shard
   roots, as before; the mark phase traces every state in the table, whichever
   space allocated it. That is the extra work, and it runs in the concurrent
   mark phase, not in the pause.
4. **Collection cycles are serialized in the process.** A cycle, from the
   stop-the-world request to the end of the bulk unmark, holds a process-wide
   cycle token. Two markers never run at once, so the one mark bit per cell is
   never shared between collectors.
5. **The pause is cooperative across spaces.** A cycle's stop-the-world raises
   the stop flag of every live space and waits for each quorum: every thread
   of every space parks at a safepoint, outside any critical section. At the
   snapshot no thread of any space is in the middle of a table operation, which
   is the property that lets sweep free the table nodes it no longer sees.
6. **The cache epoch is process-wide.** The per-thread attribute and mutable
   caches are cleared when any cycle's stop-the-world completes, not only the
   thread's own space's, because a sweep in any space may free and reuse an
   address a cached entry names.
7. **A destroyed space's entries are purged.** `~ProtoSpace` records its id;
   the next cycle of any live space removes that id's entries from the table
   in Phase 5b, allocating through the collecting space's own context.

With one space in the process, 2, 4, 5 and 6 reduce to the previous
behaviour: the id is 0, the token is never contended, the only quorum is the
space's own, and the process epoch advances with the space's cycles.

## Limitations (documented, not solved)

- **Values held outside mutable state.** A space keeps alive what its own
  roots and the global table reach. If space B holds a cell of space A in B's
  roots (a context slot, a pin, B's own immutable structures) and nothing of
  A's reaches it, A may free it. Interop is sound for objects shared through
  mutable state; a runtime that hands another runtime's cells to its own
  structures must keep them reachable in the owning space as well.
- **Table nodes of a destroyed space.** Tree nodes allocated by a space that
  is later destroyed may stay shared in paths of live entries. No space sweeps
  them, so they are retained. This is safe because `~ProtoSpace` never frees
  cell blocks; that property must be kept while the table is global.
- **Coupled pauses.** With several spaces, every cycle pauses all of them, and
  one space's collection waits for another's to finish. Pauses stay short
  (mark and sweep are concurrent), but a program with many spaces pays one
  pause per cycle of any space.
