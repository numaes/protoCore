# protoCore GC: Survivor Re-chain & Per-Context Threshold Trigger

**Date:** 2026-05-03
**Status:** Draft for review
**Scope:** `protoCore` only

---

## 1. Problem

The current concurrent mark-and-sweep GC has a **survivor-tenuring leak**.

In `ProtoSpace.cpp:308–355`, after a sweep cycle:

- Cells **not** marked → returned to free pool. Correct.
- Cells **marked** (survivors) → mark bit cleared, **remain inside the captured `DirtySegment`**, and the entire segment is recycled to `dirtySegmentFreePool`. The cells are dropped from the GC's analysis set.

A surviving cell never re-enters the candidate set. If it later becomes unreachable, it leaks forever. This is the root cause of the behaviour documented in `GC_STRESS_TEST_FIX_ANALYSIS.md` ("Promotion Delay … Conservative Collection") and the reason `memory_pressure`-style benchmarks are excluded from comparisons.

A second, related symptom: a long-running function that allocates many short-lived cells (typical loop) accumulates them on `ProtoContext::lastAllocatedCell` and never releases them until the context is destroyed, even when the working set is small.

## 2. Goals

1. **Correctness when the working set both grows and shrinks** — once a cell is unreachable, it must eventually be reclaimed regardless of how long it survived.
2. **Bound RSS in tight loops** — a function allocating heavily with bounded references must not grow RSS unboundedly while it executes.
3. **No mutator overhead** — no write barriers, no extra checks on stores, no changes to hot allocation path beyond a counter compare.
4. **No layout change to `Cell`** — keep 64-byte alignment, no new fields, no break of cache-line padding.
5. **Reversible behind a flag** — must be possible to bisect against the previous behaviour.

## 3. Non-goals

- Generational GC, write barriers, incremental marking. Explicitly rejected by design discussion.
- Parallel sweep. Single GC thread today; revisit only if measurements demand it.
- Changes to safepoint mechanism, lock-free arena allocation, or `dirtySegments` stack semantics.
- Changes to the public API (`headers/protoCore.h`).

## 4. Design

Two changes, both contained in protoCore internals.

### 4.1 Survivor re-chain in sweep

At the end of sweep, instead of recycling the captured segments with their surviving cells inside, **build a fresh chain of survivors and push it back to `dirtySegments`** for the next cycle.

Modified flow in `gcThreadLoop` sweep phase (`ProtoSpace.cpp:308–355`):

For each captured `DirtySegment`:
1. Walk `cellChain`. For each cell:
   - If unmarked → finalize, return to `freeCells` (current behaviour).
   - If marked → clear the mark bit, **prepend to a thread-local survivor chain** via `setNext(survivorHead); survivorHead = cell;`.
2. Recycle the now-empty original segment to `dirtySegmentFreePool` (current behaviour).

After the loop completes, if `survivorHead != nullptr`:
3. Pop a fresh `DirtySegment` from `dirtySegmentFreePool` (or allocate one if pool empty), set `cellChain = survivorHead`, push to the global `dirtySegments` lock-free stack — same path used by `submitYoungGeneration`.

The next STW cycle's root collection drains `dirtySegments` via `exchange(nullptr)` (`ProtoSpace.cpp:281`) exactly as today. Survivors are now part of the candidate set. Mark from roots reaches them if reachable; sweep frees them otherwise.

**Reuse of `Cell::next_and_flags` bits 6–63:** the survivor chain reuses the same `next` pointer used by the old DirtySegment chain. The old chain is being torn down at this point, so no concurrent reader exists. No new field, no layout change.

**Single global survivor structure:** the survivor chain is built and consumed by the GC thread only. No atomics, no locking. Mutators do not touch it.

### 4.2 Per-context allocation-threshold trigger

`ProtoContext::allocatedCellsCount` already exists (`headers/protoCore.h:853`) and is incremented in `allocCell()` (`ProtoContext.cpp:304`). Add a threshold check immediately after the increment.

When `allocatedCellsCount > threshold`:
1. Call `space_->submitYoungGeneration(lastAllocatedCell)` — wraps the chain in a `DirtySegment` and pushes to `dirtySegments` (lock-free). Existing API.
2. Reset `lastAllocatedCell = nullptr`, `allocatedCellsCount = 0`.
3. Call `space_->triggerGC()` — request a cycle. Existing API.

**Threshold value:**
- Compile-time default: `CONTEXT_GC_THRESHOLD_DEFAULT = 10000` (about 640 KB at 64-byte alignment).
- Runtime override: env var `PROTOCORE_GC_CONTEXT_THRESHOLD`, parsed in `ProtoSpace` constructor.
- Stored as a member of `ProtoSpace` (single source of truth, read by every context).

**Why this is safe:**
- Submission happens inside `allocCell()`, which already polls the STW flag every 64 calls. The thread reaches a safepoint naturally on the next iteration.
- Cells being submitted are still reachable through the active frame stack — the context is a root. The next mark cycle will preserve referenced cells and free unreferenced ones.
- The actual GC cycle does not start until all threads enter safepoints (`ProtoSpace.cpp:77–89`). By then, every thread is parked; no in-flight unrooted cells exist.

### 4.3 Feature flag

Both changes guarded by a compile-time flag, default OFF until validation passes:

```cpp
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
    // new sweep + threshold logic
#else
    // existing logic
#endif
```

Enable via CMake option `-DPROTOCORE_GC_REINCLUDE_SURVIVORS=ON`. CI builds both modes; bisection trivial.

## 5. Why this is correct

**Inductive argument:**

*Base case.* At `ProtoSpace` construction, `dirtySegments` is empty. The first cycle has zero candidates and zero survivors.

*Inductive step.* At the start of cycle N, every live cell in the heap is in `dirtySegments`, because:
- Either it was allocated in some context and submitted via context destruction or threshold trigger.
- Or it survived cycle N-1 and was re-chained by sweep into a new DirtySegment pushed to `dirtySegments`.

Mark from roots reaches every cell reachable from roots. Sweep frees the rest. After cycle N, every live cell is again in `dirtySegments` (re-chained as a survivor).

By induction, no live cell is ever lost to mark, and every cell that becomes unreachable is freed in a finite number of cycles.

**No write barriers needed:** because every live cell is re-marked from roots every cycle, no inter-generational pointer tracking is required. The cost is mark traversal proportional to the live working set — this IS the working set, accepted as the minimum correct work.

**Tight-loop bounding:** a loop allocating M cells per iteration with bounded retention triggers a cycle every `threshold / M` iterations. At each cycle, only currently-referenced cells survive; the rest are freed. RSS stays bounded in the working set, not in the iteration count.

## 6. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Mark cost grows per cycle (now scales with live set, not just new allocs) | Mark is concurrent with mutators; cost is amortized. This is the minimum correct work. Monitor wall-clock and STW pause separately. |
| GC starvation if alloc rate exceeds mark throughput | Existing heap-pressure trigger remains as backstop. Threshold trigger adds a *more frequent* firing condition, never a less frequent one — cannot regress liveness. |
| Threshold too low → too many cycles | Configurable via env var. Start at 10K, tune from benchmarks. |
| Threshold too high → loss of bounding effect | Heap-pressure trigger as backstop. |
| Concurrency between sweep re-chain and mutator allocation | None. Re-chain operates on segments captured at STW; mutators allocate into per-context `lastAllocatedCell`, an independent structure. The push to global `dirtySegments` is already lock-free. |
| Existing tests rely on pinning behaviour described in `GC_STRESS_TEST_FIX_ANALYSIS.md` | Run full suite under both flag states. If a test fails only with the flag ON, audit whether the test encodes a real invariant or just observes the leak. |

## 7. Tests

### 7.1 New tests (must fail today, pass with flag ON)

**T1. RSS bounded in tight loop, no escapes**
Allocate M cells per iteration, K iterations (K ≫ threshold/M), no references retained across iterations. Assert peak RSS ≤ small constant + working set, independent of K.

**T2. Long-lived survivor freed when reference drops**
Allocate a cell, hold a reference for K cycles (K > 1), drop the reference, run one cycle. Assert the cell is freed. Today this never happens after the first cycle.

**T3. Threshold trigger fires**
Single context allocates `2 × threshold` cells in a loop. Assert at least one GC cycle ran without a context destruction (currently zero cycles fire under this load).

### 7.2 New tests (must pass before AND after)

**T4. RSS grows when cells legitimately retained**
Loop appending cells to a `ProtoList`. Assert RSS grows roughly proportional to iterations. Confirms we did not break legitimate retention.

### 7.3 Existing tests must continue to pass

- `test/GCStressTests.cpp::LargeAllocationReclamation` — heap stabilises under concurrent allocation.
- All tests under `test/` and `performance/` — no functional or performance regression.

## 8. Implementation roadmap

(Full file-level breakdown deferred to the implementation plan.)

1. Add `CONTEXT_GC_THRESHOLD_DEFAULT` constant; add `gcContextThreshold_` field to `ProtoSpace`; parse env var in constructor.
2. Add CMake option `PROTOCORE_GC_REINCLUDE_SURVIVORS` and `#define` it through `proto_internal.h`.
3. Modify sweep phase in `gcThreadLoop` (`ProtoSpace.cpp:308–355`) to build survivor chain and push back to `dirtySegments`, guarded by the flag.
4. Modify `ProtoContext::allocCell()` (`ProtoContext.cpp:258–316`) to fire submit + trigger when `allocatedCellsCount > threshold`, guarded by the flag.
5. Add new tests T1–T4 under `test/GCStressTests.cpp` (or a new file).
6. CI: build and test both flag states.

## 9. Open questions

None at design time. All previously open points were resolved:

- **Per-context vs global threshold:** per-context. No atomics needed; per-context counter already exists.
- **Survivor chain location:** single, GC-private. Mutators do not touch it.
- **Write barriers:** none.
- **Cell layout change:** none. Reuse existing `next_and_flags` next pointer.
- **Removal of existing pinning:** unchanged. Pinning is implicit via the context root; this design only fixes the post-sweep drop.

## 10. References

- `headers/protoCore.h:947–1155` — `ProtoSpace` declaration.
- `headers/protoCore.h:680–860` — `ProtoContext` declaration; `allocatedCellsCount` at line 853, `lastAllocatedCell` at line 852.
- `headers/proto_internal.h:462–531` — `Cell` declaration; mark bit and next pointer in `next_and_flags`.
- `core/ProtoSpace.cpp:66–362` — `gcThreadLoop` (5 phases).
- `core/ProtoSpace.cpp:308–355` — sweep (where the leak lives).
- `core/ProtoContext.cpp:258–316` — `allocCell()` (where the threshold trigger goes).
- `GC_STRESS_TEST_FIX_ANALYSIS.md` — pre-existing analysis of the leak symptoms.
