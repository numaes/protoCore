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
