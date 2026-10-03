# GCRootScope.AllocationDuringConcurrentMarkIsSafe on macOS arm64 (2026-10-03)

## Symptom

Cross-platform run 37102703169, first attempt, job 111145228053 (macOS,
Apple clang, arm64, Release, ctest `-j2`), branch
`fix/adaptive-heap-calibration`:

```
test/GCRootScopeTests.cpp:295: Failure
Expected equality of these values:
  gThreadErrors.load()
    Which is: 1
  0u
[  FAILED  ] GCRootScope.AllocationDuringConcurrentMarkIsSafe (188 ms)
```

One of 6,000 checks read a wrong value.  The re-run passed, and so did 40
local Linux runs.  The test uses a fixed hard heap limit, not the adaptive
controller.

## Cause

Each iteration of the test's worker thread did this:

```cpp
const ProtoList* old = nullptr;
{
    ProtoContext sub(ctx->space, ctx, ...);
    old = sub.newList()->appendLast(&sub, sub.fromInteger(iteration));
}   // ~sub submits its young chain: `old` is now a candidate
const ProtoList* young = ctx->newList()->appendLast(ctx, old->asObject(ctx));
```

Between the end of `sub` and the moment `young` is linked into `ctx`'s young
chain, `old` is reachable only from a C++ local, and the construction of
`young` allocates.  An allocation can park the thread at a stop-the-world
poll or wait in `waitForHeapHeadroom`, and under the test's hard limit
cycles start often.  A cycle whose stop-the-world falls in that window finds
`old` in its segments, unmarked, and sweeps it; garbage allocated next
recycles the cell, and the check reads something else.

That is a breach of EMBEDDER-CONFORMANCE rule 3 ("No `ProtoObject*` may be
held across an allocation only in a C++ local") by the test itself.  The
collector behaved as documented.  The window is a few allocations wide,
which is why it shows up about once in a hundred runs and only where
thread scheduling makes it likely (macOS arm64 under `ctest -j2`).

## Evidence

Temporary branch `tmp/gcrootscope-flake` (deleted afterwards), workflow
running only these tests:

| Run | What | macOS arm64 | Linux arm64 | Linux x64 | Linux x64 TSan |
|---|---|---|---|---|---|
| 37104482744 | original test, `--gtest_repeat=500`, two concurrent processes | **9 failures / 1,000** | runner killed (see below) | runner killed | 0 / 100 |
| 37104482744 | probe: a whole cycle forced in the window, 2 threads x 200 iterations, 5 repeats | 9-21 errors every repeat | 1-8 every repeat | 2-16 every repeat | 22-49 every repeat |
| 37104482744 | probe: the same forced cycle, `young` built before `sub` dies | 0 errors | 0 | 0 | 0 |
| 37104862859 | fixed test, chunks of 50 repeats, two lanes | **0 / 1,500** | 0 / 1,000 | 0 / 1,000 | 0 / 100 |
| 37104862859 | probe, as above (3 repeats each) | unanchored 42-59 errors, anchored 0 | 2-4, 0 | 25-44, 0 | 28-65, 0 |

The forced-cycle probe settles the question both ways: with the old list
unanchored in the window the collector frees it on every platform, and with
the same forced cycle after the young list exists nothing is ever lost, on
arm64 as on x64 and under ThreadSanitizer.  So no missed root, missing
barrier or weak-ordering defect on arm64 is involved.

## Fix

The young list is built while `sub` is still alive, so `old` passes from
`sub`'s young chain to `young`'s reference without a gap.  The same shape
was in `ObjectReachableOnlyThroughAYoungCellSurvives` (single-threaded, so
a cycle in its window was much less likely) and was fixed too.
`GCRootScope.CandidateReachableOnlyFromAYoungCellSurvivesACycleForcedAtOnce`
is the anchored probe kept as a test: it is deterministic, and it is the
property the original test meant to check.

## Not related

- The earlier one-off macOS hang in
  `AttributeEnumerationTest.ConcurrentMutationDuringWalkIsSafe` was a
  stop-the-world quorum problem (a writer thread that reached no safepoint),
  fixed separately.  This flake is a premature free, not a hang, and has a
  different cause.

## Side observation

The Linux runners in run 37104482744 were shut down while running the
original test 500 times in one process.  The peak resident set grows by
about 33 MB per repetition (41 MB, 337 MB and 1.32 GB at 1, 10 and 40
repetitions on Linux x64; the same on macOS): `~ProtoSpace` does not return
its cell blocks to the operating system.  That does not affect a program
with one space, but a process that creates and destroys many spaces keeps
every heap it ever had.  Reported separately as a GitHub issue.
