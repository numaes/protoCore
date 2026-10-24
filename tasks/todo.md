# Write path and cross-runtime re-measurement (2.14.7)

- [x] 1. Attribute lookup baseline, per-call cache statistics, real-workload
      hit rates; cache index variants measured (not adopted).
- [x] 2. Value replacement without rebalancing (+ SparseListValueReplace
      tests); young-chain push without a spinlock (TSan clean).
- [x] 3. 2.14.7: version, changelog, README counts; merged to master (local).
- [x] 4. Runtimes built twice at current HEAD: build_cmp2146 / build_pkg_oct18.
- [x] 5. Summary table: protoCore vs C++, JVM mutable, records, Clojure and
      Scala persistent maps (attr-study/, final_table.sh).
- [x] 6. Session (rerun2.sh; rerun.sh superseded: file-instance artefact): write probes, five harnesses x 3 rounds,
      protoJS adaptive-heap evaluation, protoClojure vs JVM Clojure and bb.
- [x] 7. Re-run the summary table (on a clean master build: the first
      re-run measured a library rebuilt from this branch's working copy).
- [x] 8. Report docs/reports/2026-10-18-write-path.md + index (f2d8eb5c,
      7d44da4b).  [x] Runtime READMEs updated (local commits).
- [x] 9. Push protoCore master and tag v2.14.7.
- [ ] 11. Runtime write cost: perf the write probes; histogram of slots per
      ProtoContext (env-gated, isolated copy) per runtime; split the four
      per-context costs (slot memory + PROTO_NONE fill, ReturnReference,
      submitYoungGeneration, thread registration); mutable_population probe.
- [ ] 12. Lightweight context: per-thread slot stack (in ProtoThreadExtension,
      no ABI change; LIFO pop, holes for out-of-order heap contexts, resize in
      place at the top, new[] fallback), then the other per-context costs as
      the measurement ranks them.  Runtime quick fixes in their own repos
      (protoPython dead __mro__ walk, protoJS "length" string compare,
      protoClojure createSymbol per PUSH_VAR).
      Measured (11): slots per context: protoPython 128-255 but external
      (its frames), protoJS 16-31 external, protoST no context per call;
      protoClojure 64-127 and protoScala 4-7 through resizeAutomaticLocals,
      i.e. new[] per call (context lifecycle + malloc ~21 % / ~28 % of fib).
      In the write probes py/js/st create no context per write: their
      2,500-2,900 cycles are not context cost (still to profile).
      [x] Slot stack on perf/context-slot-stack (1441c54d + three-state
      slot source): 680/680 ctest, ASan, TSan.  A/B v1: sc fib30 -24 %,
      clj fib -9.9 %, assoc -6.8 %, tak -4.8 %, py fib +1.7 % (the
      destructor's stack check; v2 removes it).  Not merged.
      [x] v2 (1f50ba00): sc fib30 -23.7 %, clj fib -9.2 %, assoc -8.7 %,
      tak -5.8 %, py/js within 1 %; 680/680 ctest and ASan.
      [x] Runtime fixes on local branches, measured (same 2.14.7 lib):
      protoPython STORE_ATTR write probe -38.9 % (f0162c45), protoJS
      put_field -11.3 % (8b20caf3d), protoClojure PUSH_VAR -4.2 % fib
      (fe46927), protoScala list_ops Long fold (6f3baa5e).  Not pushed.
      [x] Runtime READMEs: protoCore 2.14.7 paragraphs (local commits on
      main/master, not pushed).
      [ ] Decide: merge the slot stack; merge the runtime branches; turn
      on the adaptive heap by default in protoJS.
      [ ] Next per-context costs: protoScala's call path (copy ~9,600
      cycles), ReturnReference per returned cell.
- [x] 10. Design the CAD demonstrator (docs/specs/2026-10-19-cad-versioned-model-demonstrator.md): one editor and several readers
      needing consistent versions; protoCore snapshots against an honest
      Java solution (ReadWriteLock / deep copy per version); measure reader
      latency under writes, memory per version, code needed for correctness.

# Adaptive heap calibration (fix/adaptive-heap-calibration, 2.10.1)

Problem at release (2.10.0): on a fast allocator with a 1 M-cell live set S
reached ~20 x L (1.2 GB against 672 MB under the 640 MB fixed limit).
Acceptance: peak RSS <= today's fixed policy, wall time within +5 %, no OOM
where today succeeds, bounded convergence; otherwise report the frontier.

- [x] 1. PROTOCORE_ADAPTIVE_HEAP=1 diagnostic switch (+ fixed-limit trace
      lines); measured runtime binaries against a dev library via
      LD_LIBRARY_PATH (ldd-checked).
- [x] 2. Workload matrix harness (scratch): protoCore benchmark 0/100k/1M/5M,
      protoST, protoPython, protoClojure, protoScala, protoJS (copy of the
      binary) structures + probe; self-verifying; MemoryMax scopes.
- [x] 3. Candidates measured: rate-aware target (cycle time and mark time),
      cap with a consecutive-pressure override, pure live cap, early wake on
      swept chunks, k_live 2/3/4, S0 32/128/256 MiB, trigger 0.5/0.75/0.9,
      p_high 0.2 with g 1.25.
- [x] 4. Law: k_live 3, k_cap 8 (pure cap), S0 128 MiB, trigger 0.75;
      unit tests rewritten, fast-allocator integration test (RED on 2.10.0:
      S = 30.2 M vs cap 10.4 M), storm test reformulated (settles: calm or
      cap), steady check loosened to half the garbage (measured 13.6 %).
- [x] 5. Final matrix (3 runs), report, spec section 7, docs.
- [x] 6. Local: Release 579/579 (clock-dependent included), ASan 570/570 and
      TSan 570/570 (clock-dependent excluded, as CI; 0 TSan reports).  CI on
      the branch: CI 37102700639 green; Cross-platform 37102703169 green on
      its second attempt (first: see review).
- [ ] 7. Merge to master, tag v2.10.1, GitHub release with the Linux .deb.

## Review
- The 2.10.0 runaway was not an equilibrium: the stall that grew S was the
  collector's sweep throughput (cycle time grows with S), so the trace's
  "settles at L + 2 x rate x Tc" was the program ending.  Lesson: before
  reading a trace's last value as a fixed point, check that the quantity
  driving it (here Tc) does not scale with the controlled variable.
- No setting met both acceptance criteria everywhere; the frontier is in the
  report.  The residual time cost is the single-threaded sweep (protoJS).
- Cross-platform attempt 1 failed on macOS arm64 in
  GCRootScope.AllocationDuringConcurrentMarkIsSafe (gThreadErrors = 1): a
  fixed-limit test whose code path this branch does not change; it passed on
  re-run and in 40 local runs.  A rare GC-correctness failure under
  concurrent mark on arm64, pre-existing as far as can be told: open, to be
  investigated separately.


# Adaptive heap controller (feature/adaptive-heap, 2.10.0)

Spec: docs/specs/2026-10-02-adaptive-heap-controller-design.md (approved
2026-10-02; section 6 = implementation decisions).

- [x] 1. Control law as a pure function + unit tests (AdaptiveHeapLaw.*).
- [x] 2. Limits detection: cgroup v1/v2 from an injectable root, job object,
      sysctl; default H; INT_MAX clamp (AdaptiveHeapLimits.*, AdaptiveHeapCgroup.*).
- [x] 3. Side-structure state, P/T/L measurement, S applied at cycle end,
      process budget across spaces, OOM rule under the controller.
- [x] 4. Cycle at S without a waiter (CycleStartsAtSoftLimit... RED->GREEN).
- [x] 5. Pacing at half the headroom; checkpoint soft wait; completion-based
      waits (found by tracing: S ran away to 20 M cells without them).
- [x] 6. Teardown deadlock with several spaces (MultiSpaceTeardown RED 3/3 -> GREEN).
- [x] 7. Conformance cases restore the controller (self-check RED -> GREEN).
- [x] 8. Local: Release 570/570; ASan and TSan suites; six benchmarks
      (instructions +0.00 %); adaptive_heap_benchmark vs fixed 640 MB.
- [x] 9. Docs, CHANGELOG, version 2.10.0.
- [ ] 10. CI green on the branch, merge, tag, release.

## Review
- The literal trigger of the design (a cycle only when the heap reaches S)
  makes every cycle a full stall and S runs away; pacing fixed it.  Recorded
  in the spec, section 6.
- Fast allocators settle at S = L + 2 x rate x cycle time, not
  k_live x L + S0: the expectation in section 4 was corrected with numbers.

# Global mutable table (feature/global-mutable-table)

Spec: docs/GLOBAL_MUTABLE_TABLE.md. Decision by the author 2026-09-29: global
table, every space marks it, serialized cycles, implement completely now.

- [x] 1. A thread shared by several spaces answers every member space's
      stop-the-world and leaves every quorum when it blocks
      (core/MultiSpace.cpp; MultiSpaceThreadTests RED->GREEN; ctest 501/501).
- [x] 2. Serialized collection cycles (process-wide cycle token; CollectionCyclesOfDifferentSpacesNeverOverlap RED (2 at once) -> GREEN; ctest 502/502).
- [x] 3. Global table + refs carrying the space id; every space marks it (3 GlobalMutableTable tests RED->GREEN; 2 internal tests now read globalMutableShards; ctest 506/506 excluding the step-4/6 tests).
- [x] 4. Deferred reclamation behind a grace period when several spaces are live (AValueHeldByAnotherSpacesThreadSurvivesUntilItsSafepoint RED: 302,398 corrupt reads -> GREEN; ctest 508/508).
- [x] 5. Process-wide cache epoch (multispace::gcEpoch), cleared at quiescent points (folded into 4).
- [x] 6. Purge of a destroyed space's entries (EntriesOfADestroyedSpaceArePurged RED 100 -> GREEN 0; ctest 509/509).
- [x] 7. ASan: multi-space tests clean; ThreadCacheSlotFlipsDuringMark misses its 100-cycles-in-4-s bar under ASan on master too (43-58). TSan: 0 reports in the multi-space tests after fixing 4 pre-existing races. Single-space benchmarks (6, perf stat -r 3, interleaved vs master): no regression.
- [x] 8. Docs (GLOBAL_MUTABLE_TABLE, GarbageCollector, MemoryModel, MODULE_DISCOVERY, DESIGN), CHANGELOG.
- [x] 9. protoST 1041/1041 and protoScala 2313/2313 against it (ldd-verified); protoScala interop 8/8 incl. the K4 read-through-caller test, which fails on installed 2.5.0.

## Rulings
- The cross-space cooperative stop-the-world of the first draft is replaced by
  epoch-based reclamation: each space adopts the constructing thread as its
  main thread, so a joint stop-the-world cannot complete while that thread is
  parked in another space. Cost if wrong: longer grace waits, no correctness loss.
- Grace period is quiescent-state based (safepoints outside critical sections,
  parks, unmanaged regions), not per-operation epochs: a safepoint is where the
  stop-the-world already assumes no cell is held only in C++ locals, so no table
  operation needs an announcement and nothing is added to the lookup paths.
  Cost if wrong: a thread that never reaches a safepoint delays another space's
  reclamation -- the same thread already delays its own space's stop-the-world.
- Caches are cleared at the first quiescent point after a new process epoch,
  not checked at lookup: the grace period guarantees every thread passes one
  before any freed cell is reused. Zero cost on the lookup path.

## String handle points at its rope root (3.2.0, branch feature/string-intern)

Goal: drop the `ProtoStringImplementation` header cell. A heap string handle
(tag 6, symbol tag 22: unchanged, `isStringTagFast` is inlined in embedders)
points directly at its rope root, a `StringLeafNode` or `StringInternalNode`.
A string of one leaf is ONE cell; every heap string saves one cell.

- [x] Both node types carry `node_kind` as their first data member (offset 16,
      right after `Cell`); the handle reader re-tags the root 23/24 from it.
      A test checks the offset on real objects; debug builds assert it
      against `getType()`.
- [x] Leaf layout: node_kind, byte_count, char_count, payload, content_hash.
      Payload array 36 bytes; MAX_PAYLOAD stays 32 in step 1 (no shape change).
- [x] Replace `ProtoStringImplementation` by root functions
      (`heapStringRoot`, `heapStringFromRoot`, `buildRopeFromUTF8`);
      remove dead `internString` / `StringInternSet`.
- [x] SymbolTable, ProtoObject::asList, tests (StringBuild, GlobalInterning,
      test_string) on the root API.
- [x] Cell-count test: 7..32-byte string = 1 cell; 33..64 = 3 cells.
- [x] ctest green; commit.
- [ ] Step 2: MAX_PAYLOAD 32 -> 36, goldens' rope shapes updated; ctest; commit.
- [ ] Docs: DESIGN.md string tiers, CHANGELOG.
