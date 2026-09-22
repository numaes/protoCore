# Changelog

All notable changes to protoCore are documented in this file.

## [Unreleased]
### Added
- **`ProtoSparseListObject`** (protoCore 1.3.0) — a persistent AVL map
  identical to `ProtoSparseList` except that its key is a `const ProtoObject*`
  the garbage collector traces: an object referenced only as a key stays
  alive. Keys are ordered and compared by their word (identity, tag included);
  embedded values (SmallInteger, booleans, chars, None) are valid keys and are
  never traced; `nullptr` is not a valid key. `getAt` returns `nullptr` for an
  absent key, so a stored `PROTO_NONE` stays distinguishable. Small inline
  form up to three entries, AVL beyond, exactly like `ProtoSparseList`, whose
  algorithms it shares through `core/SparseListAlgorithms.h`
  (`ProtoSparseList`'s behaviour, API, ABI and performance are unchanged).
  New API: `ProtoContext::newSparseListObject`,
  `ProtoObject::isSparseListObject` / `asSparseListObject`,
  `ProtoSpace::sparseListObjectPrototype`, `ProtoSparseListObjectIterator`.
  Uses one new pointer tag (27); the tag table now records the platform tag
  budget and the maintainer-approval rule. ABI change: every embedder must be
  rebuilt. Specification: protoScala/docs/platform/PSLO-SPEC.md. The
  performance gate and the ASan run are still pending on a quiet host: the
  perf gate was parked during verification because a parallel build on the
  same machine made retired-instruction counts unreliable (the counts
  themselves were flat, consistent with no regression, but the run was not
  clean enough to certify), and ASan was not run at all. Both are left for
  the maintainer's pre-merge check.
- **Hashed-collection helper** — `KeySemantics`, `hashedPut`, `hashedGet`,
  `hashedRemove`, `hashedForEach` over `ProtoSparseListObject`: identity keys
  are stored directly; value-equality keys are stored under a SmallInteger
  hash word with a flat `[k0, v0, k1, v1, …]` bucket list for collisions.
  Language callbacks run outside GC critical sections.
- **`ProtoObject::processOwnAttributes`** — walks an object's OWN attributes
  as `(name, value)` pairs, the enumeration the public API was missing.
  Attribute keys are stored as the interned symbol pointer reinterpreted as
  an integer (`getAttribute` computes `reinterpret_cast<uintptr_t>(name)`),
  so `getOwnAttributes()` returns a sparse list whose keys are opaque
  numbers: an embedder could read an object's own values but could not
  recover their names. That is what blocked protoJS from copying or stamping
  an object's own attributes. The callback receives the canonical symbol for
  each name, so it compares equal by identity to the symbol the attribute was
  set with, and `getAttribute` on it returns the value the callback was
  handed. Casting the stored key back to a symbol is sound because symbols
  are perennial: interned with a null `ProtoContext`, allocated outside the
  GC, never marked, swept, moved or evicted.

  The walk resolves the mutable snapshot exactly as `getAttribute` does,
  visits own attributes only (never the prototype chain), reports a
  `PROTO_NONE` value like any other and a removed attribute not at all,
  allocates nothing (`ProtoContext::allocatedCellsCount` is unchanged by it),
  and runs the callback OUTSIDE any GC critical section — so the callback may
  allocate, reach a safepoint and run arbitrary embedder code. For the
  duration of the walk the snapshot is anchored in
  `ProtoContext::pendingRoot` (saved and restored around the call), which is
  what keeps the attribute tree reachable while the callback runs. **The
  order in which attributes are visited is unspecified.**

  Tests: `test/AttributeEnumerationTests.cpp` (12 cases).
- **`PROTOCORE_HEAP_LIMIT_CELLS` environment variable** — a `ProtoSpace` reads
  it once it is fully constructed and calls `setHeapLimits(soft, hard)`:
  - `<hard>` sets the hard ceiling only;
  - `<soft>,<hard>` sets both limits, in cells;
  - unset, `0` or a hard part of `0` means no limit, which stays the default;
  - any other value is ignored without a message.

  Without a limit protoCore starts no collection cycle by itself, and
  embedders do not call `setHeapLimits`. This variable is how an embedder
  (protopy, protost, protoclj) runs under a low memory limit, for example so
  that its GC-stress tests exercise collection:
  `PROTOCORE_HEAP_LIMIT_CELLS=500000`. The README gains a "Runtime
  Configuration" table of every environment variable protoCore reads.

  Tests: `test/HeapLimitEnvTests.cpp`.
- **`ProtoObject::partialCompare`** — an IEEE partial-order comparison that
  returns `std::partial_ordering`. Numbers compare by exact value across
  SmallInteger, LargeInteger and double (-0.0 is equivalent to 0.0 and to 0);
  any NaN is unordered with everything, itself included, so `<`, `<=`, `==`,
  `>=` and `>` against 0 are false and `!=` is true; strings compare by
  content; other pairs are equivalent when identical and unordered otherwise.
  `compare` now delegates to it and keeps its behaviour: an unordered numeric
  pair (a NaN) still compares as 0, and non-numeric pairs still order by
  address. Embedders that implement comparison operators with `compare`
  (where `NaN = 1` and `NaN <= 1` are true) can switch to `partialCompare`.
  The public header now includes `<compare>`; adding a non-virtual member
  does not change any layout.
- **Unmanaged-region API for blocking OS calls** — new pair of methods on
  `ProtoThread` and `ProtoContext`: `goUnmanaged()` /
  `returnFromUnmanaged()`. Bracket a blocking OS call (`read`, `write`,
  `poll`, `sleep`, network I/O, third-party C library calls) with this
  pair and the GC's stop-the-world quorum will NOT wait for that thread
  to reach a real safepoint — the thread is pre-counted as parked in
  `ProtoSpace::parkedThreads` for the duration of the region. Without
  this, a single thread blocked in a slow syscall could pause every
  other thread in the process behind the GC quorum until the syscall
  returned. Calls nest (depth counter per thread; outermost pair manages
  the quorum slot). `returnFromUnmanaged()` blocks if a STW phase is in
  progress at the moment of return, matching the behaviour of a normal
  safepoint park. A `ProtoContext::UnmanagedScope` RAII helper
  guarantees the matching return runs on every exit path including
  exceptions. The per-thread counter lives on `ProtoThreadExtension`
  (not on the 64-byte `ProtoThreadImplementation` Cell). See DESIGN.md
  §"Unmanaged regions: blocking OS calls without blocking the GC" for
  the full contract — particularly the rule that NO `ProtoObject*`
  access is permitted while unmanaged.
- **Caller-supplied element hash for `ProtoSet`** — `ProtoSet::addWithHash`,
  `hasHash` and `removeHash` take the hash explicitly, and
  `ProtoSetIterator::nextHash` returns the hash an element is stored under,
  so set operations (union, intersection, subset tests) can combine sets
  without hashing elements again. This lets a runtime whose equality differs
  from protoCore's use `ProtoSet`: in Python, `1`, `1.0` and `True` are one
  set element and a class's `__hash__` decides membership. A set must be
  accessed through one hash function only.
- **`PROTOCORE_TRUST_SYMBOLS` environment variable (experimental)** — when the
  variable is set (to any value), `getAttribute`, `hasAttribute` and
  `hasOwnAttribute` no longer resolve a non-interned string name through
  `SymbolTable::lookupByContent`; they report the attribute as absent. It is a
  measurement aid for embedders that always pass interned symbols: on the
  protoJS standard benchmark suite it reduced times by 4–22%, while call sites
  that pass non-interned names read back `PROTO_NONE` and take slower fallback
  paths. Write paths (`setAttribute`, `setAttributeIfEqual`,
  `deleteAttribute`) still intern string names. The default behaviour is
  unchanged.
- **Attribute-cache benchmarks and statistics** — new benchmark targets
  `hash_quality_benchmark`, `cache_pressure_benchmark` and
  `mutable_access_benchmark`, and optional instrumentation of the attribute
  cache (hits, collisions and cold misses, printed at exit) compiled in only
  when `PROTO_CACHE_STATS` is defined.

### Changed
- **Concurrent mark via a per-cycle mutable-shard snapshot** — the GC mark
  phase now runs outside the stop-the-world window, concurrently with mutator
  threads. During root collection the collector loads each of the 256
  mutable-shard roots into the new `ProtoSpace::gcMutableSnapshot[]` table,
  and the marker consults only that snapshot. Because every mutation goes
  through a shard-root compare-and-swap and cell fields are immutable after
  construction, the snapshot takes the place of a write barrier: mutators pay
  no per-write cost. The pause is bounded by root collection plus the 2 KB
  snapshot capture, independent of heap size and live-object count; floating
  garbage survives at most one extra cycle. Mark bits are now cleared by a
  bulk unmark over the marked list after sweep (Phase 6), replacing the
  pre-mark unmark pass introduced in 1.2.0, and Phase 7 clears the snapshot.
  Root collection also no longer iterates the unused `stringInternMap`, and
  interned tuples are traced by the concurrent mark instead of inside the
  pause. See `docs/GarbageCollector.md` § "Concurrent Mark Without Barriers".
- **`ProtoSpace` layout** — `ProtoSpace` gains the `gcMutableSnapshot[]` table
  and a `tupleInterner` pointer. Embedders built against 1.2.0, or against an
  earlier build of this release, must be rebuilt; no source change is required.
- **No GC trigger on free-list exhaustion without a heap limit** —
  `getFreeCells` refills from the OS without waking the collector. With no
  heap limit configured (the default), collections are started by
  `triggerGC()` and at shutdown; configured soft and hard limits drive
  collections as before. Previously every exhaustion woke the collector, which
  then waited in the stop-the-world quorum for threads that never reached a
  safepoint: in a protoST actor benchmark with 8 workers it waited 2.27 s of a
  2.49 s run and swept once, at the end.
- **`toImpl` debug checks are compiled out of release builds** — the
  embedded-value tag, expected-tag and 64-byte alignment checks in `toImpl<>`
  now run only when `NDEBUG` is not defined, so release builds reduce the
  conversion to a null check and a mask. In a release build, a wrong-typed
  conversion that used to abort with a diagnostic is now undefined behaviour.
  The protoST `fib` benchmark went from about 500 ms to about 440 ms (median).
- **Install and packaging rules only in a top-level build** — when protoCore
  is included through `add_subdirectory()`, its install and CPack rules no
  longer run as part of the parent project's package. The RPM package license
  is declared as MIT.
- **Documentation restructure** — design specifications moved to
  `docs/archive/design-specs/` and the step-by-step implementation plans were
  deleted; five dated analyses moved to `docs/archive/` with a banner stating
  that their "production ready" assessment is superseded; `DOCUMENTATION.md`
  and `docs/README.md` are now indexes of the current documentation.
  `docs/GarbageCollector.md` documents the concurrent mark, the anatomy of the
  stop-the-world pause and a comparison with other production collectors, and
  `docs/STW_ELIMINATION_RESEARCH.md` records the research on bounding the
  pause. The README lists protoClojure and protoCpp in the ecosystem.
- **Repository hygiene** — build trees, in-source CMake and test outputs,
  static libraries and benchmark executables, generated Doxygen XML, a Python
  virtualenv, performance logs, IDE settings and unreferenced media files are
  no longer tracked and are ignored by `.gitignore`.

### Removed
- `ProtoThread::setManaged()` / `setUnmanaged()` — declared but never
  implemented and without callers; superseded by the unmanaged-region API.
- The unmaintained Sphinx documentation site (`docs/conf.py`, the `.rst`
  pages, `docs/Makefile`, `docs/make.bat` and the Doxyfiles under `docs/`).
  API reference generation uses the root `Doxyfile`. Guides and tutorials that
  described APIs and tools that do not exist were removed as well.

### Fixed
- **`ProtoObject::isByte` is now defined and exported.** It was declared in
  the public header but had no definition anywhere, so an embedder that
  called it failed to link. `nm -D --defined-only` on the shipped library
  listed 18 `ProtoObject` type predicates and `isByte` in neither the defined
  nor the undefined table; no commit in the repository ever added a body, and
  protoJS had already documented the workaround in
  `ProtoCoreNativeBindings.cpp` ("several others are declared in protoCore.h
  but not defined in the library").

  It answers **true for a SmallInteger whose value fits in one byte** — an
  EMBEDDED_VALUE of embedded type SMALLINT whose value is in the closed range
  `[-128, 255]`. protoCore has no distinct byte type (no `POINTER_TAG_BYTE`,
  no `EMBEDDED_TYPE_BYTE`; `fromByte(char)` is `fromInteger(char)` and
  `asByte` reads the low 8 bits of a SmallInteger), so the predicate answers
  "would this value survive the byte round trip", and that range is exactly
  the set that does: every `char` `fromByte` can encode plus the unsigned
  0..255 reading byte buffers use. `isByte(fromByte(c))` holds for every
  `char c`. It is false for integers outside the range, LargeIntegers,
  booleans, unicode chars, `PROTO_NONE`, strings, symbols, byte buffers
  (`isByteBuffer` is unrelated), doubles, methods and objects, and is safe on
  a null receiver.

  Tests: `test/AttributeEnumerationTests.cpp` (4 cases).
- **`ProtoObject::clone` now carries the own attributes and parents a MUTABLE
  receiver holds at the time of the call**, instead of the ones it was created
  with.

  **Cause.** A mutable object never writes back into its handle cell:
  `setAttribute` publishes a fresh state cell into the mutable shard table
  (built with `mutable_ref = 0`) and returns the SAME handle. `clone` read
  `oc->attributes` and `oc->parent` straight off that handle, skipping the
  `resolveMutableSnapshot` step that `getAttribute`, `getOwnAttributeDirect`,
  `getAttributes` and `isInstanceOf` all perform. The copy therefore carried
  the object's birth-time state — for a freshly created mutable object, an
  empty attribute table. protoJS's `protoCore.ImmutableObject({a:1})` came
  back as `{}`; `MutableObject`, `MakeImmutable` and `MakeMutable` share the
  same call and the same breakage, since all four use `clone` as a
  freeze / thaw operation.

  The documented contract — "new instances with the same attributes" — was the
  correct one, so the behaviour was fixed rather than the documentation.
  `clone` had neither header documentation nor a single test referencing it,
  which is how the gap survived; it now has both.

  Note: `newChild` carries the same unresolved-snapshot pattern for its parent
  link and is deliberately NOT changed here.

  Tests: `test/AttributeEnumerationTests.cpp` (4 cases).
- **`Integer::asLong` no longer returns 0 for values above the second 64-bit
  digit.**

  **Cause.** A `LargeInteger` cell holds
  `LargeIntegerImplementation::DIGIT_COUNT` (4) 64-bit digits, chained through
  `next`. `asLong` rejected only `next != nullptr` or `digits[1] != 0`, so a
  value whose only non-zero digit was `digits[2]` or `digits[3]` passed the
  check and was returned as `digits[0]`. 2^128, 2^192 and 2^200 all read back
  as 0, while 2^64 (`digits[1]`) and 2^256 (a second chunk) were rejected
  correctly, which is why the window between them looked arbitrary. protoCore's
  own bitwise operations, which call `asLong` on their operands, were affected
  as well. Reported from protoClojure, where such literals evaluated to 0.

  **Change.** `asLong` throws unless `next` is null and every digit above
  `digits[0]` is zero. Parsing, printing and arithmetic were already correct:
  a probe covering 2^(64k) for k = 1..8, the values on either side, both signs
  and round trips through `asIntegerString` in bases 10 and 16 had 11 failures,
  all of them `asLong`, and now has none.

  **Tests.** `test/LargeIntegerRangeTests.cpp`.
- **`ProtoString::createSymbol` of an existing symbol no longer allocates.**

  **Cause.** For a name longer than `INLINE_STRING_MAX_BYTES`, or with
  non-ASCII bytes, `createSymbol` built a `ProtoStringImplementation` with a
  null `ProtoContext` and only then called `SymbolTable::intern`, whose
  `normalizeForSymbol` built a second such copy. Both are perennial: their
  cells come from `posix_memalign`, belong to no young generation and no
  freelist, and no cycle ever reclaims them. When the spelling was already
  interned, both copies were dropped on the re-check inside the shard lock and
  leaked. Every protoClojure map operation on a string key longer than 6 bytes,
  and every global access with such a name, paid it.

  **Change.** Both entry points look the spelling up before building anything:
  - new `SymbolTable::lookupUTF8(ctx, bytes, len)` finds a symbol from raw
    UTF-8 bytes without allocating, hashing them exactly as
    `computeContentHash` does, so it lands in the same shard and matches the
    same bucket as a lookup keyed by a `ProtoString`;
  - `createSymbol` calls it after the inline-string path;
  - `intern` calls `lookupByContent` before `normalizeForSymbol`, which also
    covers the auto-interning of attribute names in `setAttribute`.

  Interning semantics are unchanged: symbols stay unique and perennial.

  **Measured.** 1,000,000 `createSymbol` calls for an existing 20-byte name:
  resident memory grew by 132 KB instead of 500,132 KB (about 500 bytes per
  call), and the loop took 644 ms instead of 2,615 ms. The returned pointer was
  the canonical symbol in every call, before and after.

  **Tests.** `test/SymbolInternTests.cpp`.
- **The per-thread caches are no longer GC roots.**

  **Problem.** `ProtoThreadExtension::processReferences` traced every
  `AttributeCacheEntry` (object, result, name) and every
  `MutableValueCacheEntry` (shard_root, current_value) in the concurrent
  mark. A cached old shard root kept a whole old version of its shard alive.
  After mutables-tree entries started being released, that kept released
  states alive: a probe that writes and drops 1,000,000 mutable objects kept
  256,581 cells live after settling, against 437 for an immutable control. The
  marker also read slots that their owning threads rewrite concurrently.

  **Change (maintainer's option Q).**
  - The caches are no longer traced.
  - Each thread clears both of its caches when it resumes after a
    stop-the-world, before it can look anything up again. It does so when it
    leaves the allocation poll's park, `ProtoContext::safepoint()`,
    `ProtoThread::synchToGC()` or a heap-headroom wait, and when it returns
    from an unmanaged region.
  - A per-thread copy of `gcCycleCount`
    (`ProtoThreadExtension::lastClearedEpoch`) limits the clear to once per
    cycle.
  - Parking itself does not change: when threads park, the stop-the-world
    quorum and what runs under stop-the-world are unchanged. Only what a
    thread does right after it resumes changes.
  - Entry layouts and lookup paths are unchanged.

  **Why it is sound.** Every entry a lookup sees was written after the last
  stop-the-world, so the cells it names were marked or young then, and none is
  freed or has its address reused before the next stop-the-world clears the
  entry. A thread inside a critical section delays the stop-the-world, so it
  cannot miss a clear.

  **Note for embedders.** A value read from shared mutable state and held in
  C++ across allocations was sometimes kept alive, incidentally, by the
  reading thread's own cache entry. That is no longer the case. The existing
  rule applies unchanged: root such values or keep them inside a critical
  section.

  **ABI change.** `ProtoThreadExtension` (internal) uses its last free 8 bytes
  for `lastClearedEpoch`, and stays within its 64-byte cell. Embedders must
  rebuild.

  **Tests.** New `test/ThreadCacheClearTests.cpp`:
  - both caches are empty once their owner resumes from a cycle;
  - new objects at reused addresses never get a stale cached attribute;
  - a thread returning from an unmanaged region across cycles has its caches
    cleared;
  - a thread whose heap-headroom wait spans a cycle has its caches cleared.

  `ConcurrentMarkSafety.ThreadCacheSlotFlipsDuringMark` now checks that the
  marker never reads the caches.

  **Measured.** A probe that writes and drops 1,000,000 mutable objects now
  settles at 437 live cells, equal to the immutable control, with no cache
  refresh of any kind; it settled at 256,581 before. Maximum RSS 107 MB.

  Hot paths are unchanged. `perf stat -r 5` instructions, the same binaries
  run against the library at `e0793341` and against this commit:

  | Benchmark | e0793341 | this commit | Change |
  |---|---:|---:|---:|
  | `microbenchmark_final` | 8,704,375,885 | 8,703,402,870 | −0.01% |
  | `mutable_access_benchmark` | 17,048,728,009 | 17,045,648,828 | −0.02% |
  | `cache_timing_benchmark` | 4,627,674,728 | 4,626,199,150 | −0.03% |
  | `hash_quality_benchmark` | 1,692,479,243 | 1,694,592,140 | +0.12% |
  | `object_access_benchmark` | 60,376,632,041 | 60,377,961,373 | +0.00% |
  | protoPython `attr_lookup` | 18,282,758,398 | 18,277,865,604 | −0.03% |
  | protoPython `richards_lite` | 240,453,130 | 240,306,415 | −0.06% |
  | protoClojure `fib` | 4,835,137,649 | 4,832,224,960 | −0.06% |
  | protoST `attr_lookup` | 692,787,317 | 689,030,577 | −0.54% |

  Instruction stddev is at most 0.09% on every row.

  **Cost of the clear.** One clear of both tables (57,344 bytes) takes about
  800 ns. In a probe with 16 threads and forced cycles under a heap limit, 16
  cycles produced 248 clears, about 0.2 ms of a 1,183 ms run.
- **Per-thread refill batches are sized to the heap limit.**

  **Cause.** A thread allocates from a private freelist that `getFreeCells`
  refills in batches of up to 65,536 cells when several threads run, or a whole
  8,192-cell chunk. Those cells count against the heap limit, but no cycle can
  reclaim what a thread holds. Under `PROTOCORE_HEAP_LIMIT_CELLS=500000`, eight
  worker threads' batches alone exceeded the limit, so protoST aborted with
  "out of memory" while the live set was 48,000–350,000 cells.

  **Change.** Only when a hard limit is configured, each refill is capped at
  `maxHeapSize / (8 × runningThreads)` cells, never below 512, so all threads'
  batches together use at most one eighth of the limit. A larger free chunk is
  split. An OS request keeps its usual size, and the surplus is published as
  free chunks. Without a limit, batch sizes and the path are unchanged.

  **Constants.** `kLimitBatchFraction = 8`, the suggested starting point.
  `kMinLimitedBatchCells = 512`, so a tiny limit or many threads do not turn
  every few allocations into a refill under `globalMutex`. The cap uses
  `runningThreads` at each refill. That count dips while a thread waits for
  headroom, but a waiting thread holds almost nothing, so the sum stays bounded.
  A free chunk is split only when it is larger than the cap. When the cap does
  not bind, as with a single thread under a generous limit, chunks are handed
  out whole as before.

  **Tests.** `test/HeapLimitBatchTests.cpp`:
  - Eight threads start together under a 500,000-cell limit. The sum of the
    cells held in their freelists stays within a quarter of the limit, and live
    data survives. Without the cap, the sum reached 466,392 cells, with 65,423
    in a single thread.
  - Without a limit, a refill still hands out a full chunk.

  **Measured.** `perf stat` user-space instructions, same binaries, uncapped
  against capped library:

  | Benchmark | No limit | `PROTOCORE_HEAP_LIMIT_CELLS=64000000` |
  |---|---|---|
  | Single-thread allocation probe (5,000,000 objects) | unchanged (−0.01%) | unchanged (±0.001%) |
  | `immutable_sharing_benchmark` | unchanged (−0.02%) | unchanged (±0.001%) |
  | `concurrent_append_benchmark` | ±8% run-to-run noise, no usable signal | same |

  **protoST at 500,000 cells.** The out-of-memory aborts with small live sets
  are gone. The remaining aborts report live sets of 406,000 to 583,000 cells.
- **A snapshot read from the mutables tree is never held across a
  park point** — preparation for the per-thread caches no longer being GC
  roots.

  **Why.** Three kinds of code resolve a mutable object's current state, then
  keep using it, or the shard root they read, across allocations that can
  park for a stop-the-world. Only C++ locals reference those cells. If
  another thread publishes a new state meanwhile, nothing else keeps the old
  one alive. Until now the thread's own mutable value cache entry happened to
  keep them alive, because the collector traced the caches.

  **Changes.**
  - `ProtoObject::getParents` and `ProtoObject::getAttributes` open their
    outermost critical section before resolving the snapshot and keep it until
    the result is built. Their heap checkpoint now runs before the resolve, not
    during the build.
  - No thread parks inside a critical section in any configuration:
    - `ProtoContext::allocCell`, `ProtoContext::safepoint()`,
      `ProtoThread::synchToGC()` and the headroom wait now skip parking inside
      a critical section even with `PROTOCORE_GC_REINCLUDE_SURVIVORS=OFF`, as
      they already did with it ON;
    - in that configuration, the `setAttribute` family could previously park
      while path-copying a shard root it had read;
    - the stop-the-world quorum now waits for critical sections to end in
      every configuration.
  - This also removes a documented known issue of the OFF configuration: an
    exiting thread, parked inside `removeAt`'s critical section, could be
    counted twice by the quorum.
- **Waiting for heap headroom no longer hands the waiting context's young
  generation to the collector** — under a hard heap limit, a thread that
  reaches the ceiling waits in `ProtoSpace::waitForHeapHeadroom`, called from
  the heap checkpoint at the entry of an outermost critical section. After the
  wait, `reclaimWaitLocked` called `ProtoContext::safepoint()`. With
  `PROTOCORE_GC_REINCLUDE_SURVIVORS`, once the context had allocated more than
  `maxAllocatedCellsPerContext` cells (10,000 by default), that call submitted
  its young chain.

  The wait runs inside native code, where the caller may hold a half-built
  structure only in C++ locals and in that chain, so the next cycle freed
  cells still in use. For example, protoClojure's `vector` primitive builds a
  list and passes it to `newTupleFromList`: the list was freed while the tuple
  was being built, and protoClojure's `cli/large-program` test crashed with
  SIGSEGV in about half the runs at `PROTOCORE_HEAP_LIMIT_CELLS=2000000`. This
  contradicted the rule that a young generation is submitted only when its
  context is destroyed or at a `safepoint()` the embedder calls.

  The wait now parks through the thread's park-only entry
  (`ProtoThread::synchToGC`); a context without a thread parks the same way.
  No public API changes. The only other place protoCore calls `safepoint()`
  itself is `thread_main`, on a new thread's fresh context before its method
  runs; that context holds no cells, and the call is unchanged.

  Test: `test/HeapHeadroomWaitTests.cpp`. It fails before the change in 3 of 3
  runs: a 1,000-element list read back with size 0.
- **A mutable object's state is released after the object is collected** —
  every update of a mutable object (`newObject(true)`,
  `newChild(context, true)`, `clone(context, true)`) stores its state in
  `ProtoSpace::mutableRoot`, keyed by its `mutable_ref`, and no code ever
  removed an entry. The last state of every mutable object that was ever
  written, and everything it referenced, therefore stayed reachable after the
  object became garbage, and every cycle marked it again. The collector now
  releases the entry:
  - `ProtoObjectCell::finalize` of a collected handle only records its
    `mutable_ref`;
  - after sweep, the GC thread removes the recorded refs shard by shard with
    one compare-and-swap per shard, redone from the new root when a mutator
    published to the shard meanwhile;
  - the path copies are allocated through the collector's own context,
    `ProtoSpace::gcContext`, built with a new `ProtoContext::GCOwnedTag`
    constructor that never registers as `mainContext`.

  The entry disappears in the cycle that collects the handle, and the state is
  freed in the next cycle. Two limits remain, both documented:
  - With `PROTOCORE_GC_REINCLUDE_SURVIVORS=OFF`, a handle that survives a cycle
    is never collected, so its entry stays. This is the expected behaviour of
    that configuration.
  - A thread's mutable value cache keeps a pre-release shard root alive until
    its slot is reused.

  Finalizers now have a written contract: they complete an action on a
  structure, and never allocate, publish or process
  (`docs/GarbageCollector.md` § "Finalizer contract"). Embedder guidance on
  when to use mutable objects is in
  `docs/Structural description/architecture/02_mutability_model.md`.

  Measured with a probe that writes and drops 1,000,000 mutable objects, each
  with one attribute holding a fresh object, in batches of 50,000 with one
  cycle per batch:

  | | Before | After |
  |---|---|---|
  | Entries left | 1,000,000 | 0 |
  | Live cells after settling | 5,009,397 | 256,581 (1,737 once the thread's 1,024 cache entries are replaced; immutable control: 437) |
  | Maximum RSS | 479 MB | 107 MB |
  | Wall time | 14.4 s | 5.2 s |
  | Mark, per settling cycle | 404–649 ms | 24–29 ms |
  | Sweep, per settling cycle | 367–640 ms | 31–36 ms |
  | Bulk unmark, per settling cycle | 92–114 ms | 3.4–3.6 ms |

  The release itself took 17.6 ms on average and 24.2 ms at most per cycle
  that released 50,000 entries. It is reported in the new `REL=` field of the
  `PROTOCORE_GC_PROFILE` line.

  **ABI change:** `ProtoSpace` gains the trailing members `gcContext` and
  `gcFinalizedMutableRefs`, and `ProtoContext` gains a constructor and a
  private flag. Embedders must rebuild.

  Tests: `test/MutableRootReclaimTests.cpp`.
- **Stop-the-world collects only roots** — Phase 2 of the collector walked
  every context's young chain and called `processReferences` on each young
  cell while all threads were stopped, so the pause grew with the number of
  young cells. With `PROTOCORE_GC_REINCLUDE_SURVIVORS` it also scanned every
  survivor-pen cell on non-fold cycles and relinked a folded pen one segment
  at a time under the pause. Stop-the-world now records one handle per
  context, the head of its young chain, and captures the survivor pen in
  O(1); the chains, the pen cells and the pen fold are handled by the
  concurrent mark. In a probe with one thread holding 20,000,000 young
  cells, P1 + P2 per cycle fell from 299–373 ms to 10–70 µs (20,000 young
  cells: from 244–508 µs to 13–98 µs). With `PROTOCORE_GC_INSTRUMENT`, P2
  now ends when the world resumes rather than when mark starts.
- **The collector no longer dereferences a null work-list entry** — the
  concurrent mark crashed with a segmentation fault at address 0x8 (reading
  `Cell::next_and_flags` of a null `Cell*`, `gcThreadLoop+0xdf8`), seen in
  0.2–0.3 % of protoPython runs in a stress configuration with very frequent
  collection cycles.
  `ProtoThreadExtension::processReferences` read every per-thread cache slot
  twice, once for `ProtoObject::isCellPointer` and once for `asCellPointer`,
  while the owning thread kept rewriting the slot; a slot that changed to
  `nullptr` or an embedded value between the two reads was reported as a null
  child and pushed on the work list. Every `processReferences` implementation
  now loads each reference field once (`if (const Cell* c =
  ProtoObject::asCellPointer(field)) method(ctx, self, c);`), which also stops
  a tagged null (a non-embedded tag with no pointer bits) from being reported.
  The mark loop skips null entries at its single pop site, which also covers
  roots holding a tagged null. In builds with `PROTOCORE_GC_INSTRUMENT` or
  without `NDEBUG`, a `processReferences` that reports `nullptr` aborts with a
  message naming the reporting cell's type.
- **A double's hash agrees with equality** — `DoubleImplementation::getHash`
  hashed the bit pattern, so NaNs of different sign or payload (x86
  `0.0/0.0` is a negative NaN, `std::nan("")` a positive one) were different
  `ProtoSet`/`ProtoMultiset` elements, and whether `-0.0` and `0.0` collided
  depended on the standard library. All NaNs now hash as the canonical quiet
  NaN and `-0.0` hashes as `0.0`; other doubles keep their hash. Hashes still
  differ across numeric kinds (`1` and `1.0`), as documented for `ProtoSet`:
  a language whose equality crosses kinds supplies its own hash.
- **`ProtoList::has` with integers beyond `long long`** — both list forms
  compared integer elements with `asLong`, which throws
  `std::overflow_error` for a `LargeInteger` outside that range, so `has`
  threw on a list holding such an element (or when asked for one). Integers
  are now compared by value with `Integer::compare`, as `ProtoTuple::has`
  already did.
- **The GC cycle counter advances in every build configuration** — the only
  increment of `gcCycleCount` sat inside the `PROTOCORE_GC_REINCLUDE_SURVIVORS`
  block, so with `-DPROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` `getGCCycleCount()`
  stayed at zero and heap-limit reclamation waits, which wait for the counter
  to advance, never saw a cycle complete. The counter is now incremented for
  every cycle; the survivor re-chain still uses the same cycle number to decide
  fold cycles.
- **String hashes depend on content, not rope shape** — `getHash` on a heap
  string returned a cached hash that mixed its children's hashes, so equal
  content split differently by concatenation hashed differently (a 47-byte
  literal and the same text built as 37 + 10 bytes). Every structure keyed by
  `getHash` missed equal strings, including sparse lists and tuple hashes.
  Heap strings now hash with the structure-independent FNV-1a content hash
  that string interning already uses, and inline strings use the same FNV-1a
  over their bytes, so a string hashes the same whether it is inline or
  heap-backed.
- **`LargeInteger` hashing covers every digit** — the hash used only the
  lowest digit, so every multiple of 2^64 collided and hash-keyed structures
  kept a single entry for 2^64, 2^65 and 2^70.
- **`ProtoObject::asDouble` on large integers** — no longer throws for a
  `LargeInteger` beyond the range of `long long`; the value is converted
  through its decimal digits, and out-of-range values become ±infinity.
- **Exact integer/double comparison** — `ProtoObject::compare` converted the
  integer to `double`, which threw beyond `long long` and rounded above 2^53
  (`2**70 + 1` compared equal to `2.0**70`). The integer is now compared with
  `floor(d)` as integers. NaN compares as before; ±infinity is handled
  explicitly.
- **`objectPrototype` is mutable** — `ProtoSpace::objectPrototype` was created
  immutable, so the first `setAttribute` on it returned a new object and left
  every previously stored reference stale (in protoJS,
  `Object.prototype.foo = 1` was silently lost). `getPrototype` on
  `objectPrototype` itself now returns `nullptr` instead of looping back to
  itself, and `ProtoObject::clone(ctx, true)` now returns a mutable clone (the
  flag was ignored). Across ten protoJS test262 families, passing tests went
  from 8397 to 8451 of 9823.

### Performance
- **Tuple interning uses a sharded hash table** — the interner was a binary
  search tree under `ProtoSpace::globalMutex` that never rebalanced and, keyed
  by allocation-ordered pointers, degenerated into a linked list walked twice
  per tuple creation. `TupleInterner` uses 64 shards, each with its own hash
  index and a mutex that is never held across an allocation; lookup runs
  before allocation. Interned tuples remain perennial. Interning 100,000
  distinct tuples timed out after 120 s before and now takes 0.03 s, as does
  the 4-thread concurrent test (previously 82 s).
- **`getOwnAttributeDirect` consults the attribute cache** — it resolved the
  mutable snapshot and then walked the attribute tree on every call; it now
  checks the per-thread `(snapshot, name)` attribute cache first, like
  `getAttribute`. No behavioural change.
- **Better attribute-cache slot distribution** — cell pointers are 64-byte
  aligned, so their low 6 bits are always zero; the cache index now shifts
  them out. `object_access_benchmark` used 6.6% fewer cycles and
  `hash_quality_benchmark` 16% fewer.
- **Attribute-cache entries padded to 32 bytes** — two entries now fit in one
  64-byte cache line and the table is allocated 64-byte aligned, at a cost of
  8 KB per thread. `object_access_benchmark` used 3.6% fewer cycles with 27%
  fewer L1 data-cache misses.
- **One codepoint pass per byte when building a string** — `buildAVL` counted
  the codepoints of its whole byte range on entry, then discarded the count
  whenever the range was larger than a 32-byte leaf and it recursed, so the
  scan ran once per recursion level: 15 redundant full passes over a 1 MiB
  buffer. The count is now taken in the leaf branch only, the one place that
  uses it; `StringInternalNode`'s constructor already derives `total_chars`
  and `left_chars` from its children in O(1). Every byte is scanned exactly
  once and the rope is unchanged — same leaves, same internal nodes, same
  depth, same per-node counts, same content hash. This is the path taken by
  every bulk constructor (`ProtoString::fromUTF8Buffer`,
  `ProtoStringImplementation::fromUTF8Bytes`, `ProtoString::create`,
  `ProtoString::createSymbol`), so it speeds up every file read and every
  embedder that builds a string from a buffer. Measured with `fromUTF8Bytes`
  on ASCII, cells identical at every size: 9.37 to 6.82 ns/char at 467 B,
  11.63 to 5.88 at 4 KiB, 17.25 to 6.14 at 64 KiB and 16.32 to 6.38 at 1 MiB
  (2.6x; 17.1 ms to 6.7 ms for the whole build).
- **String construction no longer builds a list of code point objects** —
  `ProtoContext::fromUTF8String`, and therefore `ProtoString::fromUTF8`,
  `fromUTF8String`, `fromStdString` and `fromCodepointTuple`, built an
  N-element `ProtoList` of code point objects one `appendLast` at a time,
  re-encoded that list into a `std::string` and then called the bottom-up rope
  builder anyway. Every append copied a root-to-leaf path, so construction
  cost O(N log N) cells of which all but the final rope were dead before the
  constructor returned: at 1 MiB, 23.1 million cells to produce a 65,536-cell
  rope, 99.7% garbage. It now decodes the bytes once and builds the rope
  directly, allocating exactly the cells the rope needs — the theoretical
  minimum of `2 * ceil(B / 32)`, about 4 bytes of heap per ASCII character,
  with no garbage at any size. Measured on ASCII:

  | length | before | after | speedup | cells before | cells after |
  |---|---|---|---|---|---|
  | 7 B | 194.4 ns/char | 11.8 ns/char | 16x | 32 | 2 |
  | 467 B | 554.5 ns/char | 7.28 ns/char | 76x | 5,108 | 32 |
  | 4 KiB | 720.4 ns/char | 6.80 ns/char | 106x | 57,576 | 256 |
  | 64 KiB | 931.7 ns/char | 6.52 ns/char | 143x | 1,183,712 | 4,096 |
  | 1 MiB | 1138.3 ns/char | 6.55 ns/char | 174x | 23,134,168 | 65,536 |

  The resulting string is unchanged in every observable way — same bytes, same
  size, same content hash, same rope (leaves, internal nodes, depth), same
  inline-versus-heap representation, same symbol behaviour — including for
  malformed UTF-8, which is still decoded and re-encoded so that a truncated
  sequence degrades to its lead byte and an overlong sequence collapses to its
  shortest form.

  The construction also no longer holds a GC critical section across the
  per-character loop. That section suppressed this thread's stop-the-world
  parking for the entire build (1.38 s for 1 MiB) while protecting nothing:
  cells allocated during the build sit on the context's young chain, which the
  collector records as a root and which can only become a sweep candidate once
  `ProtoContext::safepoint()` or context destruction submits it — neither of
  which the builder calls. The heap-ceiling backpressure the section's
  constructor performed is kept, as an explicit `heapLimitCheckpoint()` before
  the first allocation. Repeated builds of a 467-character value under a hard
  ceiling now run collections and stay inside it.

  Elsewhere the two string changes are neutral or better:
  `immutable_sharing_benchmark`, `list_benchmark` and
  `object_access_benchmark` move by +0.8%, +0.2% and -0.6% of cycles with
  instruction counts flat. One microbenchmark is slower:
  `string_concat_benchmark` (10,000 rope joins through `appendLast`, a path
  neither change touches) takes 2.8% more wall clock and 3.8% more cycles
  while executing 0.3% more instructions — a code-layout effect of the new
  functions in `core/ProtoString.cpp`, not extra work.

### Tests
- `StringBuildTests` (eleven cases) pins what string construction *produces*,
  so that changes to how it is built cannot change what is built. A 42-entry
  golden corpus — the well-formed ladder from empty to 64 KiB, 2/3/4-byte
  sequences, combining marks, and 18 malformed-UTF-8 cases — was captured from
  the library before the change and is checked for content bytes, codepoint
  size, content hash, inline-versus-rope representation and rope shape (leaf
  count, internal count, depth). The same corpus is compared, node for node,
  against a reference implementation of the old code-point-list construction
  kept in the test file. Every leaf's `char_count` is recounted from its own
  payload and every internal node's
  `total_chars` / `left_chars` / `total_bytes` is checked against its
  children; multi-byte sequences are built at every length around the 32-byte
  leaf boundary. Further cases bound the cells one build may allocate to the
  size of the rope it produces (a bound the list construction misses by two
  orders of magnitude); run a thousand 467-character builds under a hard heap
  ceiling and check that collection cycles run and that both the heap and the
  resident set stay bounded; and build 1 MiB through the public and the bulk
  entry points. Plus the inline boundary, symbol interning and identity, and
  `fromStdString` agreeing with `fromUTF8` over the whole corpus.
- `GCRootScope` (five cases, cycles forced with a small heap limit): a probe
  cell in a live young chain and one in the survivor pen are traversed by the
  collector but never while `stwFlag` is raised (each failed against the
  library before the corresponding fix); an object referenced only by a
  young cell, and one referenced only through the mutables tree, survive
  cycles; four threads allocate and check young cells over old candidates
  while cycles run.
- `ConcurrentMarkSafety.ThreadCacheSlotFlipsDuringMark` flips the main
  thread's attribute-cache and mutable-value-cache slots between a cell and a
  non-cell from a helper thread while a hard heap limit just above the
  startup heap forces repeated cycles; it crashed before the null work-list
  fix.
  `GCMark.NullReferenceIsSkipped` pins a tagged null in a root set (it crashed
  before the fix), and
  `GCMarkDeathTest.NullReferenceFromProcessReferencesIsReported` checks that
  instrumented and debug builds name a cell type whose `processReferences`
  reports `nullptr`.
- `NumericCompareTest` (four cases): every NaN is unordered in both operand
  orders against NaNs, ±0.0, ±inf, SmallIntegers and ±2^70; for ordered
  numbers from -inf to +inf (including -2^70, -0.0/0/0.0, 2^53+1 and 2^70)
  the sign of `partialCompare` matches `compare` and the expected order;
  strings, objects and mixed pairs; and NaN elements do not alias numbers in
  `ProtoSet`, `ProtoMultiset`, `ProtoList` and `ProtoTuple`.
- `NumericTest.DoubleHashAgreesWithEquality` checks that quiet, negative and
  payload NaNs hash alike, that `-0.0` and `0.0` hash alike, and that a set
  treats them as one element (the NaN cases failed before the fix).
- `ListTest.HasComparesLargeIntegersByValue` checks `has` on inline and AVL
  lists holding 2^70 against an equal distinct object, neighbours and small
  integers (it threw `std::overflow_error` before the fix).
- New suites and cases: `ConcurrentMarkSafety` (mutation during mark, no lost
  mutable references), `UnmanagedRegionTest` (six cases), three `TupleTest`
  interning cases, `StringTest.EqualContentHashesEqualWhateverTheRepresentation`,
  `SetTest.ExplicitHashKeysElementsByTheCallersHash` and three `NumericTest`
  cases for large-integer hashing, conversion and comparison.
- `SwarmTest.OneMillionConcats` and `SwarmTest.LargeRopeIndexAccess` are
  enabled again (the rope GC failures they were disabled for no longer
  reproduce), and `SwarmTest.LargeRopeIndexAccessUnderHeapPressure` builds a
  large rope under a tight heap limit so reclamation runs during
  construction.

## [1.2.0] - 2026-05-22
### Added
- **`ProtoObject::setAttributeIfEqual` — public attribute compare-and-swap** —
  `setAttributeIfEqual(ctx, name, expected, newValue)` writes `newValue` only
  if the receiver's current OWN value for `name` is still pointer-identical to
  `expected`, returning whether the swap happened. It exposes the shard-root
  CAS loop that `setAttribute` already runs internally, so an embedder can
  build a lock-free read-modify-write (e.g. appending to a list held under an
  attribute) with a CAS-retry loop instead of an external mutex. `expected ==
  nullptr` means "the attribute is currently absent". Requires a mutable
  receiver; an immutable receiver is a no-op returning `false`. This is the
  protoCore primitive that lets protoST drop its per-actor mailbox mutex.
- **Configurable heap allocation limit with reliable OOM detection** — a new
  `ProtoSpace::setHeapLimits(softCells, hardCells)` lets an embedder cap the
  `Cell` heap instead of growing it unbounded until the OS is exhausted. Both
  limits are in `Cell`s; `0` (the default) disables them and the allocator is
  bit-for-bit the previous unbounded path — the entire feature is gated on
  `maxHeapSize > 0`.
  - `getFreeCells` clamps every OS request to the remaining headroom, so
    `heapSize` never crosses the hard ceiling for ordinary allocations.
  - A thread that must wait for the GC leaves the running set first
    (`ProtoSpace::waitForHeapHeadroom` → `reclaimWaitLocked`), so it never
    stalls the Stop-The-World quorum.
  - The blocking check runs at outermost critical-section entry
    (`ProtoContext::heapLimitCheckpoint`, called from the `CriticalSection`
    constructor): a thread mid tree-build holds un-anchored cells and must stay
    in the running set, so it cannot wait there. An allocation that drains the
    cell pool *inside* a critical section may overshoot by at most one OS batch.
  - OOM is detected from per-cycle *reclamation*: the GC publishes
    `reclaimedLastCycle`; two consecutive completed cycles that each reclaim
    zero cells while the heap is at its ceiling are genuine OOM. A mark-phase
    live count would miss a live context's un-submitted young generation, which
    fills the heap without entering `markedList`. On confirmed OOM,
    `outOfMemoryCallback` is invoked once, then protoCore performs a controlled
    `std::abort()` with a diagnostic.
  - `getFreeCells(const ProtoThread*)` → `getFreeCells(ProtoContext*)`; new
    `ProtoSpace` members `softHeapLimit`, `reclaimedLastCycle`,
    `liveCellsLastCycle`, `memoryReclaimedCV`; new `ProtoContext` method
    `heapLimitCheckpoint`.
  - See `DESIGN.md` § "The Heap Allocation Limit and Out-of-Memory Detection"
    and `docs/archive/design-specs/2026-05-22-allocation-limit-oom-design.md`.

### Changed
- **Interned strings are always perennial** — `SymbolTable::intern` now builds
  every symbol with a null `ProtoContext` unconditionally; the `is_strong`
  parameter and the never-called `removeWeak()` weak-eviction path are removed.
  Previously a "weak" symbol could be allocated against a `ProtoContext` and
  collected by the GC — but every caller already interned strong, so the weak
  path was dead code, and it was unsound to keep: a perennial symbol whose
  bucket got evicted would lose its canonical pointer and break pointer-identity
  symbol comparison. Now, when a symbol is created — or a non-interned string is
  converted to one — the interned string is allocated outside the GC and lives
  for the lifetime of the process. No public API change
  (`ProtoString::createSymbol` is unchanged). See `core/SymbolTable.cpp` and
  DESIGN.md § "Mechanism A — Perpetual allocation via `ProtoContext* = nullptr`".

### Fixed
- **GC stale-mark bug** — Mark phase set the per-cell mark bit on every reachable
  cell, but Sweep only cleared that bit on cells inside the captured
  `segmentsToProcess` snapshot. Cells reachable from a root that lived outside
  that snapshot (young cells whose owning context never submitted, perpetual
  prototypes, tuple/string interner entries) carried `mark=1` over from the
  previous cycle. The next cycle's mark drain skipped them via the
  `if (!isMarked())` guard and never traced their children, so any candidate
  reachable exclusively through that path was freed by Sweep while still live.
  Reproduced 100% with `PROTOCORE_GC_REINCLUDE_SURVIVORS` enabled and a
  16k-node mutable-object workload (protoJS `tree_traversal` benchmark crashed 0/5).
- **Fix** — Added a pre-mark unmark pass that walks the live graph from all
  roots and clears the mark bit on every reachable cell before Phase 4 begins.
  Cost is `O(reachable cells)`, comparable to Mark itself, in exchange for a
  clean tricolor invariant at the start of every cycle. This pass was later
  replaced by the bulk unmark of the concurrent mark (see the concurrent mark
  entry above); its `docs/GarbageCollector.md` section no longer exists.

### Tests
- 196/196 protoCore tests pass — including the `AllocationLimit*` suite
  (hard-limit reclamation, soft limit, `setHeapLimits` validation and an
  unrecoverable-OOM death test) and the `AttributeCas*` suite (compare-and-swap
  match / mismatch / absent / immutable, plus an 8-thread no-lost-update
  concurrency proof).

## [1.1.0] - 2026-04-02
### Added
- **Performance Benchmarking Suite**: Integrated micro-benchmarks for List, SparseList, Object Access, and String Concatenation into the CMake build system.
- **JIT Impact Analysis**: Comprehensive performance grounding for the "Mechanism over Policy" strategy, validating structural sharing efficiency and identifying attribute lookup bottlenecks.
- **String Handling Refactor**: Introduced **Inline Strings** (up to 6 UTF-8 bytes) stored directly in `ProtoObject*` tagged pointers.
- Optimized hybrid data model for `ProtoString` (Inline + Rope).
- Enhanced `RopeCharacterIterator` and `ProtoStringIteratorImplementation` for performance.
- Direct bitwise comparison and hashing for inline strings.

### Fixed
- **API contract: PROTO_NONE for missing keys** — Align `getAttribute` and `ProtoSparseList::getAt` with the documented API: return `PROTO_NONE` instead of `nullptr` when an attribute or key is not found.
- **ProtoObject::getAttribute** — Returns `PROTO_NONE` instead of `nullptr` when the attribute does not exist (ObjectTest.GetMissingAttribute).
- **ProtoObject::hasAttribute** — Correctly treats `PROTO_NONE` as "not found" when checking attribute existence.
- **ProtoSparseList::getAt** — Returns `PROTO_NONE` instead of `nullptr` when the key is not in the map (ContextTest.LocalVariableAllocation).
- **ProtoMultiset** — `add`, `count`, and `remove` now treat `PROTO_NONE` as a missing entry, fixing MultisetTest.AddAndCount, MultisetTest.Remove, and MultisetTest.RemoveNonExistent.

### Tests
- All 136 tests pass (100% pass rate) following the performance infrastructure integration.

## [1.0.0] - 2024-01-22
- Initial release of protoCore shared library.
