# Architecture Overview: The Mutability Model

This overview summarizes how protoCore supports mutable objects on top of immutable cells. [DESIGN.md](../../../DESIGN.md) §§ 3–4 and the dated design document [MUTABLE_SHARDING_AND_CACHE_REFACTOR.md](../../MUTABLE_SHARDING_AND_CACHE_REFACTOR.md) give the details.

## Identity and State

Every heap cell in protoCore is immutable after construction. An immutable object keeps its attributes in its own cell, and "changing" it returns a new object. A mutable object, created with `newObject(true)` or `newChild(context, true)`, is instead an identity: its cell carries a `mutable_ref` number, taken from a counter in `ProtoSpace`, and the cell itself never changes.

The current state of a mutable object, an immutable object holding its attributes, is stored outside the cell, in the space's `mutableRoot` table. Updating a mutable object publishes a new state for its `mutable_ref`; the object's cell is not written.

## The Sharded `mutableRoot`

`mutableRoot` has `MUTABLE_ROOT_SHARDS = 256` shards, and a mutable object belongs to shard `mutable_ref % MUTABLE_ROOT_SHARDS`. Each shard slot holds a `std::atomic<ProtoSparseList*>` and is padded to 64 bytes, so neighbouring slots do not share a cache line. The sparse list maps each `mutable_ref` in the shard to its current state.

## Compare-and-Swap Updates

An update such as `setAttribute` on a mutable object:

1. loads the current root of the object's shard;
2. builds a new sparse list containing the new state, sharing the unchanged parts of the old one;
3. publishes the new root with `compare_exchange_weak`;
4. starts again from step 1 if another thread published a change to the same shard in the meantime.

Threads that update objects in different shards do not conflict. For an attribute-level compare-and-set, read the current value with `getOwnAttributeDirect` and write with `setAttributeIfEqual` in a retry loop.

This lock-free path covers updates of mutable objects. Other runtime structures, such as the thread list and the collector's bookkeeping, are protected by `ProtoSpace::globalMutex`.

## Reading Mutable State

Each thread keeps a 1024-entry mutable value cache (`MUTABLE_VALUE_CACHE_DEPTH`). An entry records a `mutable_ref`, the shard root seen when the entry was filled, and the resolved state. A lookup loads the shard root again; if it is still the same pointer, the cached state is returned without searching the sparse list. A successful update to the shard installs a new root pointer, which invalidates every cached entry for that shard on its next lookup, so threads need no invalidation messages. The collector traces the cache entries, so a cached shard root cannot be freed and its address reused while the entry exists.

## Interaction with the Garbage Collector

Every change to mutable state goes through a shard root, so the 256 shard roots describe all mutable state in the system. During the stop-the-world phase the collector copies them into a per-cycle snapshot (`gcMutableSnapshot`), then marks from that snapshot while application threads continue to update the live table. No write barrier is needed. See [the garbage collector overview](01_garbage_collector.md) and [GarbageCollector.md](../../GarbageCollector.md).
