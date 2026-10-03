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
