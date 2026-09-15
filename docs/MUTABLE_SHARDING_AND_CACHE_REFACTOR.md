# Mutable Object Refactor: Wider Sharding + Per-Thread Value Cache

**Status:** Implemented (April 2026). Sections 1–9 record the design as written before implementation, when `mutableRoot` had 16 shards; section 10 records the results.
**Author:** core team
**Date:** 2026-04-25
**Scope:** `protoCore` only. **The public API does not change.**

---

## 1. Problem Statement

`protoCore` represents mutable objects with two pieces:

1. An immutable `ProtoObjectCell` carrying a unique `mutable_ref` id (assigned at first mutation).
2. A side table — `ProtoSpace::mutableRoot` — mapping `mutable_ref → currentSnapshot` (a `ProtoSparseList`, AVL-backed).

Every read of a mutable object resolves through the side table:

```
load shard root  →  AVL lookup by mutable_ref  →  current snapshot
```

In `protoPython` essentially every user-level object is mutable
(modules, classes, instances, dicts, lists). The AVL traversal becomes
the hottest path of the runtime. The original proposal was to shard the
side table into 256 roots selected by `id % 256`, and to add a per-thread
cache of each mutable's current value, similar to the attribute cache, so
that the most common case (a mutable accessed by its own thread) becomes
an O(1) operation.

---

## 2. Current State (baseline)

A careful audit of the codebase shows that **part of the proposal is already
implemented**. We must base the refactor on what is, not on what we believed
to be.

### 2.1 Sharding — already present

`headers/protoCore.h:850-851`

```cpp
static constexpr int MUTABLE_ROOT_SHARDS = 16;
std::atomic<ProtoSparseList*> mutableRoot[MUTABLE_ROOT_SHARDS];
```

- 16 independent shards, indexed by `mutable_ref % 16`.
- Each shard is updated via `compare_exchange_weak` with bounded retry
  (≤100 iterations). Used in 14 sites in `core/ProtoObject.cpp` (lines
  235, 333, 444, 499, 541, 744, 1001, 1069, 1113, 1149, etc.).
- All shards are pre-seeded with empty `ProtoSparseList` at space creation
  (`core/ProtoSpace.cpp:401-405`).
- The GC root-scan walks all shards (`core/ProtoSpace.cpp:243-244`):

```cpp
for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
    if (auto* r = space->mutableRoot[s].load())
        addRootObj(reinterpret_cast<const ProtoObject*>(r));
}
```

### 2.2 Per-thread cache — only for *attribute* lookups

`core/Thread.cpp:42-49` and `headers/proto_internal.h:928-938`:

```cpp
struct AttributeCacheEntry {
    const ProtoObject* object;
    const ProtoObject* result;
    const ProtoString* name;
};
// Per-thread, sized THREAD_CACHE_DEPTH (= 1024 entries).
```

This cache accelerates **attribute resolution on a fully resolved object
snapshot**. It does *not* short-circuit the *snapshot resolution itself*.
Every attribute lookup against a mutable object still pays:

```
load mutableRoot[shard]   ← atomic read (fast)
implGetAt(mutable_ref)    ← AVL traversal, O(log N_shard)
```

**This is the cost the refactor targets.**

### 2.3 What is missing

| Piece                                                             | Present? |
|-------------------------------------------------------------------|----------|
| Sharding of `mutableRoot`                                         | ✅ (16)  |
| Lock-free CAS updates per shard                                   | ✅       |
| GC root-scan of every shard                                       | ✅       |
| Per-thread cache of *attribute* result                            | ✅       |
| **Per-thread cache of *snapshot* (mutable_ref → current value)**  | ❌       |
| Sufficient shard count for ≥ 64-core machines                     | ❌ (16)  |
| False-sharing protection between shard atomics                    | ❌       |

---

## 3. Refined Strategy

The proposal is sound; this section refines it on three axes: shard count
justification, cache validation protocol, and GC interaction.

### 3.1 Shard count: 16 → 256

**Why bump the count.** Two effects compound:

1. **Contention.** With S shards and W concurrent writers uniformly
   distributed, expected contention per shard is W/S. At S=16 and a 32-core
   protoPython workload mutating modules/classes/instances, every shard sees
   ~2 active writers. CAS retry rates climb. At S=256, contention falls
   below 1 in expectation across modern many-core hosts.
2. **AVL depth.** Each shard's `ProtoSparseList` has depth ≈ log₂(N_shard).
   For a working set of 1 M live mutables:
   - S=16  → 62 500 entries/shard → depth ≈ 16
   - S=256 → 4 000  entries/shard → depth ≈ 12
   AVL traversal is the dominant cost on cache-miss reads. A 25 % depth
   reduction is meaningful on the hot path.

256 is chosen because:
- It is a power of two (`mutable_ref & 0xFF` instead of modulo).
- It fits in one byte for hashing/diagnostic purposes.
- Per-shard cache footprint stays small (one atomic pointer + padding).

**Cost.** GC adds 240 extra atomic loads at STW (240 × ~2 ns ≈ 0.5 µs).
Negligible.

### 3.2 Per-thread mutable-value cache

A thread-local hash table that short-circuits the
`load + AVL` sequence on the common own-thread path.

#### 3.2.1 Entry layout

```cpp
struct MutableValueCacheEntry {
    unsigned long       mutable_ref;   // 8B  — key
    ProtoSparseList*    shard_root;    // 8B  — root pointer at the time we cached
    const ProtoObject*  current_value; // 8B  — resolved snapshot
};
// 24 B per entry, identical to AttributeCacheEntry.
```

Entries live inside `ProtoThreadExtension` (already a Cell, already
lifecycled with the thread, already scanned by GC).

#### 3.2.2 Sizing

`MUTABLE_VALUE_CACHE_DEPTH = 1024` initially, mirroring
`THREAD_CACHE_DEPTH`.
- 1024 × 24 B = 24 KB per thread (fits in L1).
- Direct-mapped with `mutable_ref & (DEPTH - 1)`.
- Conflict misses are *correctness-safe*: a miss falls back to the
  authoritative shard load.

#### 3.2.3 Lookup protocol

```
read_mutable(ref):
    idx = ref & (CACHE_DEPTH - 1)
    e = cache[idx]

    if e.mutable_ref == ref:
        // Validate: the shard root must be unchanged since we cached.
        shard = ref & (NUM_SHARDS - 1)
        live_root = mutableRoot[shard].load(acquire)
        if live_root == e.shard_root:
            return e.current_value         // O(1) HIT

    // Miss path: authoritative resolve.
    shard = ref & (NUM_SHARDS - 1)
    live_root = mutableRoot[shard].load(acquire)
    snap = live_root->implGetAt(ref)
    cache[idx] = { ref, live_root, snap }
    return snap
```

#### 3.2.4 Mutation protocol (own thread)

```
write_mutable(ref, new_state):
    shard = ref & (NUM_SHARDS - 1)
    do:
        old_root = mutableRoot[shard].load(acquire)
        new_root = old_root->implSetAt(ref, new_state)
    while not mutableRoot[shard].CAS(old_root, new_root)

    idx = ref & (CACHE_DEPTH - 1)
    cache[idx] = { ref, new_root, new_state }     // refresh, no invalidation needed
```

#### 3.2.5 Cross-thread mutation

Thread A writes; thread B reads:

1. Thread A successfully CASes shard root from R₀ to R₁.
2. Thread B's cached entry holds `(ref, R₀, v₀)`.
3. Thread B's lookup reloads `live_root = R₁ ≠ R₀` → miss.
4. Thread B re-resolves through the new R₁ and refreshes its own cache.

No coordination, no signaling, no invalidation broadcast.
**This is the central property of the proposal.**

### 3.3 ABA, lifetime and GC interaction

#### 3.3.1 ABA on shard roots

Could shard root cycle R₀ → R₁ → R₀? No: each `implSetAt` returns a
**newly allocated** `ProtoSparseListImplementation` (immutable structural
sharing — `ProtoSparseList` is built on AVL persistent rewriting).
Therefore pointer equality on the shard root implies content equality.

#### 3.3.2 Lifetime of cached `shard_root`

A cached pointer must remain valid as long as it sits in the cache. Two
threats:

- **GC frees the old root.** Every cache entry transitively keeps the
  root alive only if the GC scans the cache.
- **Use-after-free during validation.** If GC reclaimed the cell, the
  comparison against `live_root` is technically undefined.

**Resolution:** treat the `MutableValueCacheEntry::shard_root` and
`current_value` as GC roots, like the `attributeCache` entries.

This design first extended `ProtoThreadExtension::processReferences` to
iterate the new cache. That was sound while mark ran under stop-the-world.
Once mark became concurrent (May 2026), the marker read cache slots that
the owning thread was rewriting, and a slot read twice could yield a null
work-list entry (the September 2026 crash in `gcThreadLoop`). Both caches
are now scanned as roots in the stop-the-world Phase 2 of `gcThreadLoop`
(`scanThreadCaches` in `core/ProtoSpace.cpp`), while every owner is parked,
and `ProtoThreadExtension::processReferences` reports nothing. An entry
written after that snapshot holds cells the thread obtained after it, which
the snapshot or a young generation already protects.

This means:
- Every `shard_root` cached at the snapshot is marked, so old roots
  *that are still cached* survive the cycle even after a CAS replaced them
  in the side table. Memory cost is bounded by the working-set hot data,
  not the entire heap.
- Sweep cannot free a pointer that is cached when a later lookup compares
  it; comparison is always defined.

#### 3.3.3 Invalidation at STW boundaries

Optional optimisation: at start of STW, walk every thread cache and zero
it. This frees the GC from tracing stale roots and shortens the next mark.
Recommended **only after measurement**; the safe default is to scan as
roots.

### 3.4 False-sharing protection

The 16 atomics in the current layout occupy a single cache line. Concurrent
CASes on different shards bounce that line between cores. We pad shards
to 64 B:

```cpp
struct alignas(64) ShardSlot {
    std::atomic<ProtoSparseList*> root;
    char _pad[64 - sizeof(std::atomic<ProtoSparseList*>)];
};
ShardSlot mutableRoot[MUTABLE_ROOT_SHARDS];
```

Cost: 256 × 64 B = 16 KB per `ProtoSpace`. Trivial.

### 3.5 Choice of indexing function: modulo vs mask

`mutable_ref % 256` is a `divq` on x86; `mutable_ref & 0xFF` is a `andq`.
We use the mask form. Distribution: `nextMutableRef` is a global
monotonic counter; low bits cycle through every shard index in order — a
*better* distribution than user-controlled hashes, since it guarantees
uniform shard population over time.

### 3.6 Memory ordering

- Reads of `mutableRoot[shard]`: `memory_order_acquire` — pairs with the
  release in CAS to publish the new root *and* its newly written
  AVL nodes.
- CAS in writes: `memory_order_acq_rel`.
- Cache reads/writes are thread-local: relaxed ordering.

---

## 4. Public API Stability Proof

The refactor touches only the implementation. Every change is *behind*
public surfaces.

| Public surface                                              | Change?            |
|-------------------------------------------------------------|--------------------|
| `protoCore.h` ProtoObject methods                           | None               |
| `protoCore.h` ProtoSpace methods                            | None               |
| `protoCore.h` `MUTABLE_ROOT_SHARDS` value                   | 16 → 256           |
| `protoCore.h` `mutableRoot` field type                      | Array of `ShardSlot` (same observable behaviour: still atomic pointer) |
| ABI: callers using `ProtoObject::setAttribute`, etc.        | Unchanged          |
| ABI: callers using `ProtoSpace::MUTABLE_ROOT_SHARDS` value  | None — protoPython, protoJS never index this field directly |
| `proto_internal.h` `AttributeCacheEntry`                    | Unchanged          |
| `proto_internal.h` new `MutableValueCacheEntry`             | Internal-only      |
| `ProtoThreadExtension` layout                               | Adds one pointer; internal-only |

**Verification step (part of the implementation plan, not done now):**
`grep -r MUTABLE_ROOT_SHARDS` across `protoPython/` and `protoJS/`
to confirm the constant is not used outside `protoCore`. If
any external code reads it, we keep `MUTABLE_ROOT_SHARDS` as a
`constexpr` whose *value* changes — source-compatible, ABI-compatible
because it is inlined at every callsite.

---

## 5. Performance Model

Hot path: read of a mutable object inside a tight loop, own thread.

| Step                              | Before     | After (cache hit) |
|-----------------------------------|------------|-------------------|
| `mutableRoot[shard].load`         | 1 atomic   | 1 atomic          |
| AVL `implGetAt`                   | O(log N_s) | —                 |
| Cache validate / payload load     | —          | 2 word-loads      |
| **Total**                         | ~25-60 ns  | ~5-8 ns           |

Cache miss (rare, after foreign mutation or eviction) costs roughly the
*before* number plus one extra cache-line write for the refill.

For protoPython the assumption is that >95 % of mutable reads are
own-thread reads against state that did not change between the prior
mutation and the next access. Empirical validation will be part of the
implementation phase.

Additional benefit of bumping shards to 256: even on miss paths the AVL
traversal shortens by ~25 %.

---

## 6. Migration Plan (as written before implementation)

At the time of writing, the strategy was to be analysed and refined
before any implementation, so only this design document was committed.

The planned phases were:

1. **Phase A — widen shards.** Bump `MUTABLE_ROOT_SHARDS` to 256, add
   the `ShardSlot` cache-line padding, switch `%` to `&`. Update GC
   and `ProtoSpace` initialisation. No behaviour change beyond
   contention reduction.
2. **Phase B — add the cache.** Introduce
   `MutableValueCacheEntry`, allocate per-thread, integrate into
   `ProtoThreadExtension` lifecycle and GC scan. Plumb the
   lookup/refresh into the resolution sites in `core/ProtoObject.cpp`
   (every `mutableRoot[shard].load()` site followed by
   `implGetAt(mutable_ref)`).
3. **Phase C — instrument and tune.** Hit-rate counters (debug-only,
   compiled out in release). Tune `MUTABLE_VALUE_CACHE_DEPTH` based on
   protoPython benchmarks (`test_grammar`, the standard library
   bootstrap workload).
4. **Phase D — STW invalidation experiment.** Compare scan-as-root vs
   STW-clear; pick the cheaper.

Each phase is one commit on `master`, gated by:
- protoCore test suite green (`ctest --test-dir build`).
- protoPython 54/75 baseline preserved.
- protoJS test262 baseline preserved.

---

## 7. Open Questions / Risks

1. **Cache-line pressure.** 24 KB per thread is fine for ≤ 32 threads;
   for thread-per-coroutine designs we may need either a smaller cache
   or a global-but-striped cache. Punted to Phase C measurement.
2. **GC traceability of cached snapshots.** The cache becomes a
   secondary root set. Its size (24 KB × N threads) is bounded but
   non-trivial; it could keep historically dead snapshots alive longer
   than today. STW-clear (option 3.3.3) eliminates this risk at the
   cost of a cold cache after each GC.
3. **mutable_ref reuse.** `nextMutableRef` is monotonic; `mutable_ref`
   is never reassigned during a process lifetime. No ABA at the id
   level.
4. **Stress on `implGetAt` on miss path.** AVL implementation
   correctness under heavy concurrent CAS retries already covered by
   existing concurrent-append benchmarks; no new race surface.

---

## 8. Acceptance Criteria for the Refactor

These criteria were set before implementation. This document does not
record the test runs made when the change was merged, so a criterion is
marked as met only where section 10 or the current code shows it.

| Criterion | Status |
|---|---|
| All `protoCore` tests pass (`ctest --test-dir build`) | Not recorded here; see [TESTING.md](TESTING.md) to run the suite |
| No regression in the `protoPython` test_grammar baseline | Not recorded here |
| No regression in the `protoJS` test262 baseline | Not recorded here |
| Microbenchmark: ≥ 3× speed-up on a mutable-object hot-read loop | Met according to section 10 (snapshot resolution more than 5× faster than the AVL search) |
| No new `proto_internal.h` symbol leaks into the public API | Partly: `MutableValueCacheEntry` is defined only in `headers/proto_internal.h`, but `headers/protoCore.h` forward-declares it and `ProtoContext` holds a pointer to it |
| GC stress test green at the previous load | Not recorded here; the stress tests are in `test/GCStressTests.cpp` |

---

## 9. Summary

| Aspect                | Decision                                                  |
|-----------------------|-----------------------------------------------------------|
| Shard count           | 16 → **256**, mask-indexed, cache-line padded             |
| Per-thread cache      | **New**: `MutableValueCacheEntry[1024]` per thread        |
| Validation key        | `(mutable_ref, shard_root)` — pointer equality            |
| Foreign-thread sync   | Implicit via shard-root pointer change (no broadcast)     |
| GC integration        | Cache scanned as root via `processReferences`             |
| Public API change     | **None**                                                  |
| Status                | Implemented in April 2026; see section 10                 |

The refactor delivers the O(1) own-thread access target without
adding new public surface and without disturbing the existing shard,
GC, or atomic-update machinery — it builds on them.

---

## 10. Implementation Results (April 2026)

The refactor was merged on 2026-04-25 (commit `7d3674cd`). The figures below were recorded at that time with `performance/cache_timing_benchmark.cpp`.

### Benchmark Results
| Metric | Performance | Improvement |
| :--- | :--- | :--- |
| **Mutable Snapshot Resolution** | **~2.1 ns** | **>5x faster** than AVL search |
| **End-to-End Attribute Access** | **8.7 ns** | **~40% reduction** in hot-path latency |
| **Contention (256 shards)** | **<1% CPU overhead** | — |

### Key Files
- `core/ProtoObject.cpp`: Implementation of `resolveMutableSnapshot` and cache invalidation.
- `headers/proto_internal.h`: Definition of `MutableValueCacheEntry`.
- `core/ProtoSpace.cpp`: Initialization of 256 shards with cache-line padding.

The original results table also stated linear scaling up to 128 cores. No measurement on hardware of that size is recorded, so that statement has been removed.

In the current code, the shard is selected with `mutable_ref % ProtoSpace::MUTABLE_ROOT_SHARDS` (`core/ProtoObject.cpp`), and `AttributeCacheEntry` has grown to 32 bytes while `MutableValueCacheEntry` remains 24 bytes (`headers/proto_internal.h`).

The 256-shard system provides a significant headroom for future high-concurrency workloads while the per-thread cache effectively transforms a global state lookup into a local memory operation.
