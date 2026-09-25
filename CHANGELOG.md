# Changelog

All notable changes to protoCore are documented in this file.

## [2.4.0] - 2026-09-25

Two kernel defects, on the maintainer's instruction to *"fix both without fail
even though they touch the kernel"*. Both were found by the P4 embedder
conformance work; neither was reachable from the embedders' own test suites.

**`PROTOCORE_ABI_SOVERSION` stays 3.** No class gained or lost a member, no
virtual was added or removed, no signature and no return convention changed.
The two new symbols (`proto::returnUnusedCellBatch` and
`proto::pmqTakeAllWindowHook`) are additive and declared only in
`headers/proto_internal.h`, so no embedder's compiled layout or call sites
move, and `SameMajorVersion` consumers keep working.

**Why 2.4.0 and not 2.3.1.** A patch release would say "nothing here you need
to know about", and that is not true of the second fix. It removes a
documented Known Issue from `docs/GarbageCollector.md` -- an exiting thread now
gives its allocation batch back -- which is a new guarantee an embedder may
rely on, and it gives `ProtoThread` new post-conditions after a completed
`join`: `getCurrentContext()` now returns `nullptr` instead of a stale pointer
to a context nobody owned, and the `std::thread` object is gone. Reading a
finished thread's context was never defined, but it used to return something;
that is a behaviour change, so it gets a minor bump.

### Fixed

- **`ProtoMPSCQueue::takeAll` could lose the last message of a batch**
  (`core/ProtoMPSCQueue.cpp`). Silent data loss, shipped. `takeAll` published
  its retain cell with the chain it had **loaded**, then detached a possibly
  **longer** chain -- every node a producer prepended in between was in the
  detached chain and in no retain cell. The file's own argument for those nodes
  was that they "were allocated after S, so they are young cells of the pushing
  context and are not candidates of the running cycle", which is true and
  insufficient: it covers only the cycle running when the window closed, and
  the walk that follows parks for stop-the-world every 64 nodes, so the call
  routinely spans a cycle **boundary**. For the next cycle those nodes are
  ordinary candidates, hanging off nothing but a C++ local, and the collector
  has no view of C++ locals. Observed signature, in the field: of a 182-message
  batch exactly one element lost every own attribute, and always the **last** --
  the chain is LIFO, so the node prepended inside the window is the last item
  out.

  The fix is one store: after the detaching exchange, publish the chain that
  was actually detached into the same retain cell. It cannot narrow anything
  (only prepends happen, so the loaded chain is a suffix of the detached one),
  it leaves the publish-before-detach ordering the GC-safety proof depends on
  untouched, and it is still inside the window, so it is visible before this
  thread can park and therefore before any cycle for which those nodes are
  candidates can begin. The window's ABA argument is also intact: a store is
  neither an allocation nor a protoCore call, so nothing there can complete a
  sweep between the `head` load and the CAS that races it.

  **Reproduced deterministically before being fixed.** Racing a producer
  against a consumer gave 4 failures in 40 runs against 0 in 40 -- p ~ 0.12,
  which cannot distinguish a fix from luck, and this project does not ship on
  that. `test/ProtoMPSCQueueWindowTests.cpp` instead **enters** the window
  through a test-only hook (`proto::pmqTakeAllWindowHook`, null in every build,
  one relaxed load per `takeAll`) and asserts two things: that every node the
  detach took is reachable from `retained`, and -- with a pause armed from
  inside the window, so the candidate set is always fixed with the walk in
  flight -- that a canary `Cell` reachable only through the window node is
  traced and never finalized. Removing the widening store fails both, 10 runs
  out of 10, with identical numbers.

- **An exiting thread no longer leaks its allocation batch**
  (`core/Thread.cpp`, `core/ProtoSpace.cpp`). Measured in a bare `ProtoSpace`
  with no runtime at all: `freeCellsCount` fell by **4,096 cells per
  empty-bodied thread** and **8,192 per allocating one**, while `heapSize` and
  `liveCellsLastCycle` stayed constant -- the signature of memory that is
  neither live nor free. The thread's root `ProtoContext` (and with it its
  entire un-submitted young generation, and its `automaticLocals` array), the
  two per-thread caches (32 KiB `aligned_alloc` + 24 KiB `malloc`, invisible to
  `heapSize`) and the `std::thread` object leaked with it. After the fix,
  200 threads through a bare space leave `heapSize` unchanged and `inUse`
  **lower** than the baseline.

  **The release is not in `finalize`, and that is the point.** Sweep calls only
  `finalize()`, and `ProtoThreadImplementation::finalize` is empty, so the
  obvious fix is to fill it in. It is illegal three times over.
  `docs/GarbageCollector.md` section 7 forbids a finalizer from blocking,
  allocating or publishing with compare-and-swap -- and returning the batch
  blocks on `globalMutex`, destroying the context publishes a young generation,
  and joining the `std::thread` blocks outright. A finalizer is also no proof
  that the OS thread has stopped, since sweep runs with the world going, so it
  could free caches a live thread is still reading. And it would never run at
  all: the thread cells live in the young chain of the scratch `ProtoContext`
  that `ProtoSpace::newThread` never destroys, so they are never sweep
  candidates.

  So the release happens where the owner gives the resource up. On the exiting
  thread, in `thread_main`'s tail (`releaseExitingThread`), after it has left
  `runningThreads` and the threads list -- so it can block freely without
  holding the stop-the-world quorum, and no root scan can be walking its
  context. Order is load-bearing: the root context is destroyed **first**,
  because `~ProtoContext` is what submits the young generation (including the
  cells `removeAt` just allocated for the new threads list, which are reachable
  from `space->threads` and so were never garbage, merely unaccounted); the
  batch goes back **last**, via the new `returnUnusedCellBatch`, because until
  the context is gone the thread could still allocate from it. The
  `std::thread` object is released by `ProtoThread::join` -- the only place
  that can, since a thread cannot join itself and a completed join is the only
  proof protoCore ever gets that the OS thread is gone. A thread that is never
  joined still leaks its `std::thread`; that is the embedder's side of the
  contract.

  This also closes a latent GC hazard that the fix would otherwise have turned
  into a use-after-free. `ProtoContext`'s constructor registers every context
  it builds as the current context of its thread, or as `ProtoSpace::mainContext`
  when it has none; a new thread's root context has `previous == nullptr`, so it
  looked like a thread root and silently took over one of the **creating**
  thread's root slots. That was merely wrong while nothing deleted the context.
  `ProtoThreadImplementation`'s constructor now snapshots both slots and puts
  them back.

  Regression cover: `test/ThreadExitReleaseTests.cpp`, asserting the
  per-thread cell delta against a denominator and, one by one, that a joined
  thread holds no context, no batch, neither cache and no `std::thread`.

### Changed

- `ProtoThread::join` releases the `std::thread` object once the join has
  completed, and nulls it, so a second `join` on the same `ProtoThread` returns
  at the existing null check instead of touching a freed object.
- `ProtoThreadExtension::clearCachesAfterStopTheWorld` returns immediately when
  either cache is null, which is the state of a thread that has exited.

### Documentation

- `docs/GarbageCollector.md` section 7 gains a worked example of what the
  finalizer contract rules out, using the thread release as the case, and
  names the Phase 5b record-then-drain pattern as the escape hatch for release
  work that must publish.
- `docs/GarbageCollector.md` Known issues: the exiting-thread batch leak is
  struck out and its fix described; the `newThread` scratch-context leak, which
  remains, is written down for the first time -- it is a handful of cells per
  thread rather than a batch, and it is the reason a thread's release cannot be
  driven from a finalizer.

## [2.3.0] - 2026-09-25

Phase P4. Maintainer's instruction of 2026-09-25: *"add an audit phase for all
embedders that checks protoCore's rules"*, delivered as an executable suite
rather than a review; plus the maintainer's ruling, given during the phase, to
fix `ProtoThread::join` in the kernel instead of auditing every embedder for it.

**`PROTOCORE_ABI_SOVERSION` stays 3.** No class gained a member, no signature
changed, no return convention changed. The version moves 2.2.0 -> 2.3.0 for one
behaviour change and one new artefact, and `SameMajorVersion` consumers keep
working. But read the behaviour change below before linking an old embedder
binary against this library and assuming nothing moved.

### Fixed

- **`ProtoThread::join` now leaves protoCore's running set while it blocks**
  (`core/Thread.cpp`). This closes a **deadlock**, not slow shutdown.
  `runningThreads` starts at 1 -- the main thread is counted from `ProtoSpace`
  construction -- and every managed thread adds one, while a stop-the-world
  phase cannot begin until `parkedThreads >= runningThreads`. A bare
  `std::thread::join` reaches no safepoint, so a registered thread blocked there
  still counted as running: the quorum could never be met, no cycle could start,
  and every thread that then needed memory waited for a cycle that could not
  begin -- usually including the thread being joined, which is why the join never
  returned either. Measured in protoClojure: four blocking joins each hung to a
  90-second timeout and each completed in about three seconds once bracketed.

  No protoCore documentation stated the obligation, and `join` is protoCore's own
  blocking call, so an embedder had no way to know it had to bracket a kernel API
  against the kernel's own quorum. Wrapping the call in an `UnmanagedScope` as
  well remains harmless and idempotent (`unmanagedDepth` is a counter; only the
  outermost pair moves `parkedThreads`), so existing embedder guards need not be
  removed -- and four runtimes in this family have them.

  **The one exception, and it is the caller's bug:** inside a
  `ProtoContext::CriticalSection` (`criticalSectionDepth > 0`) `join` does NOT
  leave the running set, because the caller holds cells reachable only from C++
  locals and a root scan would miss them. Leaving would trade a deadlock for
  memory corruption, which is the worse trade. The join still happens, the quorum
  is still held for its duration, and a one-time diagnostic names
  `docs/EMBEDDER-CONFORMANCE.md` rule 12. Pinned by
  `ConformanceSelfCheck.JoinInsideCriticalSectionStillJoinsAndDoesNotPark`.

  This covers `ProtoThread::join` only. A runtime that calls `std::thread::join`
  or `pthread_join` directly on a thread it registered is still broken, and the
  kernel cannot see it -- which is why that stayed a conformance rule.

### Added

- **`libprotoCoreConformance` and `protoCore::conformance`** -- the embedder
  conformance suite: twelve executable cases driven through a
  `proto::conformance::Host` adaptor each runtime implements itself. Framework-free
  (cases return results as data), and protoCore never names a runtime -- both
  asserted by tests rather than intended.
- **`headers/protoCoreConformance.h`**, plus three-line GoogleTest and Catch2
  adapters and `PROTOCORE_CONFORMANCE_ISOLATE_MAIN`, all installed.
- **`scripts/conformance/check_static.py`** and `rules.json` -- the static half,
  as a per-repository ratchet with written justifications and a hash of each
  allowlisted line. The script tests itself: seven positive fixtures must fire
  and four negative fixtures must stay quiet.
- **`docs/EMBEDDER-CONFORMANCE.md`** -- the twelve rules as normative text, the
  per-function absent-value sentinel table, the three conforming shapes of rule
  11, and the three judgement items with what IS mechanised beside what is not.
- **`test/ConformanceSelfCheckTests.cpp`** -- rule 10 applied to the suite
  itself. Six deliberately non-conforming hosts each break exactly one rule and
  each must turn its case red; four more tests police the harness.

### Documented

- **`ProtoContext::safepoint()` is the only place a context's young generation is
  submitted.** The header documented it as the stop-the-world handshake hook and
  said nothing about submission, yet under `PROTOCORE_GC_REINCLUDE_SURVIVORS` it
  is the sole submission point. An embedder reading only that paragraph would
  conclude that a CPU-bound loop needs a safepoint and an allocating loop does
  not, which is the opposite of the truth for reclamation. That omission is a
  plausible contributing cause of two measured bugs: one runtime reclaimed 0
  cells of 2,748,398 across its whole history with 833 tests green, and another's
  apparent live set was 110x its real one.

### Known, and reported rather than fixed

- **`ProtoThread::getCurrentThread` and `ProtoSpace::getCurrentThread` are
  declared in `headers/protoCore.h` and defined nowhere.** An embedder that calls
  either gets an undefined reference at link time. Found while writing the
  rule-11 case, which now reads `space->threads` directly.

## [2.2.0] - 2026-09-25

Phase P3. Maintainer's ruling of 2026-09-24: *"hacer la internación global y la
lista de módulos como raíz"*, and, separately, that a module's identity in that
list is provider + path + version.

### Added

- **`globalSymbolTable()` and `globalSymbolCount()`** (`headers/proto_internal.h`,
  `core/SymbolTable.cpp`). One `SymbolTable` for the lifetime of the process,
  created on first use and deliberately never destroyed.
- **`ModuleIdentity`** (`headers/protoCore.h`, `core/ModuleIdentity.cpp`): a
  module's identity as provider GUID + logical path + version, rendered as one
  canonical string with `\x1F` between the components. `unversioned()`,
  `getProviderGUID()`, `getLogicalPath()`, `getVersion()`, `asKey()`,
  `operator==`.
- **`ModuleRootTable`** (`headers/proto_internal.h`, `core/ModuleRoots.cpp`) and
  `globalModuleRootTable()`: the process-global module list, and a GC root. 8
  shards, append-only chunks that never move, a per-shard published count, the
  `TupleInterner` capture/walk split, and `purgeSpace()` for teardown.
- **Four additive `ProtoSpace` methods**: `addModuleRoot`, `moduleRootCount`,
  `registerModule`, `findModule`. Non-virtual; no vtable and no layout change.

### Changed

- **All string interning is process-global.** `ProtoString::createSymbol` returns
  the same address for the same bytes in every `ProtoSpace` of the process.
  `ProtoSpace::symbolTable` keeps its type and slot and is now a **borrowed**
  pointer to the one global table, so all eleven existing
  `ctx->space->symbolTable` call sites are untouched and the mid-construction
  sentinel that six null checks in `core/ProtoObject.cpp` rely on still fires
  exactly when it used to.
- **`~ProtoSpace` no longer frees the symbol table.** The first space to die
  would otherwise free the table every other space of the process is still
  using. This fixes a leak rather than creating one: `~SymbolTable` frees only
  the `Bucket` nodes, never the symbol cells, so every destroyed space already
  leaked its whole symbol set. `delete tupleInterner` stays.
- **`SharedModuleCache` is keyed by `ModuleIdentity::asKey()`**, not by the bare
  logical path.
- **The cache probe moved INSIDE the resolution-chain loop**, one probe per
  entry, because the provider is not known until an entry is selected. Chain
  order is therefore now respected: a module already loaded from a later chain
  entry no longer shadows an earlier entry that can serve the same path.
- **The O(modules) stop-the-world module loop is gone.** GC Phase 2 calls
  `ModuleRootTable::captureForGC()` — 8 counter reads, no entry dereferenced —
  and GC Phase 4 pushes the captured entries after the world resumes, filtered to
  the collecting space's own entries. Measured on the same test with 2000 module
  roots: cumulative Phase 2 over three cycles 329 μs before, 138 μs after.
- **`ProtoSpace::moduleRoots` and `moduleRootsMutex` are retired** — held empty,
  never iterated, retained only so the layout does not change. Use
  `addModuleRoot()` for a module, or `createRootSet()` for anything that must be
  unpinned.
- **`getImportModuleImpl` builds its wrapper once**, for both the cache hit and
  the fresh load, and parents it to `space->objectPrototype` in both. The two old
  branches differed on that, so the same call returned a wrapper with a different
  prototype chain depending on whether the module happened to be cached already.
- **The `"Absolute fall back (rare or error)"` comment in
  `ProtoContext::allocCell` is corrected.** That branch is the perennial
  allocation path that `SymbolTable::intern` and `ProtoString::createSymbol`
  depend on by contract; describing it as an error path invited a future reader
  to delete it. The comment now also states the distinction the phase rests on: a
  perennial cell is never swept but also never **scanned**.

### Fixed

- **The same attribute name had a different address in each `ProtoSpace` unless
  it fitted in the pointer word.** An attribute key is the address of an interned
  symbol, and protoCore embeds a short ASCII string in the pointer word
  (`INLINE_STRING_MAX_BYTES == 6`), so a 5-byte name matched across spaces by
  accident while a 7-byte one missed with **no error at all** — `getAttribute`
  returned `PROTO_NONE`, which is also a legitimate value. Half-global identity
  with silent partial failure.
- **A module loaded through a provider called directly was rooted only inside the
  providing runtime.** It reached neither `SharedModuleCache` nor any
  `moduleRoots`, so destroying that runtime while an importer still held its
  values dropped the only anchor. `ProtoSpace::registerModule` closes it.
- **`provider:st/counter_lib` and a local `counter_lib` were one module**, because
  the cache key was the path with the provider prefix stripped, with the first
  load winning for both.
- **A `ModuleRootTable` entry could outlive the `ProtoSpace` it names**, and the
  allocator can hand a later `ProtoSpace` the same address, whose collector would
  then match the dead space's entries by owner and trace cells in a heap with no
  owner. `~ProtoSpace` calls `purgeSpace()` after its GC thread has been joined.
  Found by a test, not by argument.

### Unchanged, on purpose

- **Tuple interning stays per space**, with a test that keeps it that way
  (`GlobalInterning.TupleInternerStaysPerSpace`) and the reasoning in
  `TupleInterner`'s doc block. Intern globally what is keyed by **content**; keep
  per-space what is keyed by **address**. A symbol's key is its bytes, which are
  space-independent; a tuple's key is its element **addresses**, which are not, so
  a global tuple table would deliver no cross-space identity for the ordinary
  case at all — and the one case where it would alias (a tuple of
  now-globally-interned symbols built in two spaces) would put one collector into
  another's heap for no benefit.
- **Global interning does not make objects portable across spaces.** It fixes
  attribute *keys*. Prototypes, `PROTO_NONE`, the mutables tree and every
  per-space callback remain per space.

### ABI

`PROTOCORE_ABI_SOVERSION` **2 → 3**.

The `ProtoSpace` layout is **byte-for-byte identical** — no field added, removed
or reordered, and the public additions are one class and four non-virtual
methods. An `offsetof` program compiled against this tree's headers and against
the base commit's produces an empty diff.

The soname nevertheless moves, and that is the point: unlike every previous
protoCore change, **a stale embedder binary here links successfully and runs, and
is simply wrong about symbol identity in a multi-space process.** There is no
load-time error, no crash and no diagnostic — just a `getAttribute` that returns
`PROTO_NONE`. A soname bump converts that into a load-time error.

**A clean rebuild of every embedder is mandatory.** A stale binary links.

## [Unreleased]

### Added

- **CMake package configuration.** `install(EXPORT protoCoreTargets)` with the
  namespace `protoCore::`, a `protoCoreConfig.cmake` generated from
  `cmake/protoCoreConfig.cmake.in`, and a `SameMajorVersion`
  `protoCoreConfigVersion.cmake`. Consumers now use
  `find_package(protoCore 2.0 REQUIRED CONFIG)` and link
  `protoCore::protoCore`; the configuration also asserts that the library
  matching `SOVERSION` is present in the prefix, so a prefix whose CMake files
  outlived its library fails with a message instead of a link error. Before
  this, `install(TARGETS ... EXPORT protoCoreTargets ...)` named an export set
  that was never written out, so no consumer could tell 1.x from 2.x.
- **`lib/pkgconfig/protoCore.pc`**, generated from `cmake/protoCore.pc.in`,
  including a `soversion` pkg-config variable for consumers that are not CMake
  projects.
- **The NSIS installer records `Version`, `Soversion` and `InstallDir` under
  `HKLM\SOFTWARE\protoCore`**, so dependent Windows installers have something
  to test — a DLL carries no soname. Configured but unverified: no Windows host.

### Changed

- **`SOVERSION` is derived from the new `PROTOCORE_ABI_SOVERSION` variable**,
  which is also what the package configuration and `protoCore.pc` report, so a
  consumer's check and the file on disk cannot disagree.
- **The exported interface include directory uses `CMAKE_INSTALL_INCLUDEDIR`**
  instead of the hardcoded `include`, matching the install destination.
- **`CPACK_DEBIAN_PACKAGE_NAME` (`protocore`) and `CPACK_RPM_PACKAGE_NAME`
  (`protoCore`) are set explicitly** instead of relying on each generator's
  default casing. The resulting package names are unchanged.
- The Linux CPack branch now also prints a `STATUS` line when the DEB or RPM
  generator is *disabled*, so a packaging run that produced fewer artefacts
  than expected says why.

- **The two `ProtoMPSCQueue` stress tests now apply backpressure.** Both aborted
  on protoCore's OOM guard, and the diagnosis is that the tests, not the queue,
  were at fault: they pushed 8 x 1,000,000 (and 400,000) messages with no flow
  control at all, so the backlog — which is live, and which only the consumer
  can release — grew past whatever heap ceiling it was given.

  The evidence that decides it:

  * **The wall is not the queue's.** A control program with no queue anywhere,
    doing nothing but `ProtoContext::newList(n, items)` under the same
    412,144-cell ceiling, completes at n = 20,000 and runs out of memory at
    n = 30,000 — the same boundary, and the same reported live set (348,469
    cells), as the queue stress with the same in-flight bound. One `takeAll` of
    N items transiently allocates about `N*log2(N)` cells, because the bulk
    builder appends element by element and the whole path-copy trail stays in
    the consumer's young generation until the build ends.
  * **The failure tracks the ceiling, not the producer count.** Given 1.76 M
    cells the live set stops at 2.00 M; given 6.26 M it stops at 5.89 M. With
    the in-flight set bounded at 30,000, one producer fails exactly as eight do.
  * **Rate-limiting the producers removes it entirely.** The full 8 x 1,000,000
    run completes under the *same* 412,144-cell ceiling in 11.6 s with 398 GC
    cycles, 8,000,000 items consumed, none lost, none duplicated, per-producer
    FIFO intact.

  Recorded for the record, because it is real and shapes how an embedder must
  size a mailbox: once the backlog has filled the heap the system is in a
  genuine circular wait, and the OOM abort is the only exit. At the abort
  (gdb, `thread apply all bt`) all eight producers were parked in
  `ProtoSpace::waitForHeapHeadroom` inside `push`, the consumer was parked in
  the same wait inside `newList` inside `takeAll` — unable to allocate the list
  whose completion was the only thing that could have released the backlog —
  and the GC thread was idle with nothing to reclaim. **The heap-ceiling
  protocol is unchanged; sizing is the caller's job.** The rule the numbers
  give is that a mailbox's in-flight set must stay well under the point where
  `N*log2(N)` approaches the heap ceiling.

  Both tests now bound the in-flight set (10,000 items for the 8 x 1M stress,
  250 for the heavier mark-race probes), wait inside an `UnmanagedScope` so a
  throttled producer never delays a pause, and carry an abort flag so a
  consumer that gives up can never leave a producer blocked and hang the join.
  The in-loop `ASSERT`s that could return from the test body with producers
  still running are replaced by counters checked after the join. Two new guards
  keep the result honest: the collector must complete at least ten cycles, and
  the in-flight bound must actually have bound at least once — a bound raised
  until it stops binding would silently restore the unbounded test.

### Fixed

- **`ProtoMPSCQueue::takeAll` no longer holds the world stopped for the length
  of the batch it drains.** With `newList(n, items)` fixed (below), what was
  left of the pause was `takeAll`'s own chain walk and reversal: two O(batch)
  loops that make no protoCore call, and therefore never reach a
  stop-the-world poll, however far outside a critical section they run. The
  pause was still linear in the batch — measured medians of **338 us** at
  50,000 items and **3,731 us** at 400,000, against a flat 27-34 us for a
  plain bulk build of the same sizes. PMQ-SPEC section 3 constraint 1 (no
  stop-the-world work proportional to queue length) was not met.

  Both loops now call `parkForStopTheWorld` every 64 nodes — the park-only
  half of `safepoint()` that the `newList` fix factored out, matching
  `allocCell`'s every-64-allocations cadence. After the fix the same medians
  are **32 us** at 50,000, 32 us at 100,000, 31 us at 200,000 and **27 us** at
  400,000: flat, and level with the plain bulk builder. Constraint 1 is met.

  The poll is placed strictly *after* the publish window (read epoch → maybe
  release → load `head` → fill and publish the retain cell → detach) and after
  its `CriticalSection` has been destroyed. Nothing was added inside that
  window, which is what keeps ABA impossible by construction (PMQ-SPEC section
  7): reusing the address loaded from `head` would still require a sweep
  between that load and the CAS, hence a pause, hence this thread parking
  between them — which it still cannot do. Parking in the walk is safe because
  nothing the walk needs lives only in a C++ local: the nodes hang off the
  retain cell this `takeAll` already published onto `retained`, and the items
  hang off the nodes.

  `MPSCQueueGC.LargeDrainDoesNotBlockStopTheWorld` now measures two batch
  sizes a factor of four apart in one run and asserts the pause does not grow
  with the batch, which is what constraint 1 actually forbids; the previous
  single-size bound is kept as a sanity check. `parkForStopTheWorld` moved
  from an anonymous namespace in `core/ProtoContext.cpp` to a protoCore-
  internal declaration in `headers/proto_internal.h`. No public API or ABI
  change.

- **`ProtoContext::newList(n, items)` no longer holds the world stopped for the
  length of the list it builds.** The bulk builder wrapped its whole O(n) AVL
  construction in a `ProtoContext::CriticalSection`. A thread inside a critical
  section never parks, so the collector could not begin its stop-the-world
  phase until the last element was in: the pause grew with the size of the
  list. Measured on a 100,000-element build with a collection requested against
  it, the stop-the-world pause (phases P1 + P2) falls from a median of **39 ms**
  to **31 us**; the collector's own instrumented P1 total over a run falls from
  about 48 ms per cycle to under 0.25 ms per cycle. No embedder API changes and
  no ABI change.

  The section is replaced by an anchor: every intermediate of the build is
  parked in `ProtoContext::pendingRoot`, which the stop-the-world root scan
  reads, so a collection landing mid-build traces the spine the loop is
  standing on from a real root. The slot's previous occupant is saved and
  restored, the same discipline `ProtoObject::processOwnAttributes` uses. The
  heap-ceiling backpressure the section's constructor took at depth 0 is kept,
  at the same point in the control flow — before the first allocation, with
  nothing half-built — exactly as `newStringFromUTF8` keeps it.

  Callers are unaffected in what they may pass, with one contract made
  explicit: a collection can now run while `newList` is executing, so elements
  the caller supplies must be reachable from a GC root, as they must be around
  any other allocation. Elements freshly built in a live context are on that
  context's young chain and therefore already safe.

  Cost: within 1% at 10,000 and 100,000 elements; about 6% on a 1,000-element
  build (least-contended sample of 144), for the anchor store per element and a
  stop-the-world poll every sixteen.

  New tests in `test/BulkListBuildTests.cpp` cover both halves: elements
  survive collections forced during the build (single-threaded under a hard
  heap limit, and with three concurrent builders), a value named by
  `pendingRoot` survives once its context's young generation has been
  submitted, and the stop-the-world pause during a large build is bounded well
  below the duration of the build.

## [2.1.0] - 2026-09-23

### Added

- **`ProtoMPSCQueue`** — a mutable, lock-free, multi-producer /
  single-consumer FIFO of `ProtoObject*` items whose contents the collector
  traces (spec `protoScala/docs/platform/PMQ-SPEC.md`). `push` is lock-free,
  O(1) and allocates one cell; `takeAll` returns every queued item in push
  order as an immutable `ProtoList`; `isEmpty` is a snapshot. One pointer
  tag (28) for the handle, three `CellType`s, a dedicated prototype, and
  `ProtoContext::newMPSCQueue` / `ProtoObject::isMPSCQueue` /
  `ProtoObject::asMPSCQueue`.

  It is the shared actor mailbox of protoScala Phase 5, protoClojure and
  protoST, and it closes protoClojure's unrooted-payload defect: a queued
  message, its arguments and its reply future become GC roots.

  **It adds nothing to the stop-the-world pause** beyond one O(1) global
  prototype root, needs no write barrier and changes no collector phase.
  Correctness under concurrent marking rests on two orderings, proved and
  documented in `core/ProtoMPSCQueue.cpp` and `docs/GarbageCollector.md`:
  `processReferences` loads `head` before `retained`, and `takeAll`
  publishes its retain cell before it detaches a chain.

  Caller contract worth repeating: the queue must stay reachable for the
  duration of a call, and each producer turn should use its own
  `ProtoContext`, exactly as every other protoCore allocation does — a
  context owns its young generation until it is destroyed.

  > **SUPERSEDED — both open items below were closed in the same merge
  > (`f60baf11`).** The two blocks that follow are kept as the record of what
  > was true when `ProtoMPSCQueue` first landed, but neither still holds:
  > `takeAll`'s pause is no longer proportional to the batch (see "`takeAll` no
  > longer holds the world stopped for the length of the batch it drains" under
  > *Unreleased → Fixed*; PMQ-SPEC §3 constraint 1 is now **met**), and the two
  > stress tests no longer abort on the OOM guard (see "The two
  > `ProtoMPSCQueue` stress tests now apply backpressure" under *Unreleased →
  > Changed*; the fault was in the tests, not the queue). Read both blocks
  > below as history, not as current status.
  >
  > Note for the maintainer: the entries currently under `[Unreleased]` are all
  > ancestors of `f60baf11` and so shipped *as part of* 2.1.0. Whether to fold
  > them into this section or cut a 2.1.1 is a release-numbering decision left
  > to you; nothing in the code depends on it.

  **Known limitation (recorded when the queue first landed; now fixed — see the
  note above).** With the
  bulk-builder fix above in place, the stop-the-world pause during a
  200 000-item drain falls from a median of 88 ms (min 82 ms, max 330 ms
  over 9 samples) to about 2 ms (min 22 us, max 5.3 ms) —
  `MPSCQueueGC.LargeDrainDoesNotBlockStopTheWorld`. What remains is in the
  queue, not in the builder: `takeAll` walks the detached node chain into a
  vector and reverses it without making any protoCore call, so that loop
  never polls the stop-the-world flag and the pause is still linear in the
  batch (340 us at 50 000 items, 3.67 ms at 400 000, a flat ~0.7% of the
  drain, against a flat 27-34 us for a plain bulk build of the same sizes).
  PMQ-SPEC §3 constraint 1 was therefore **not met yet at this point**. `push`
  is unaffected, and a consumer that drains often keeps its batches small.
  (Closed later in the same merge: the two loops now poll every 64 nodes and
  the pause is flat at 20-36 us from 50,000 to 400,000 items.)

  **Also open at this point (since closed — see the note above).** On top of
  the new builder,
  `MPSCQueueConcurrency.EightProducersOneConsumerLoseNothingAndDuplicate-
  Nothing` and `MPSCQueueGC.PushAndTakeAllDuringConcurrentMarking` abort
  with protoCore's out-of-memory guard. The consumer stops draining (the
  probe in `.agent_scratch` shows `consumed` frozen at 5 732 while
  `produced` runs to 54 600) and the backlog fills whatever heap it is
  given: the live set at the abort tracks the ceiling (320 k cells at a
  302 k ceiling, 1.25 M at a 1.26 M ceiling). Before the builder fix these
  tests passed, but they were not testing what they claimed — with the old
  builder the collector completed **zero** cycles at 10 000 and 50 000
  pushes per producer and the heap ran to 2.5 M cells against a declared
  302 k ceiling, so the ceiling was never enforced. Whether the fix belongs
  in the queue, in the tests' unbounded mailbox under a hard cap, or in the
  heap-headroom back-pressure is a maintainer decision.

### Changed

- `ProtoSpace` gains one field (`mpscQueuePrototype`). The soname stays
  `libprotoCore.so.2`, so **every embedder must still be rebuilt from
  clean**: a stale binary would use the old layout.
- Pointer-tag budget: used 0-28 (29), free 29-63 (35).

### Verified

- **The mandatory clean rebuild of every embedder was carried out against this
  merge (`f60baf11`) on 2026-09-23** (PMQ-SPEC §6 step 2), each one confirmed by
  `ldd` to resolve `libprotoCore.so.2` from the workspace build and not the
  stale 1.0.0 in `/usr/local/lib`:

  | Project | Result | Baseline |
  |---|---|---|
  | protoCore | 438/438 | 438/438 |
  | protoPython | 582/583 | 560/561 (count has since grown) |
  | protoJS | ctest 34/34, conformity 5/5, test262 `built-ins/{Object,Reflect,Proxy}` 3619 passed | 33/33, 5/5, 3619 |
  | protoST | 833/833 | 833/833 |
  | protoClojure | 383/383 | 383/383 |
  | protoScala | 694/694 | 694/694 |

  No regressions. protoPython's single failure is the pre-existing
  `protopy_import_site`, which a `.pth` in a sibling `venv/` triggers. The
  test262 subset matches its baseline exactly with **no newly failing test**;
  two tests newly pass, from protoJS's own integrity-levels fix.

- **protoScala's mailbox seam now selects the queue.** With `newMPSCQueue`
  present in `headers/protoCore.h`, protoScala's configure reports "actor
  mailboxes on protoCore ProtoMPSCQueue", `protoscala --version` reports
  `(actor mailboxes: ProtoMPSCQueue)`, and `nm -uC` shows the binary
  referencing `ProtoMPSCQueue::push`, `takeAll`, `isEmpty`, `asObject`,
  `ProtoContext::newMPSCQueue` and `ProtoObject::asMPSCQueue`. The CAS-list
  fallback is no longer compiled in. PMQ-SPEC §6 step 3 is unblocked.

## [2.0.0] - 2026-09-23

A major release that merges two independent lines of work:

- **`ProtoMap`** and the shared hashed-collection helper — the persistent map
  from language objects to values every runtime on the platform was building
  for itself (`feature/pslo-p1`, spec
  `protoScala/docs/platform/PROTOMAP-SPEC.md`).
- **The parent-chain lookup fixes** — `isInstanceOf`, `hasParent`,
  `hasAttribute`, `getAttributes`, `getAttribute`, `newChild` and `setParents`
  (`feature/descendant-of`, spec
  `protoScala/docs/platform/ISINSTANCEOF-FIX.md`).

### Upgrading from 1.2.0 — read this first

**The ABI version goes from `SOVERSION 1` to `SOVERSION 2`** (library
`libprotoCore.so.2.0.0`, soname `libprotoCore.so.2`). The `ProtoSpace` layout
changed (a new `mapPrototype` root and the concurrent-mark tables) and the
lookup and `setParents` semantics changed, so **every embedder must be rebuilt
from clean**. The soname bump is deliberate: a stale embedder binary now fails
to load with a missing-soname error instead of linking against an
incompatible layout and crashing at some later, unrelated point.

Four behaviour changes are visible to embedder code that was correct against
1.2.0. None of them is a bug being reintroduced; each is a previously wrong
answer becoming right, and each can change what an embedder observes:

1. **`setParents` flattens the chain.** The installed chain is now the listed
   parents, de-duplicated and in the given order, followed by every ancestor
   of each listed parent that is not already present (each parent's own chain
   order, in listed-parent order). Previously the chain held only what the
   caller listed. Consequences: `getParents()` on the result returns MORE
   entries than were passed (a caller that compares its list against
   `getParents()` for equality will now see a longer list); attribute
   precedence between two ancestors that were previously only reachable
   through different intermediate parents is now decided by this flattened
   order; and an object's ancestors are all directly visible to
   `hasParent`/`isInstanceOf` without a recursive walk. A list that already
   contains every ancestor of every listed parent (a full linearization)
   installs exactly as given — for those callers the change is a no-op.
2. **`setParents` with the receiver among the new parents is a silent no-op
   for that entry** instead of throwing `std::invalid_argument`. Nothing in
   the embedders catches that exception, and the check it replaced only ever
   caught a direct one-hop self-reference, never a longer cycle — skipping is
   exactly as complete a guard, without an exception embedders must catch.
3. **`isInstanceOf` and `hasParent` see all parents, with no step cap, and
   resolve a mutable receiver's current snapshot.** The old `isInstanceOf`
   gave up after 50 steps and answered `PROTO_FALSE`, and both read a mutable
   object's birth-state chain, so parents added later through
   `addParent`/`setParents` were invisible. Deep hierarchies and mutable
   receivers now answer correctly; code that relied on the old `false` (for
   example as a depth guard) will see `true`. The same cap removal applies to
   `getAttribute`, `hasAttribute` and `getAttributes`, which now merge the
   whole flattened chain.
4. **`newChild` captures the prototype's CURRENT chain.** A child of a mutable
   prototype used to inherit the prototype's birth-state ancestors; it now
   captures whatever the prototype's chain is at the moment `newChild` is
   called.

**Known embedder breakage, to be fixed in those repositories, not here:**

- **protoJS** — its parent-chain integrity markers assume `setParents`
  installs exactly the list it was given, so the flattened chain trips them.
- **protoPython** — its metaclass isolation relies on `newChild` capturing the
  prototype's birth-state chain and on `setParents` not flattening; with the
  corrected semantics a metaclass's ancestors become visible on instances that
  previously did not see them.

Both are consequences of protoCore answering correctly where it used to answer
wrongly; the fixes belong in protoJS and protoPython.

### Added
- **`ProtoMap`** — a persistent AVL map
  identical to `ProtoSparseList` except that its key is a `const ProtoObject*`
  the garbage collector traces: an object referenced only as a key stays
  alive. Keys are ordered and compared by their word (identity, tag included);
  embedded values (SmallInteger, booleans, chars, None) are valid keys and are
  never traced; `nullptr` is not a valid key. `getAt` returns `nullptr` for an
  absent key, so a stored `PROTO_NONE` stays distinguishable. Small inline
  form up to three entries, AVL beyond, exactly like `ProtoSparseList`, whose
  algorithms it shares through `core/SparseListAlgorithms.h`
  (`ProtoSparseList`'s behaviour, API, ABI and performance are unchanged).
  New API: `ProtoContext::newMap`,
  `ProtoObject::isMap` / `asMap`,
  `ProtoSpace::mapPrototype`, `ProtoMapIterator`.
  Uses one new pointer tag (27); the tag table now records the platform tag
  budget and the maintainer-approval rule. ABI change: every embedder must be
  rebuilt. Specification: protoScala/docs/platform/PROTOMAP-SPEC.md. The
  performance gate and the ASan run, left pending on the branch, were both
  run at merge time — see "Performance gate and sanitizer run (2026-09-23)"
  under Performance below.
- **Hashed-collection helper** — `KeySemantics`, `hashedPut`, `hashedGet`,
  `hashedRemove`, `hashedForEach` over `ProtoMap`: identity keys
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
- **`ProtoObject::setParents` self-reference is now a silent no-op
  (the offending entry is skipped) instead of throwing
  `std::invalid_argument`; the true termination argument was corrected
  in every place it was documented.**

  Two separate problems, one fix. First: nothing in the embedders this
  branch was validated against catches `std::invalid_argument` from
  `setParents`, and it is reachable from ordinary code — e.g.
  protoPython's metaclass-resolution fallback does not re-check
  `metacls != targetClass` before a `setParents`-based rebuild, so a
  pattern that ends up there can raise uncaught. Second: the check this
  replaces was never a complete cycle guard to begin with — it only ever
  caught a DIRECT reference back to the receiver (a listed parent equal
  to it, or an ancestor found while walking a LISTED parent's own
  one-level chain). It does not, and structurally cannot without
  unbounded work, catch a longer chain of references built up across
  several SEPARATE `setParents` calls on different mutable objects:
  `a.setParents(ctx,[b])`, then `b.setParents(ctx,[c])`, then
  `c.setParents(ctx,[a])` — none of these three individual calls sees
  enough to reject the third. Continuing to throw for only the narrower,
  directly-detectable case was therefore incomplete protection with all
  of the uncaught-exception downside.

  Skipping the offending entry — omitting it from the flattened list,
  while every OTHER listed parent/ancestor is still applied normally —
  is exactly as complete a guard as throwing was (it prevents THE SAME
  set of direct cases; the longer, cross-call case was never caught
  either way), without the exception. It also matches `addParent`, which
  already tolerates `obj->addParent(ctx, obj)` as a silent no-op (via
  `hasParent`'s `target == this` short-circuit). A single-entry list
  whose one entry is the receiver itself ends up empty, same as passing
  an empty list directly — already-documented `setParents` behaviour, not
  a new case.

  **The true termination/safety argument** (corrected everywhere the old,
  false one was written down — the header doc comments for
  `getAttribute`/`setParents` and this file): every chain-lookup method
  (`getAttribute`, `hasAttribute`, `isInstanceOf`, `hasParent`,
  `getAttributes`) is a single-level walk of ONE receiver's own,
  already-built `ParentLinkImplementation` list — built once, forward
  only, by `newChild`/`addParent`/`setParents`, never mutated afterward —
  and none of them ever follows a visited entry into THAT entry's own
  separate chain. Termination therefore never depended on "no
  self-reference of any shape can exist" (false, as the three-object
  example above demonstrates); it depends only on each individual list
  being finite, which is guaranteed by how it was built, regardless of
  what any OTHER object's chain happens to reference.

  Tests: `test/SetParentsFlattenTests.cpp` — the direct and
  two-mutable-object self-reference cases now assert a skip, not a
  throw (`MutualMutableSelfReferenceIsSkipped`,
  `DirectSelfReferenceIsSkippedLeavingAnEmptyChain`,
  `SelfReferenceAmongOtherParentsOnlySkipsItself`,
  `ImmutableReceiverInItsOwnNewParentsListIsNotSkipped`), plus a new
  `ThreeObjectCycleIsNotDetectedButCausesNoHarm` reproducing the
  three-`setParents`-call case above and confirming it does not crash,
  hang, or make any of the four lookup methods report a false ancestor.

- **`ProtoObject::getAttributes` aborted the process on a non-object
  parent — e.g. a heap `ProtoString` added via `addParent`, or a
  SmallInteger installed via `setParents` (neither is rejected at
  construction time: `addParent` only rejects an EMBEDDED value, and
  `setParents`'s own flattening has no tag filter at all). Its rewrite
  (an earlier round in this branch) called
  `toImpl<const ProtoObjectCell>(ancestor)` on every chain entry with no
  tag check first; a non-object tagged pointer does not address a
  `ProtoObjectCell`-shaped `Cell`, so dereferencing it through that cast
  aborted the process (a debug-build `toImpl` type assertion, `SIGABRT`;
  a release build would corrupt memory instead).**

  Fixed by mirroring `getAttribute`'s own, already-correct policy for
  this case (its chain-navigation loop already redirects a non-object
  `currentPointer` to `currentPointer->getPrototype(context)`, one hop,
  rather than dereferencing it as an object): a non-object chain entry in
  `getAttributes()` now contributes its OWN prototype's OWN attributes —
  one hop, not the prototype's further chain — instead of crashing.
  `isInstanceOf`/`hasParent` were never at risk (they only ever compare
  chain-entry pointers, never dereference one as a `ProtoObjectCell`);
  `hasAttribute` already had the same non-object handling `getAttribute`
  has, being a direct port of its chain-navigation loop.

  `flattenParentsOrder`'s own policy is stated explicitly where the
  asymmetry lives: step 1 (the listed parents) accepts any entry
  regardless of tag, matching `addParent`/`setParents`; step 2 (walking
  each listed parent's OWN chain) cannot walk a non-object entry's chain
  (it does not have one), so it is skipped as a source of further
  ancestors there — but is NOT removed from the flattened list step 1
  already added it to.

  Tests: `test/NonObjectParentTests.cpp` (8 cases) — a heap-string
  parent via `addParent` and a SmallInteger parent via `setParents`, then
  `getAttributes`/`getAttribute`/`hasAttribute`/`isInstanceOf` against
  both, confirmed not to crash and to answer through the entry's own
  prototype, plus a not-found lookup past a non-object entry still
  terminating cleanly.

- **`ProtoObject::newChild` resolved a mutable prototype's snapshot
  BEFORE opening its `ProtoContext::CriticalSection`, leaving the
  resolved snapshot unprotected across a GC park.**

  `CriticalSection`'s constructor calls `heapLimitCheckpoint()` at the
  outermost nesting depth, which can block waiting for a GC cycle when
  the heap is over its configured limit (`PROTOCORE_HEAP_LIMIT_CELLS`).
  `newChild` called `resolveOwnCell(context, this)` — which can itself
  resolve a mutable snapshot — and held the result in a C++ local (`oc`)
  across that constructor call. If a GC cycle ran during that park and
  nothing else kept the resolved snapshot reachable, `oc` could be left
  dangling by the time the critical section's body dereferences
  `oc->parent`. `getParents`/`getFirstParent`/`getAttributes` already
  open their `CriticalSection` before resolving anything, precisely to
  avoid this; `newChild` now does too — the same three-cell allocation
  it always protected is still covered, just with the mutable-snapshot
  resolve moved inside the section as well.

  This needs `PROTOCORE_HEAP_LIMIT_CELLS` set low enough to actually
  trigger a checkpoint park during the window between resolve and use to
  manifest, which made it impractical to turn into a deterministic
  regression test in the time available for this round; flagging this
  here rather than shipping a flaky or non-reproducing one.

- **`ProtoObject::getAttribute` no longer caps its chain walk at 500
  steps — no lookup or traversal method in protoCore has a depth cap any
  more.**

  `getAttribute` gave up (`iterationCount > 500`) and returned
  `PROTO_NONE` ("not found") for an attribute living further than 500
  own-chain entries from the receiver — a false negative for a perfectly
  good hierarchy, the same class of bug `isInstanceOf`'s old 50-step cap
  and `hasAttribute`'s old 50-step cap both had (both already fixed in
  earlier rounds). This was the one depth cap left in any lookup path,
  and the one remaining place `getAttribute` could disagree with
  `isInstanceOf`/`hasParent`/`hasAttribute`/`getAttributes` (all already
  uncapped). It is gone: all five now always agree, at any depth.

  Termination without a cap is guaranteed by construction, not by a
  limit, and was already true before this fix — removing the cap adds no
  new risk: every one of these walks only ever follows ONE receiver's
  own, already-built `ParentLinkImplementation` list (built once, forward
  only, by `newChild`/`addParent`/`setParents`, never mutated afterward),
  and never follows a visited entry into THAT entry's own separate
  chain — so it is always a single forward pass over one strictly finite
  list, regardless of what any OTHER object's chain references. (This
  replaces an earlier, incorrect version of this paragraph that claimed
  `setParents` rejects any input that could create a cycle at all — it
  does not; see the self-reference entry above for what it actually
  catches. The argument this paragraph needs never depended on that
  claim: it only needs that a single walk never crosses from one chain
  into another, which was true throughout.)

  Surveyed every lookup/traversal path in `core/*.cpp` for any other
  step/depth-limit constant: none remain. The one numeric bound left
  anywhere near an attribute walk is `processOwnAttributes`'s
  `kMaxDepth = 64` stack for its OWN in-order AVL traversal — a
  different kind of bound entirely: it is not a parent-chain depth cap
  (it never gives up on the SEARCH; it bounds the recursion-free
  in-order walk of ONE object's own attribute tree), and it is not
  arbitrary — the sparse list's `size` field is 24 bits, so a balanced
  tree of that many entries is at most ~35 deep, making 64 a
  mathematically safe upper bound, never an approximation that could be
  exceeded by a legitimately larger hierarchy.

  Tests: `test/GetAttributeNoCapTests.cpp` (5 cases) — the old cap pinned
  first (depths 501 and 2000 both returned `PROTO_NONE` for a root
  attribute against the pre-fix code, confirmed before removing the
  cap), then found at depth 501 and depth 2000, agreement with
  `hasAttribute`/`isInstanceOf`/`getAttributes` on the same 900-level
  chain, a not-found lookup on a 2,000-level chain still terminating as
  `PROTO_NONE`, and shallow own/inherited baselines. Also updated
  `test/HasAttributeChainTests.cpp`'s `DivergesFromGetAttributeBeyond500
  Levels` (renamed `AgreesWithGetAttributeBeyond500Levels`) now that the
  divergence it pinned no longer exists.

- **`ProtoObject::getAttributes` (the merged-attribute-view snapshot) now
  walks the receiver's whole flattened chain instead of recursing into
  only the first parent link — a second or later DIRECT parent's
  attributes are no longer silently dropped from the merge.**

  `getAttributes()` recursed as `pl->getObject(context)->getAttributes
  (context)` on `oc->parent` — the FIRST link of the receiver's own
  chain — and never followed `pl->getParent(context)` (the chain's
  remaining entries) at all. For an object built via more than one
  `addParent` call (a diamond) or via `setParents` with more than one
  listed parent, every attribute that lived only on the second-or-later
  parent was invisible through `getAttributes()`, even though
  `getAttribute`/`hasAttribute`/`isInstanceOf` (all fixed in earlier
  rounds to walk the receiver's own chain directly) already saw it — the
  three disagreed.

  `getAttributes()` is now the same iterative walk of the receiver's own
  chain those methods use, with an explicit **merge order (shadowing
  rule)**, stated in its header doc comment: own attributes first, then
  the chain head to tail; a key already set by a nearer entry is never
  overwritten by a farther one. This is exactly `getAttribute`'s/
  `hasAttribute`'s own "first match wins" precedence, so all three always
  agree on which value a key resolves to — including the ordering
  divergence between `addParent` (interleaves a parent's own ancestors
  right after that parent) and `setParents` (batches all missing
  ancestors after all listed parents), which now produces the same
  `getAttributes()` result as `getAttribute` in both cases. No step cap
  (unlike `getAttribute`'s 500-step one — the chain is walked in full),
  and no more C++ recursion depth proportional to chain length either
  (the old recursive-into-first-parent shape, applied to a very deep
  single-parent-per-level chain, would recurse once per level).

  Tests: `test/GetAttributesMergeTests.cpp` (10 cases) — the old
  first-parent-only bug pinned first (an `addParent` diamond and a
  multi-parent `setParents` list both dropped the second parent's
  attribute against the pre-fix code, confirmed before writing the fix),
  then own-attributes-only and single-parent-chain baselines, the
  shadowing precedence (own over any ancestor, nearer over farther), the
  exact `addParent`-vs-`setParents` ordering-divergence case agreeing
  with `getAttribute`, a 1,000-level single-parent chain, a mutable
  receiver, and agreement with `getAttribute`/`hasAttribute` on a
  diamond.

- **`ProtoObject::hasAttribute` now walks the flattened chain the way
  `getAttribute` does, allocation-free and with no step cap — fixing the
  same class of false-negative bug `isInstanceOf` had.**

  `hasAttribute` used a fixed-size (64-slot) sibling-stack DFS with an
  arbitrary 50-step cap and returned `PROTO_FALSE` — a false negative —
  for any hierarchy deeper than 50 links. It is now the same linear
  chain-navigation loop `getAttribute` uses (own attributes, then the
  chain head to tail), minus `getAttribute`'s attribute cache and its
  500-step cap: `hasAttribute` has no cap at all, and resolves a mutable
  receiver (and every mutable object visited along the chain) to its
  current snapshot exactly as `getAttribute` and the already-fixed
  `isInstanceOf`/`hasParent` do.

  At the time of this fix `getAttribute` still had its own separate
  500-step cap, so the two were not guaranteed to agree for very deep
  hierarchies; that cap is gone too now (see the later entry in this
  file) and they always agree, at any depth.

  Surveyed the other attribute-lookup helpers for the same defect:
  `hasOwnAttribute`, `getOwnAttributeDirect` and `processOwnAttributes`
  only ever probe the receiver's OWN attributes — no chain walk, no
  defect possible. `getAttributes()` (the merged-view snapshot) does walk
  the chain, but via true recursion into only the FIRST parent link — a
  different bug shape (missing siblings, not a step cap), fixed in a
  later round (see the entry near the top of this section, which also
  covers a separate crash the same rewrite introduced).

  Tests: `test/HasAttributeChainTests.cpp` (13 cases) — the old cap
  pinned first (a 60- and a 520-level chain both returned `PROTO_FALSE`
  against the pre-fix code, confirmed before writing the fix), then own/
  inherited/absent/`None`-valued baselines, an `addParent` diamond, chains
  past 50 and past 500 levels, agreement with `getAttribute` within its
  cap, the documented divergence beyond it, a mutable receiver (plain and
  via `newChild`), and a non-object receiver answered through its
  prototype.

- **`ProtoObject::newChild` now captures a MUTABLE prototype's CURRENT
  chain, not its birth-time chain — fixing instances of a mutable class
  that was re-parented after the class was made mutable.**

  `newChild` read the prototype handle cell's own `parent` field directly.
  For a mutable object that field is fixed at `newObject(true)` time and
  never updated in place (`addParent`/`setParents` publish a fresh state
  into the mutable shard instead), so `cls = newObject(true);
  cls->setParents(ctx, [base]); inst = cls->newChild(ctx)` silently built
  `inst` with NO ancestors at all: `inst->isInstanceOf(ctx, base)` was
  `PROTO_NONE` and `inst->getAttribute` never found any of `base`'s
  attributes. `newChild` now resolves the prototype to its current
  snapshot first (the same resolution `getAttribute`/`getParents`/
  `hasParent`/`isInstanceOf` already used), matching how every other
  chain-reading method treats a mutable object.

  The child's chain tail is still captured BY VALUE, once, at the moment
  of the `newChild` call: an instance created BEFORE a later re-parenting
  of its class does NOT retroactively gain the new ancestor; only
  instances created AFTER do. This is ordinary "capture at creation time"
  semantics, and matches the shape protoST's `addBehavior:` mechanism
  documents relying on for its own "future instances" contract
  (`protoST/src/primitives/object_prims.cpp`, the D21 "DOCUMENTED
  LIMITATION" comment) — that mechanism rebuilds the class as a fresh
  object rather than mutating an existing one, so it never depended on
  `newChild` observing a mutation of an EXISTING class object and is
  unaffected by this fix either way. What this fix DOES retire is the
  narrower "PROTOCORE CONSTRAINT" documented a few lines above that
  comment in the same file: mutating an EXISTING mutable class directly
  via `addParent`/`setParents` used to be invisible to instances created
  AFTER the mutation too (not only ones created before) — that half of the
  documented constraint no longer holds.

  Tests: `test/InstanceOfHasParentTests.cpp` — a `setParents`-built mutable
  class seen by a `newChild` instance, protoPython's own pattern
  (`newObject(true)` → `addParent` → `newChild`, both immutable and
  mutable children), and a mutable class re-parented after an instance
  already exists (the existing instance keeps the old ancestor, a new
  instance created afterwards gets the new one).

- **`ProtoObject::isInstanceOf` lost the "parentless object is an instance
  of `objectPrototype`" answer when its DFS-removal rewrite stopped
  bootstrapping from `getPrototype()` — restored.**

  A plain object with no parent chain of its own (never `newChild`'d,
  `addParent`'d or `setParents`'d anything) is, by convention, considered
  a descendant of the universal root `space->objectPrototype` —
  `getPrototype()` has always returned `objectPrototype` for exactly this
  case. The linear-walk rewrite searched the receiver's own chain directly
  and, for a chain-less receiver, found nothing and answered `PROTO_NONE`
  instead. `isInstanceOf` now applies the same fallback `getPrototype()`
  does, but ONLY at the top level for the receiver itself (an object WITH
  an explicit chain of its own is not implicitly rooted at
  `objectPrototype` unless its own construction put it there), and never
  for `objectPrototype` asking about itself (an object is not its own
  instance).

  Test: `test/InstanceOfHasParentTests.cpp` (`ParentlessObjectIsInstanceOf
  ObjectPrototype`, `ObjectPrototypeIsNotItsOwnInstance`,
  `ObjectWithExplicitChainIsNotImplicitlyRootedAtObjectPrototype`).

- **`setParents`'s DEDUPLICATION check is no longer O(n²), and flattening
  no longer allocates or re-runs inside the mutable CAS retry loop.**
  **Correction: this is narrower than an earlier version of this entry
  claimed** — see the numbers below; a `setParents` call whose listed
  parents each carry substantial ancestry of their own is still
  quadratic overall, just with a far smaller constant factor.

  The de-duplication check the flattening algorithm added (an entry
  equal to one already kept is dropped) was a linear scan of the
  accumulator built so far — O(n) per candidate, O(n²) total for n
  candidates that share no ancestry — running inside a GC critical
  section, and for a mutable receiver, inside its CAS retry loop (so a
  contested retry redid the whole O(n²) computation from scratch, even
  though the flattened chain never depends on the receiver's own current
  state). Measured before this fix: roughly 3 ms for a 4,000-entry list
  of independent (no shared ancestry) candidates.

  Fixed by: (1) a small open-addressing pointer set for the dedup check —
  O(1) amortised per candidate instead of O(current-size); (2) an inline-
  capacity-then-heap-fallback buffer (`SmallVector`/`ObjectPointerSet`,
  256/512 inline slots) for both the ordered accumulator and the dedup
  set, so the common case (a parent list of a few dozen to a couple
  hundred entries) never touches the heap at all; (3) moving the entire
  flattening computation (both the dedup pass and the ancestor-walk pass)
  OUTSIDE any GC critical section — it allocates no Cell, so it needs no
  GC protection — and outside the mutable receiver's CAS retry loop
  entirely, so a contested retry only rebuilds the cheap wrapping
  `ProtoObjectCell`, not the flattened chain.

  **What this fixes, and what it does not.** For N listed parents that
  share NO ancestry (step 2 does ~no work; the old cost was almost
  entirely the dedup scan in step 1), re-measured after this fix
  (average of 3 runs, `newObject(false)` release build):

  | N (listed parents) | before (reported) | after, immutable | after, mutable |
  |---:|---:|---:|---:|
  | 100  | — | ~0.008 ms | ~0.007 ms |
  | 1000 | — | ~0.12 ms  | ~0.10 ms  |
  | 4000 | ~3 ms | ~0.6–0.9 ms | ~0.5–0.7 ms |

  This shape is now roughly linear (a 4–6× improvement at N=4000). But
  step 2 itself — walking every LISTED parent's own chain to collect its
  ancestors — is inherent work: it must visit every (parent, own-chain-
  entry) pair at least once, and each check is now O(1) instead of O(n),
  but the NUMBER of pairs is not reduced. For N listed parents that
  together already form a complete linearization (e.g. N objects
  P₁..P_N with Pᵢ = Pᵢ₋₁.newChild(), listed in full as
  [P_N, ..., P₁] — every entry's own ancestors already listed elsewhere,
  the realistic "pass an existing MRO to setParents" shape), the total
  work is Σᵢ (i-1) = Θ(N²) checks regardless of the dedup fix. Measured
  (average of 3 runs, same build):

  | N (full linearization) | setParents time |
  |---:|---:|
  | 250  | ~0.56 ms |
  | 500  | ~2.1 ms  |
  | 1000 | ~8.5 ms  |
  | 2000 | ~43.5 ms |

  Each doubling of N costs roughly 4×: quadratic, as expected — the dedup
  fix made each of the Θ(N²) checks O(1) instead of O(current-size), a
  large constant-factor win (illustrated by the independent-parents
  table above), but it did not, and could not, change the Σᵢ shape this
  input pattern inherently requires.

- **Documented, explicitly, that `setParents` and `addParent` place a
  listed parent's own missing ancestors in DIFFERENT positions, which can
  flip attribute lookup precedence.** `setParents` appends ALL missing
  ancestors AFTER ALL listed parents; `addParent`, called once per parent,
  interleaves each call's own missing ancestors immediately after that
  call's parent — so `d.addParent(ctx, b); d.addParent(ctx, c);` and
  `d.setParents(ctx, [c, b])` (same listed parents, same order) can
  produce chains that disagree on which of two candidate ancestors'
  attributes wins. See `addParent`'s and `setParents`'s header doc
  comments for the worked example, and
  `test/SetParentsFlattenTests.cpp`'s
  `OrderingDiffersFromAddParentAndAffectsAttributePrecedence` for a case
  where the two constructions produce different `getAttribute` results
  for the exact same set of parents and ancestors.

- **Corrected the "`hasParent` doesn't see a mutable object's children's
  full ancestry" gap** — that gap was exactly the `newChild` bug fixed
  above; `hasParent` itself was already correct (it resolves a mutable
  receiver's current snapshot, and always did), it was just fed an
  incomplete chain by the old `newChild`. With that fixed, `hasParent`
  (like `isInstanceOf`) now answers the full, transitive ancestry
  question for every object, including instances of a mutable class. (At
  the time of this entry `getAttribute` still had a separate 500-step
  cap that was the one remaining gap; removed in a later round (see
  the entry near the top of this section).)

- **`ProtoObject::setParents` now flattens the chain it installs, so
  `ProtoObject::isInstanceOf` is a pure linear walk with no recursion, no
  cap and no allocation — for every object, with no exceptions.**

  This supersedes the entry below: `setParents` was, until now, the one
  construction path that did not flatten (it installed exactly the given
  list, without copying in each listed parent's own ancestors), which is
  why `isInstanceOf` originally needed a recursive fallback for chains it
  had touched. It no longer does.

  **`setParents`'s new chain**, in order: (1) the entries of the given
  list, in the given order, de-duplicated; (2) every ancestor of each of
  those listed parents — walking each parent's own chain in that parent's
  own order, in the same order the parents were listed — that is not
  already present. A list that already contains every ancestor of every
  listed parent (e.g. a full linearization) is unaffected by step 2 and
  installs exactly as given, in the exact same order — a no-op relative to
  the old verbatim behaviour.

  **Behaviour change**: `getAttribute` (and `hasParent`, and
  `isInstanceOf`) can now see a grand-parent's attribute through a
  `setParents`-built object that could not see it before — e.g.
  `x = obj.setParents(ctx, [p])` where `p` has its own ancestor `g` with
  attribute `a`: `x.getAttribute(ctx, a)` used to return `PROTO_NONE`
  (only `p`'s own attributes were visible) and now finds `g`'s value,
  because `g` is flattened into `x`'s own chain alongside `p`. This is
  intended: it is the same completeness `newChild`/`addParent` already
  guaranteed, now extended to `setParents`, and it is why
  `isInstanceOf`/`hasParent`/`getAttribute` now agree on what an object
  inherits, regardless of which construction path built its chain — WITH
  ONE EXCEPTION, corrected in a later round (see the entry near the top
  of this section): `getAttribute` used to still cap its walk at 500
  steps, so it could still disagree with the other two for a very deep
  chain.

  **Self-reference handling** (this originally threw
  `std::invalid_argument`; corrected to a silent skip in a later round —
  see the entry near the top of this section for why): a mutable object's handle is stable
  across mutation, so it is the only case where `setParents` could be
  asked to make an object its own ancestor — directly
  (`a.setParents(ctx, [a])`) or through another mutable object's chain
  (`a.setParents(ctx, [b])` then `b.setParents(ctx, [a])`, where `a`'s
  chain now contains `b`). An immutable `setParents` call can never
  create a real self-reference this way — it always builds a brand-new
  handle nothing could have referenced yet.

  Tests: `test/SetParentsFlattenTests.cpp` (10 cases: the no-op
  linearization property, a non-flat list being flattened, listed-parent
  order preservation with overlapping ancestors, de-duplication of a
  repeated listed parent, the `getAttribute` visibility change, agreement
  between `isInstanceOf`/`hasParent`/`getAttribute`, a mutable object
  after `setParents`, self-reference handling (a two-mutable-object case
  and a direct case), and an immutable receiver listing its own old
  handle — not a self-reference).

- **`ProtoObject::isInstanceOf` and `ProtoObject::hasParent` now walk the
  flattened parent chain directly instead of allocating or capping the
  search.**

  `isInstanceOf` used a depth-first walk with a fixed-size (64-slot)
  sibling stack and gave up after an arbitrary 50-step cap, returning
  `PROTO_FALSE` (a third, distinct value, never documented as part of the
  found/not-found contract) instead of the correct answer for any hierarchy
  deeper than 50 links. `hasParent` allocated a `ProtoList` via
  `getParents()` on every call just to test membership.

  `newChild`, `addParent` and (as of the entry above) `setParents` all
  guarantee that an object's own chain already contains every one of its
  ancestors as a direct entry, so `isInstanceOf` is now a single
  allocation-free linear scan of that chain, the same one `getAttribute`
  walks, with **no length limit and no recursion**. `hasParent` keeps its
  existing single-level contract (`target == this`, or a direct entry in
  the receiver's own chain) and is now just that scan without the
  `ProtoList` allocation — which, now that every chain is flat by
  construction, agrees with `isInstanceOf` on every ancestor, not only a
  direct one.

  Fixing this surfaced and corrected two bugs the old implementation had:
  `isInstanceOf` explored a receiver's own chain only through
  `getPrototype()`, which returns just the first entry, so a second or
  third parent added via `addParent` (e.g. the classic diamond,
  `chain=[C,B,A]`) was silently unreachable even though `hasParent`
  correctly reported it present; and `isInstanceOf` never resolved a
  mutable receiver's current snapshot (`getPrototype()` does not), so it
  answered false for every parent ever added to a mutable object. Both are
  an unavoidable consequence of scanning the receiver's own resolved chain
  directly instead of bootstrapping from `getPrototype()`.

  Tests: `test/InstanceOfHasParentTests.cpp` (15 cases, covering every
  chain-shaping construction path: `newChild`, `addParent` including the
  diamond case, `setParents`, `clone`, mutable objects after
  `addParent`/`setParents`, non-object receivers, and a 1,000-level
  `newChild` chain that used to hit the 50-step cap and now correctly
  returns `PROTO_TRUE`).
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
- **Performance gate and sanitizer run (2026-09-23)** — measured at merge
  time on the release host with `perf stat -e cycles,instructions -r 3`,
  baseline `e43fa2e4` versus this release, with the SAME benchmark sources on
  both sides (the branch's rewritten, self-verifying `sparse_list_benchmark`
  was compiled against the baseline library too, so the two columns run the
  same program). The host was loaded throughout (1-minute load average 6.5 to
  7.9 on 12 cores), so the three interleaved rounds below are reported in
  full and the conclusion rests on retired instructions, which are
  load-independent; cycles are given for completeness and their spread across
  rounds is larger than the difference between the two columns.

  | Benchmark | Round | Instructions, baseline | Instructions, 2.0.0 | Delta |
  |---|---|---|---|---|
  | `sparse_list_benchmark` | 1 | 561,955,175 | 558,644,067 | −0.59% |
  | `sparse_list_benchmark` | 2 | 559,208,979 | 559,064,235 | −0.03% |
  | `sparse_list_benchmark` | 3 | 562,104,844 | 558,678,437 | −0.61% |
  | `object_access_benchmark` | 1 | 60,374,194,360 | 60,332,010,971 | −0.07% |
  | `object_access_benchmark` | 2 | 60,370,335,560 | 60,333,693,343 | −0.06% |
  | `object_access_benchmark` | 3 | 60,373,632,245 | 60,322,191,846 | −0.09% |

  Cycles, same runs: `sparse_list_benchmark` 535.8M / 537.2M / 545.1M
  (baseline) against 530.6M / 531.6M / 545.8M; `object_access_benchmark`
  24.99G / 24.78G / 25.25G against 25.97G / 25.10G / 25.09G. The one outlier
  (round 1, +3.9% cycles) carried a ±3.29% run-to-run spread of its own and
  did not reproduce in rounds 2 and 3, whose instruction counts are flat.
  **Conclusion: no measurable regression on either benchmark.** Both
  benchmarks self-verify (`VERIFIED` / `Checksum verified.`), so a crash
  could not be counted as a fast run.

  AddressSanitizer (`-fsanitize=address`, RelWithDebInfo): the full 412-case
  suite ran with **zero AddressSanitizer reports**. Three cases fail under
  ASan and all three fail identically at the `e43fa2e4` baseline, so they are
  pre-existing and not caused by this release:
  `ConcurrentMarkSafety.ThreadCacheSlotFlipsDuringMark` (asserts that at
  least 100 GC cycles run inside a fixed 4-second budget — throughput
  sensitive, it also fails in a plain Debug build at the baseline, 31 cycles
  there and here) and the two `SymbolIntern` cases that cap resident-set
  growth at 4 MiB (ASan's shadow memory and redzones exceed that cap by
  construction).
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
- `test/MapParentChainTests.cpp` (nine cases) — the one place where this
  release's two halves meet. A `ProtoMap` handle is a NON-OBJECT cell
  pointer, so every rule the parent-chain rewrite states for a non-object
  receiver or a non-object chain entry has to hold for it through
  `ProtoSpace::mapPrototype`. The cases pin: `getPrototype` answers
  `mapPrototype` for tag 27; `isInstanceOf` on a map answers through that
  prototype and walks the prototype's own flattened chain;
  `getAttribute`/`hasAttribute`/`getAttributes` on a map receiver reach
  `mapPrototype`'s attributes and a missing key still terminates; `newChild`
  on a map childs its prototype; a map stored as a parent is kept by
  `addParent` and by `setParents`' flattening, contributes no ancestors of
  its own, preserves the listed order around it, and is found by
  `hasParent`/`isInstanceOf`; a mutable receiver sees a map parent added
  after creation; and a map reachable ONLY through a parent-link chain (the
  child pinned in a root set, single-root pinning) survives forced collection
  cycles with its 64 entries intact. Neither half needed a code change to
  satisfy them — the file exists so a later change to either half cannot
  quietly drop the map case.
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
