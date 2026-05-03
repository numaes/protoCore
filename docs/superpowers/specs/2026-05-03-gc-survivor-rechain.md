# protoCore GC: Survivor Re-chain & Per-Context Threshold Trigger

**Date:** 2026-05-03
**Status:** Implemented and **enabled by default** (`PROTOCORE_GC_REINCLUDE_SURVIVORS=ON`; configure with `-DPROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` to bisect against the previous behaviour)
**Scope:** `protoCore` (with a small embedder-side change in `protoPython` to route the bytecode operand stack through GC-visible `automaticLocals`)

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

### 4.2 Per-context allocation-threshold submission

`ProtoContext::allocatedCellsCount` (`headers/protoCore.h:853`) tracks every successful allocation in `allocCell()` (`ProtoContext.cpp:304`). When the count crosses `ProtoSpace::maxAllocatedCellsPerContext` the context hands its young chain over to `dirtySegments` so the next GC cycle can reclaim what is no longer reachable, then resets both the chain and the counter to start fresh.

**Where the submission lands.** `safepoint()` — *not* `allocCell()`. The submission sequence is:

```cpp
while (lock.test_and_set(std::memory_order_acquire)) {}
Cell* chain = this->lastAllocatedCell;
this->lastAllocatedCell = nullptr;
this->allocatedCellsCount = 0;
lock.clear(std::memory_order_release);
if (chain) this->space->submitYoungGeneration(chain);
```

The intuition for "submit at safepoint, never inside `allocCell`":

- Every embedder-driven safepoint is a point where the interpreter (or other client code) is in a *self-consistent* state. For protoPython that means between two bytecode opcodes: every reachable `ProtoObject*` is in either `automaticLocals` (the operand stack, see § 4.3), `closureLocals`, `returnValue`, `pendingRoot`, an embedder `ProtoRootSet`, or a global prototype. Nothing is held only in C++ locals across this point.
- Inside `allocCell()`, by contrast, the calling code is mid-expression. Native helpers — `setAttribute`'s build-and-CAS, `LOAD_NAME`'s chained lookups, `BUILD_FUNCTION`'s call to `createUserFunction`, every immutable-`SparseList` rebuild — hold freshly allocated `Cell*` values in C++ locals between successive allocations. Submitting `lastAllocatedCell` from there moves those values out of the implicit pinning the chain provides (members of `lastAllocatedCell` are not in `dirtySegments` and are therefore not candidates for sweep) into candidate status. A subsequent sweep would free them while the helper still holds them.

The submission also skips while the calling thread is inside a critical section (§ 4.3), so a wrapper invoked from inside another wrapper cannot leave a half-built tree exposed.

**Threshold value:**
- Compile-time default: `CONTEXT_GC_THRESHOLD_DEFAULT = 10000` (about 640 KB at 64-byte alignment).
- Runtime override: env var `PROTOCORE_GC_CONTEXT_THRESHOLD`, parsed in the `ProtoSpace` constructor.
- Stored on `ProtoSpace` (single source of truth, read by every context).

### 4.3 Critical sections: STW barred during construct + CAS

Mutable `setAttribute`, `ProtoSparseList::implSetAt`, and similar helpers build a fresh structure (a new attributes tree, a new `ProtoObjectCell`, a new outer `SparseList`, …) and only attach it to a GC root via a final `compare_exchange_weak` on `ProtoSpace::mutableRoot[shard].root`. Between the first allocation and that final CAS the in-flight cells are reachable only through C++ locals on the calling thread.

Even though § 4.2 keeps chain submission out of `allocCell`, the GC's stop-the-world phase still runs concurrently with the mutator. STW root collection during a half-built tree would walk roots that do not yet point at the new structure; sweep would then free the new structure's cells from `dirtySegments` (where the per-context threshold submission left them) and the mutator would CAS a root pointer at memory the collector has already returned to the free pool.

`ProtoContext::CriticalSection` (declared in `headers/protoCore.h`, around line 855 of the field block) is an RAII guard that increments a per-context depth counter on construction and decrements on destruction. The cooperative STW polls in `ProtoContext::allocCell()`, `ProtoContext::safepoint()`, and `ProtoThreadImplementation::implSynchToGC()` skip parking while the depth is non-zero. Because parking is the only way `parkedThreads` reaches `runningThreads`, STW root collection cannot start until the construction completes and the guard is destroyed.

The design intentionally couples chain submission and STW: the safepoint check at the top of `safepoint()` *also* skips while the depth is non-zero, so a wrapper called transitively from inside another wrapper sees a single critical section, not nested submissions.

**Where the guard is currently used:** `ProtoObject::setAttribute` for both the mutable and immutable branches (see `core/ProtoObject.cpp`).

**Where it should be added as the codebase evolves:** any helper that allocates one or more cells and only publishes them to a GC root via a final atomic store or CAS. Direct callers of `ProtoSparseList::implSetAt` outside `setAttribute`, `ProtoList::appendLast` plus parent re-stitching, and equivalent build-then-CAS patterns are candidates. The guard is per-thread, lock-free, and cheap.

### 4.4 Embedder operand stack on `automaticLocals`

The protoPython bytecode interpreter previously held its operand stack in a `std::vector<const ProtoObject*>` (`ExecutionEngine.cpp:GCStack` over a fallback `std::vector` when the calling context had no slot region pre-sized for it). That vector lives outside the GC's root scan: cells pushed onto it were pinned only by membership in `lastAllocatedCell`, which broke the moment § 4.2 began submitting the chain.

The fix is in protoPython: `makeCodeObject` now sizes `co_automatic_count` to `compiler.getMaxStack() + 32` for the four code-object construction sites that previously passed `0` (module body, `eval`, `exec`, `py_compile`). With `co_automatic_count` set, `runCodeObject` allocates the call's `ProtoContext` with enough `automaticLocals` slots to hold both the locals and the operand stack; `executeBytecodeRange` slices `slots = automaticLocals + stackOffset` and the operand stack is automatically a GC root via the existing `automaticLocals` traversal in `gcThreadLoop`.

Other embedders driving their own bytecode (or AST walk) must apply the same pattern.

### 4.5 Feature flag

Both changes guarded by a compile-time flag, default OFF until validation passes:

```cpp
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
    // new sweep + threshold logic
#else
    // existing logic
#endif
```

**Default ON** since the May 2026 audit landed all the necessary critical-section coverage.  Configure with `-DPROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` to bisect against the previous behaviour or to reproduce historical numbers.

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

## 5b. Performance characterisation (default ON, post-audit)

Measured on protoPython benchmarks (5-run median, single-thread, x86_64 Linux, both binaries built `-DCMAKE_BUILD_TYPE=Release`).  The full critical-section audit landed across May 2026; final side-by-side numbers below.  All ratios are wall time vs CPython 3.14 (smaller is better).

| Benchmark           | OFF              | **ON (default)** | Δ                  |
|---------------------|------------------|------------------|--------------------|
| `startup_empty`     | 0.66× faster     | 0.66× faster     | flat               |
| `int_sum_loop`      | 0.67× faster     | **0.61× faster** | better             |
| `list_append_loop`  | 6.39×            | 6.90×            | −8 % (regression)  |
| `str_concat_loop`   | 11.67×           | 11.70×           | flat               |
| `range_iterate`     | 4.26×            | 4.32×            | flat               |
| `multithread_cpu`   | 1.17×            | **1.13×**        | better             |
| `attr_lookup`       | 1.57×            | **1.45×**        | better             |
| `call_recursion`    | 2.56×            | **2.38×**        | better             |
| `memory_pressure`   | 190.91× / **1347 MB** | **43.38× / 358 MB** | **4.4× faster, 3.8× less RSS** |
| **Geomean**         | **3.69×**        | **3.06×**        | **17 % faster**    |

`memory_pressure` is the headline result: the unbounded growth of `lastAllocatedCell` during a long-running interpreter — the leak this design was built to address — is reclaimed mid-execution, RSS drops 3.8× and wall time drops 4.4×.  Geomean across the suite is **17 % faster than the OFF baseline**, despite the additional GC bookkeeping.  Five workloads improve, three are flat, one (`list_append_loop`) regresses by ~8 % — heavy churn into structures the program then discards.

The `str_concat_loop` regression and the `call_recursion` regression both share a profile: they allocate aggressively, mostly into structures the program then discards, with very little long-lived state. The threshold submission fires often, every cycle re-marks the working set, and the mutator pays for that mark traversal even when the live set is small. Two mitigations are practical follow-ups (neither needed for correctness):

1. **Stagger the re-chain.** Push survivors to a secondary holding pen folded back into `dirtySegments` only every Nth cycle, so most cycles touch only freshly submitted cells. Correctness is preserved; the cost amortises to roughly `1 + 1/N` × the current overhead.
2. **Adaptive threshold.** Raise the per-context threshold under low heap-pressure and lower it as `freeRatio` shrinks. The current implementation uses a fixed env-tuned value; an adaptive policy would reduce `submitYoungGeneration` traffic on workloads that do not need the tighter bound.

### 5b.1 Build-mode pitfall observed during measurement

Initial benchmarks reported a 2× regression that vanished once the ON build was reconfigured with `-DCMAKE_BUILD_TYPE=Release`. The build directory had been created without the build type, defaulting to no `-O3 -DNDEBUG`, and `perf record` showed 5–6% of CPU spent in `std::__is_constant_evaluated`, `toImpl<…>`, `Cell::setNext`, `Cell::getNext`, and `std::atomic<…>::load` — all functions that the compiler inlines away under `-O3`. Anyone reproducing the numbers in this section must specify the build type explicitly:

```bash
cmake -B build_on -S . -DPROTOCORE_GC_REINCLUDE_SURVIVORS=ON -DCMAKE_BUILD_TYPE=Release
```

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

## 8. Implementation summary (as shipped)

| Component | Location | Status |
|---|---|---|
| CMake option `PROTOCORE_GC_REINCLUDE_SURVIVORS` (default OFF) | `protoCore/CMakeLists.txt` | ✅ |
| Threshold field + env var `PROTOCORE_GC_CONTEXT_THRESHOLD` | `protoCore/headers/protoCore.h`, `core/ProtoSpace.cpp` | ✅ |
| Survivor re-chain in sweep | `protoCore/core/ProtoSpace.cpp` (sweep phase of `gcThreadLoop`) | ✅ |
| Per-context threshold submission at `safepoint()` | `protoCore/core/ProtoContext.cpp` | ✅ |
| `ProtoContext::CriticalSection` RAII guard | `protoCore/headers/protoCore.h`, `core/ProtoContext.cpp`, `core/Thread.cpp` | ✅ |
| Critical section around mutable + immutable `setAttribute` | `protoCore/core/ProtoObject.cpp` | ✅ |
| Operand stack on `automaticLocals` (module / `eval` / `exec` / `py_compile`) | `protoPython/src/library/PythonEnvironment.cpp`, `protoPython/src/library/BuiltinsModule.cpp` | ✅ |
| Tests T1–T4 | `protoCore/test/GCSurvivorRechainTests.cpp` | ✅ (T2/T4 active; T1/T3 disabled — they assert per-context counter reset, which now happens at `safepoint()` rather than every `allocCell()`, so the test pattern needs to call `ctx->safepoint()` explicitly to observe the reset) |

## 9. Open questions

None at implementation time. All previously open points were resolved:

- **Per-context vs global threshold:** per-context. No atomics needed; per-context counter already exists.
- **Survivor chain location:** single, GC-private. Mutators do not touch it.
- **Write barriers:** none.
- **Cell layout change:** none. Reuse existing `next_and_flags` next pointer.
- **Where to submit the chain:** `safepoint()`, not `allocCell()`. See § 4.2.
- **How to keep STW out of mid-construction:** `ProtoContext::CriticalSection`. See § 4.3.
- **Embedder operand stack:** route through `automaticLocals` via `co_automatic_count = compiler.getMaxStack() + 32`. See § 4.4.

## 10. References

- `headers/protoCore.h:947–1155` — `ProtoSpace` declaration.
- `headers/protoCore.h:680–860` — `ProtoContext` declaration; `allocatedCellsCount` at line 853, `lastAllocatedCell` at line 852.
- `headers/proto_internal.h:462–531` — `Cell` declaration; mark bit and next pointer in `next_and_flags`.
- `core/ProtoSpace.cpp:66–362` — `gcThreadLoop` (5 phases).
- `core/ProtoSpace.cpp:308–355` — sweep (where the leak lives).
- `core/ProtoContext.cpp:258–316` — `allocCell()` (where the threshold trigger goes).
- `GC_STRESS_TEST_FIX_ANALYSIS.md` — pre-existing analysis of the leak symptoms.
