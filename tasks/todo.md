# Global mutable table (feature/global-mutable-table)

Spec: docs/GLOBAL_MUTABLE_TABLE.md. Decision by the author 2026-09-29: global
table, every space marks it, serialized cycles, implement completely now.

- [x] 1. A thread shared by several spaces answers every member space's
      stop-the-world and leaves every quorum when it blocks
      (core/MultiSpace.cpp; MultiSpaceThreadTests RED->GREEN; ctest 501/501).
- [ ] 2. Serialized collection cycles (process-wide cycle token).
- [ ] 3. Global table + refs carrying the space id; every space marks it.
- [ ] 4. Epoch-based deferred reclamation when several spaces are live.
- [ ] 5. Process-wide cache epoch, checked at lookup.
- [ ] 6. Purge of a destroyed space's entries.
- [ ] 7. Stress under ASan and TSan; benchmarks single-space before/after.
- [ ] 8. Docs (GarbageCollector, MemoryModel, MODULE_DISCOVERY, DESIGN), CHANGELOG.
- [ ] 9. Rebuild and test protoST and protoScala against it.

## Rulings
- The cross-space cooperative stop-the-world of the first draft is replaced by
  epoch-based reclamation: each space adopts the constructing thread as its
  main thread, so a joint stop-the-world cannot complete while that thread is
  parked in another space. Cost if wrong: longer grace waits, no correctness loss.
