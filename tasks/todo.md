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
- [ ] 7. Stress under ASan and TSan; benchmarks single-space before/after.
- [x] 8. Docs (GLOBAL_MUTABLE_TABLE, GarbageCollector, MemoryModel, MODULE_DISCOVERY, DESIGN), CHANGELOG.
- [ ] 9. Rebuild and test protoST and protoScala against it.

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
