# protoCore GC Survivor Re-chain — Implementation Plan

> Companion to `docs/superpowers/specs/2026-05-03-gc-survivor-rechain.md`.
> Steps use checkbox syntax for tracking. Each task ends in a commit.

**Goal:** ship the survivor re-chain + per-context threshold trigger behind a compile-time flag.

**Tech:** C++20, CMake, ctest (Google Test), single-thread GC.

**Status:** all tasks complete. The shipped implementation differs from the original
plan in two respects, documented in the spec § 4.2 / § 4.3:

1. The threshold submission happens in `ProtoContext::safepoint()`, not in
   `allocCell()`. Submitting from inside `allocCell()` orphans `ProtoObject*`
   values held in C++ locals across allocations by native helpers (e.g.
   `LOAD_NAME` chained lookups, `BUILD_FUNCTION → createUserFunction`). Embedder
   safepoints are the only points where every reachable cell is anchored to a
   real GC root.
2. Mutable `setAttribute` and similar tree-builders enter a per-context
   critical section (`ProtoContext::CriticalSection`) that bars cooperative
   STW polling until the construct + CAS-into-root sequence completes. This
   prevents the GC from observing a half-built tree.

A small embedder-side change in `protoPython` (route the bytecode operand stack
through `automaticLocals` by sizing `co_automatic_count = getMaxStack() + 32`
in module / `eval` / `exec` / `py_compile` code-object construction) is
required for any embedder driving its own bytecode loop on protoCore.

---

## Task 1 — CMake option and macro plumbing

**Files:**
- Modify: `protoCore/CMakeLists.txt`

**Steps:**

- [ ] Add `option(PROTOCORE_GC_REINCLUDE_SURVIVORS "Enable GC survivor re-chain & per-context threshold trigger" OFF)`.
- [ ] When ON, propagate as compile definition: `target_compile_definitions(protoCore PRIVATE PROTOCORE_GC_REINCLUDE_SURVIVORS)`.
- [ ] Build OFF: `cmake -B build -S . && cmake --build build` — verify clean compile.
- [ ] Build ON: `cmake -B build_on -S . -DPROTOCORE_GC_REINCLUDE_SURVIVORS=ON && cmake --build build_on` — verify clean compile (no behaviour change yet).
- [ ] Commit: `gc: cmake option for survivor re-chain (no-op)`.

---

## Task 2 — Threshold infrastructure on ProtoSpace

**Files:**
- Modify: `protoCore/headers/protoCore.h` (ProtoSpace declaration around line 947–1155)
- Modify: `protoCore/core/ProtoSpace.cpp` (constructor)

**Steps:**

- [ ] Add `static constexpr unsigned int CONTEXT_GC_THRESHOLD_DEFAULT = 10000;` near the top of ProtoSpace.
- [ ] Add `unsigned int gcContextThreshold;` field to ProtoSpace, public.
- [ ] In ProtoSpace constructor, set `gcContextThreshold = CONTEXT_GC_THRESHOLD_DEFAULT;` then override from env var `PROTOCORE_GC_CONTEXT_THRESHOLD` if set and parses to a positive integer.
- [ ] Build both flag states. Run existing tests: `ctest --test-dir build -j$(nproc) --output-on-failure`. Should pass identically to before.
- [ ] Commit: `gc: threshold field + env var on ProtoSpace`.

---

## Task 3 — Tests T1, T2, T4 (will fail in OFF mode for T1/T2 due to existing leak)

**Files:**
- Create or modify: `protoCore/test/GCStressTests.cpp` (or new file `test/GCSurvivorRechainTests.cpp`)

**Steps:**

- [ ] Read `test/GCStressTests.cpp` to learn the test framework pattern (Google Test, ctest target name).
- [ ] **T1: RSS bounded in tight loop, no escapes.** Single context, loop K=200 iterations of M=10000 cells alloc'd into a temporary then dropped. Track `space->freeCellsCount` or RSS-equivalent metric; assert it does not grow linearly in K (allow constant slack).
- [ ] **T2: long-lived survivor freed when reference drops.** Allocate one cell, hold via a root-set, force GC twice (cell survives both). Drop the root, force GC once. Assert the cell was returned to `freeCells`.
- [ ] **T4: legitimate retention grows.** Loop appending cells to a `ProtoList`; assert RSS / live-cell-count grows ~ K. Confirms no over-collection.
- [ ] Build OFF, run all three. **T1 and T2 must FAIL today** (the leak); T4 must PASS. If T4 fails, the test is wrong; fix it.
- [ ] Build ON, run all three. They will all still fail for now (we haven't touched sweep) — but they should be clean compilations.
- [ ] Commit: `gc: tests for survivor re-chain (T1, T2, T4)`.

---

## Task 4 — Survivor re-chain in sweep phase

**Files:**
- Modify: `protoCore/core/ProtoSpace.cpp` lines 308–355 (sweep phase of `gcThreadLoop`).

**Steps:**

- [ ] In the inner cell-walk loop, when `cell->isMarked()` (the `else` branch at line 326), after `cell->unmark()` add (under `#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS`):
  ```cpp
  cell->internalSetNextRaw(survHead);
  survHead = cell;
  ```
  with `Cell* survHead = nullptr;` declared before the inner loop.
- [ ] After the inner loop, replace the unconditional segment-recycle (lines 341–353) with a conditional:
  - If `survHead != nullptr` (under flag): set `currentSeg->cellChain = survHead;` and CAS-push `currentSeg` to `space->dirtySegments` (NOT `dirtySegmentFreePool`).
  - Otherwise: existing recycle to `dirtySegmentFreePool`.
- [ ] OFF mode unchanged.
- [ ] Build ON, run T1 + T2 + T4. **T1 and T2 should now PASS** with the flag ON. T4 still PASS.
- [ ] Build OFF, run all tests. **T1 and T2 should still fail** (flag-gated). T4 PASS. Existing GCStressTests pass.
- [ ] Commit: `gc: re-chain survivors after sweep, push back to dirtySegments`.

---

## Task 5 — Test T3 (threshold trigger fires)

**Files:**
- Modify: same test file as Task 3.

**Steps:**

- [ ] **T3: threshold trigger fires.** Set `space->gcContextThreshold = 100` (low). In a single context, alloc 250 cells in a loop (no escapes). Track GC cycle count via a counter. Assert the counter advanced (at least 1, ideally 2) without any context destruction.
- [ ] Build ON, run T3. Should FAIL today (no trigger code yet).
- [ ] Build OFF, run T3. Should be skipped or PASS-by-virtue-of-flag (acceptable both ways; document the choice in the test).
- [ ] Commit: `gc: test T3 for threshold trigger`.

---

## Task 6 — Threshold trigger in allocCell

**Files:**
- Modify: `protoCore/core/ProtoContext.cpp` lines 258–316 (`allocCell`).

**Steps:**

- [ ] After `this->allocatedCellsCount++` (line 304), under `#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS`, add:
  ```cpp
  if (this->space &&
      this->allocatedCellsCount > this->space->gcContextThreshold &&
      this->space->gcThread &&
      std::this_thread::get_id() != this->space->gcThread->get_id()) {
      while (lock.test_and_set(std::memory_order_acquire)) {}
      Cell* chain = this->lastAllocatedCell;
      this->lastAllocatedCell = nullptr;
      this->allocatedCellsCount = 0;
      lock.clear(std::memory_order_release);
      if (chain) {
          this->space->submitYoungGeneration(chain);
      }
      this->space->triggerGC();
  }
  ```
- [ ] Build ON, run T3. **T3 should PASS.**
- [ ] Run T1, T2, T4 again — confirm still PASS in ON.
- [ ] Build OFF, run full suite — confirm no regression.
- [ ] Commit: `gc: per-context allocation threshold trigger`.

---

## Task 7 — Full regression and turn-on decision

**Steps:**

- [ ] Build OFF, run `ctest -j$(nproc) --output-on-failure` from `build/`. All pre-existing tests pass.
- [ ] Build ON, run `ctest -j$(nproc) --output-on-failure` from `build_on/`. All pre-existing tests pass + new T1-T4 pass.
- [ ] If either fails, investigate before proceeding.
- [ ] Decision point: leave default OFF (safer, requires opt-in) or flip default ON (commit if confident from regression). User decides.
- [ ] Commit: `gc: enable survivor re-chain by default` (only if user approves the flip).

---

## Notes

- DirtySegment fields are `cellChain` (Cell*) and `next` (DirtySegment*). Reuse pattern from `submitYoungGeneration` for the CAS push.
- `Cell::internalSetNextRaw` sets the next pointer without altering flags (used by sweep already).
- `space->triggerGC()` exists at `protoCore.h:1043`; non-blocking signal.
- `space->submitYoungGeneration(chain)` exists at `protoCore.h:1029`; lock-free push to `dirtySegments`.
