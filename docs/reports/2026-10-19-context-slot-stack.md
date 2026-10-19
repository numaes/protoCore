# Context slot arrays from a per-thread stack (branch perf/context-slot-stack)

Date: 2026-10-19.  Author: Gustavo Marino, with Claude.  Status: on a
branch, not merged.

Question: does a `ProtoContext` per call cost the runtimes a heap
allocation for its slot array, how large is that array, and what does a
per-thread slot stack (no `malloc` per call) save?

Machine, power profile and toolchain as in
[2026-10-18-write-path.md](2026-10-18-write-path.md).

## Verdict

- **Only protoClojure and protoScala allocate slots per call.**  protoPython
  uses 128-255 slots per call and protoJS 16-31, but both pass their own
  frame buffers (`externalSlots`); protoST creates no context per call.
  protoClojure (64-127 slots) and protoScala (4-7) construct the context
  empty and call `resizeAutomaticLocals`, a `new[]` and a `delete[]` per
  call.  In `fib` the context's constructor, resize, destructor, `malloc`
  and `free` are about 21 % (protoClojure) and 28 % (protoScala) of the
  cycles.
- **With the slot stack: protoScala `fib30` -24 %, protoClojure `fib`
  -9 %, `assoc` -9 %, `tak` -6 %** (cycles, A/A within 1 %); protoPython
  and protoJS unchanged.
- **The runtimes' write cost is not the context's.**  In the write probes
  of the 2026-10-18 report, protoPython, protoJS and protoST create no
  context per write; their 2,500-2,900 cycles per write are elsewhere
  (not yet profiled).  protoScala's case-class `copy` (four contexts with
  0-7 slots each) gains only 2 %.

## 1. Slots per context

An isolated copy of 2.14.7 counted, per destroyed context, its slot count
and whether it owned a heap array (`PROTOCORE_CTX_STATS`,
[ctx_stats_instrumentation.diff](data/2026-10-19-context-slot-stack/ctx_stats_instrumentation.diff));
each runtime's binary ran against it (`LD_LIBRARY_PATH`).

| Workload | Contexts | Slots per context (bulk) | Heap slot arrays |
|---|---:|---|---:|
| protoPython `bench_fib` | 1,456,715 | 128-255 | 1 |
| protoPython `binary_trees` | 158,376 | 128-255 | 2 |
| protoJS `function_calls` | 1,000,016 | 16-31 | 1 |
| protoScala `fib30` | 2,692,650 | 4-7 | 2,692,544 |
| protoScala `object_tree` | 852,114 | 0-15 | 655,396 |
| protoClojure `fib` | 2,692,569 | 64-127 | 2,692,553 |
| protoST `fib` | 18 | - | 1 |

Full counts, with `ReturnReference` cells and young-chain submissions per
context: [results/ctx_workloads.txt](data/2026-10-19-context-slot-stack/results/ctx_workloads.txt),
[results/ctx_probes.txt](data/2026-10-19-context-slot-stack/results/ctx_probes.txt).
Contexts that allocate nothing submit nothing (protoPython `fib`: 4
submissions for 1.46 M contexts).

## 2. The change

- `ContextSlotStack` (`proto_internal.h`): a per-thread region of 2^18
  slots (2 MiB), reserved on first use, carved by moving `top`.  Contexts
  of a thread nest LIFO, so a context's destructor moves `top` back.
- A context destroyed out of order (one created with `new` and deleted
  later) leaves a hole; `top` absorbs holes once everything above is gone.
  Up to 64 holes are tracked; beyond that the region below the untracked
  one is lost for the thread's lifetime and contexts fall back to `new[]`
  once the rest fills.
- `resizeAutomaticLocals` grows in place when the context is the top (the
  protoClojure and protoScala pattern: construct empty, then resize).
- No thread, external slots, or no room: `new[]`, as before.
- The stack lives in the same allocation as the thread's mutable-value
  cache (`ThreadBuffers`), so `ProtoThreadExtension` stays one cell.
  `ProtoContext::ownsSlots_` (a private `bool`) became `slotsSource_` (a
  private `uint8_t`: external, heap, stack); size and layout unchanged.
- A first version decided "stack or external" in the destructor by
  comparing the pointer with the stack's bounds; that read a cache line of
  the thread buffers on every context destruction and cost protoPython
  `fib` 1.7 %.  The three-state field removed it.

`ContextSlotStackTests` (7): adjacent slots and reuse, `PROTO_NONE` start,
in-place and moving resize, out-of-order holes absorbed, a full stack
falling back to the heap, slots kept as GC roots.  680/680 under `ctest`
(Release); AddressSanitizer 677 passed, 3 skipped (their own skips);
ThreadSanitizer 680 with no report.

## 3. A/B

[ab.py](data/2026-10-19-context-slot-stack/ab.py): the same runtime
binaries (built against 2.14.6) load either a build of clean master
`f2d8eb5c` (A) or of the branch (B), each from a fresh copy per run; arms
A, B, A again; `perf stat -r 5`; three rounds, medians; every run's output
checked.

| Case | A Gcycles | B Gcycles | B/A cycles | A/A | B/A instructions |
|---|---:|---:|---:|---:|---:|
| protoClojure `fib` | 1.791 | 1.626 | -9.2 % | -0.1 % | -2.9 % |
| protoClojure `tak` | 0.143 | 0.135 | -5.8 % | -0.8 % | -2.5 % |
| protoClojure `assoc` probe | 5.010 | 4.574 | -8.7 % | -0.2 % | -2.4 % |
| protoScala `fib30` | 1.690 | 1.290 | -23.7 % | -0.4 % | -21.2 % |
| protoScala `object_tree` | 1.208 | 1.183 | -2.1 % | -0.1 % | -1.5 % |
| protoScala `copy` probe | 20.030 | 19.626 | -2.0 % | -1.0 % | -1.6 % |
| protoPython `bench_fib` | 1.366 | 1.366 | +0.1 % | -0.5 % | +0.1 % |
| protoJS `function_calls` | 3.354 | 3.323 | -0.9 % | -0.2 % | -0.9 % |

The first version's table: [results/ab_v1.md](data/2026-10-19-context-slot-stack/results/ab_v1.md).

## 4. What remains per context

From the counts and the profiles, in the order they would pay:

- protoScala `copy` costs about 9,600 cycles for four contexts; the slot
  stack took 2 % of it, so the cost is in protoScala's call path
  (`execute`, `invoke`, argument handling), not in protoCore's context.
- A `ReturnReference` cell per returned cell (protoScala: one per
  context in `copy`; protoPython `binary_trees`: 52,789).
- protoClojure still interns the global's name on every `PUSH_VAR`
  (`createSymbol` + `strlen` + `materialise`, about 6 % of `fib`); fixed on
  the protoClojure branch `perf/intern-global-symbols-at-compile-time`.

## What this does not prove

- One machine; three rounds.
- Embedders that destroy many contexts out of order on one thread (more
  than 64 live holes) degrade to the old `new[]` path; none of the five
  runtimes does, per the counts above, but it is not enforced.
- A context must be destroyed on the thread whose stack holds its slots,
  the invariant the mutable-value cache already relies on; it is not
  checked.
