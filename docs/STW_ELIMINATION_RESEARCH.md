# Research Note — Eliminating the Stop-The-World GC Pause

> **Status: RESEARCH ONLY — not approved for implementation.**
>
> This document captures an architectural exploration. It is a path to
> *investigate*, not a plan to *build*. The current STW design with a
> cooperative quorum + `UnmanagedScope` is correct, well-understood, and
> already achieves sub-millisecond pauses on realistic workloads. Replacing
> it is a high-risk change that should only be undertaken when a concrete
> user workload demonstrates that the current design is the bottleneck.
>
> Read this before considering any concurrent-mark work. The traps
> described here are subtle and the consequences of getting them wrong
> are silent use-after-free in production.

---

## 1. Why This Note Exists

The current GC uses concurrent marking followed by a short cooperative
Stop-The-World (STW) phase to (a) snapshot roots consistently and
(b) finalize the mark before sweep. With the `UnmanagedScope` API now in
place across protoCore, protoST, protoPython, and protoJS, threads blocked
in syscalls no longer pin the STW quorum, so pauses are bounded by the
slowest *running* thread reaching its next allocation safepoint.

In practice this gives < 1 ms pauses on typical workloads — a very good
result for a general-purpose runtime. The question this note explores:
**can we eliminate even that short STW phase and run the GC fully
concurrently with all mutators?**

The attraction is obvious. Fully concurrent GC means:
- True parallel mark-sweep with zero coordination cost on mutators
- p99 latency independent of heap size or root set size
- A genuine differentiator vs. CLR/JVM/V8 for hard-realtime workloads

The cost, as this note documents, is non-trivial and the failure mode is
catastrophic. Hence: research, not implementation.

---

## 2. Why Immutability Seems to Eliminate the Problem

protoCore's central design property is **structural immutability**: every
`ProtoList`, `ProtoTuple`, `ProtoString`, `ProtoSparseList`, etc. is
immutable. Any "modification" returns a new structurally-shared version.
Combined with per-thread allocation arenas, this creates the appearance
that concurrent GC should be cheap:

- No write barriers in the classical sense — fields of immutable Cells
  never change after construction.
- The snapshot-at-the-beginning is gratis: old Cells *are* the snapshot.
- Per-thread arenas localize 90-95% of all allocations away from any
  shared region that would need coordinated collection.

This intuition is partially correct but **incomplete**.

---

## 3. The One Mutable Slot

protoCore is not pure-immutable. Every mutable Object instance carries
**exactly one mutable field**: a pointer to its current internal
`SparseList` of attributes. When client code calls
`obj->setAttribute(ctx, k, v)`, the runtime:

1. Allocates a new `SparseList` `S'` that contains `k → v` plus all
   previous bindings (structurally shared with the old one).
2. Atomically swaps `obj->internal_ptr` from `S_old` to `S'`.

That single atomic swap is the *only* point in the system where a
reachable graph mutates. Everything else allocates and discards.

That single point is sufficient to break concurrent mark.

---

## 4. The Race That Breaks Naïve Concurrent Mark

Setup (no thread creation, no cross-thread send required):

- GC initiates concurrent mark. Thread set = {T1, T2}.
- A shared mutable Object `S` is reachable from a global root.
  `S.internal = S_old`. `S_old` contains no reference to a fresh Cell `X`.
- T1 has, inside its current `ProtoContext` `C1`, a local `L1 = X`.
  No one else holds `X`.

The catastrophic interleaving:

```
1. GC visits the global root → reaches S → marks S black.
   Reads S.internal = S_old → marks S_old (which has no reference to X).
2. GC has not yet reached T1's roots.
3. T1 executes:   S.setAttribute("f", X)
                    → allocates SparseList S' (contains f→X)
                    → atomic swap:  S.internal_ptr : S_old → S'
4. T1 returns from the current method.  C1 is closed/popped.
   The local L1 is gone.
5. GC visits T1's roots.  C1 no longer exists.  X is not anywhere.
6. GC completes mark.  X is white.
7. GC frees X.
8. Future access to S.f → use-after-free.
```

X is reachable in the final graph (`root → S → S' → X`) but was never
visited by the GC because:

- `S` was scanned *before* the swap (saw `S_old`, not `S'`).
- `S'` was allocated *after* `S` was scanned and was never visited.
- `T1` no longer held `X` in `C1` when the GC reached it.

This is a textbook **incremental update violation** of the tricolor
invariant: a black object (`S`) was allowed to acquire a pointer to a
white object (`X`, via the new `S'`) without the GC being informed.

It requires no thread creation, no cross-thread message, no exotic
condition. It requires only that the graph mutate during mark. Which,
on any nontrivial workload, happens constantly.

**This is why handshake-on-thread-lifecycle alone is insufficient.**

---

## 5. The Minimum Viable Recipe for STW-Free Mark

To preserve the tricolor invariant without a global STW, three
mechanisms must work together. Any one of them missing reintroduces the
race above (or a variant).

### 5.1 Thread Lifecycle Handshake

Threads created during an active mark must register themselves with the
GC before allocating, so their roots are scanned before mark closes.
The thread list itself must be a versioned immutable structure with
CAS-append.

- Fast path (no GC active): plain CAS-append, no cost.
- Slow path (GC active): thread enters `pending_scan@epoch_N` state and
  blocks briefly while the GC drains its initial root set.

This is what HotSpot does with `Threads_lock` during VM operations and
what ZGC does with its thread handshake protocol — implemented
lock-free here using a CAS-published thread list and an epoch counter.

### 5.2 Write Barrier on the One Mutable Slot

The atomic swap inside `Object::setAttribute` (the only graph-mutating
operation in the entire runtime) must inform the GC when it occurs
during mark.

```cpp
void Object::swapInternal(SparseList* newSL) {
    SparseList* old = internal_.exchange(newSL,
                                          std::memory_order_acq_rel);
    // Slow path: only during concurrent mark
    if (__builtin_expect(gc->markActive(), 0)) {
        gc->markGray(newSL);   // ensure newSL gets traversed
    }
}
```

This is a Dijkstra-style incremental-update barrier. Cost:

- GC inactive (~95% of wall time): one relaxed load, well-predicted
  branch. ~1-2 ns. Negligible.
- GC active: one atomic store to a thread-local mark queue. ~10-20 ns.
  Only during the mark window, only on real `setAttribute` calls.

**The architectural payoff of immutability is not the elimination of
write barriers — it is their compression to a single audit-able choke
point.** CLR and JVM must instrument every `obj.field = x` site in the
generated code. protoCore needs to instrument exactly one C++ function.
This is ~100× less instrumented surface, and the entire correctness
argument fits on one page.

### 5.3 Allocation Barrier

Cells allocated during mark are marked black at construction time. They
cannot be reachable from anything outside the allocating thread until
they are explicitly published (via field installation, message send,
return value, etc.), so it is safe to assume them live for the duration
of the current GC cycle.

This is a trivial change to `new(ctx) Cell` — set the mark bit in the
header inline. Cost: one store. Already on the allocation hot path,
already touching the header.

### Combined Invariant

With (5.1) + (5.2) + (5.3) all in place, the tricolor invariant holds:

- No black-to-white edge can exist: either the write barrier promotes
  the target to gray (5.2), or the target was born black (5.3), or the
  target's owning thread will be scanned (5.1).
- The mark queue is non-empty as long as graph mutation introduces new
  references, and empty when the graph reaches a fixed point — the
  natural termination condition.

Sweep can then run concurrently with no further coordination.

---

## 6. What This Buys You

If implemented correctly:

- **Zero global STW.** Pause time becomes O(1) regardless of heap size,
  root set size, or thread count.
- **No worst-case scaling cliff** as `MAX_THREADS` grows. Currently a
  64-thread system has 64 cooperative safepoints to drain on every
  major collection; with this design that drains in parallel with no
  coordination point.
- **Realtime-friendly.** A hard-realtime audio thread, game frame
  thread, or financial tick processor can run with bounded latency
  inside the same protoCore process as a heavy batch worker.

This is a *genuine* differentiator vs. every mainstream runtime
except ZGC/Shenandoah/Go — and unlike those, it costs us *one*
instrumented function instead of write-barrier injection across the
whole compiler.

---

## 7. What This Costs You

Six categories of cost, in roughly increasing severity:

1. **Per-Cell header**: one bit for mark color (already present),
   possibly one extra for "born during mark epoch N".

2. **One atomic operation per `setAttribute`** when GC is inactive
   (a relaxed load + predicted branch). When GC is active, one atomic
   push to a thread-local mark queue per `setAttribute`. Mark windows
   are typically a small fraction of wall time.

3. **Thread spawn slow path** during active GC: low-microsecond
   coordination cost on a rare event.

4. **GC implementation complexity**: roughly 2-3× the code size of the
   current STW GC. The correctness argument involves the tricolor
   invariant across mutator/collector interleaving — every subtle bug
   here surfaces as silent use-after-free in production, typically
   minutes-to-hours after the actual race.

5. **Debugging**: concurrent GC bugs are among the hardest in systems
   programming. They are non-reproducible, timing-dependent, and
   typically manifest as corruption far from the cause.

6. **Backpressure on the mark queue**: if mutators outpace the GC's
   ability to drain the queue, memory grows unboundedly. The mitigation
   ("slow mutators when queue is full") reintroduces a form of pausing
   that is harder to bound than the STW we eliminated.

---

## 8. Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| Tricolor invariant violation → UAF | **Catastrophic, silent** | Exhaustive concurrent-mark test harness with deterministic interleaving (e.g., Lincheck-style or model-checked). Not optional. |
| Mark queue starvation under mutator pressure | High | Adaptive mutator throttling. Reintroduces partial pause. |
| Bugs surfacing only at high core counts | High | Test on 64+ core hardware regularly. CI on AWS metal instances. |
| Implementation complexity overwhelms maintenance | Medium | Single owner, single-purpose module, exhaustive comments. |
| Performance regression on small workloads | Low | Conditional via `ProtoSpace::setConcurrentMark(true)`; default off until proven. |

The **catastrophic-silent** failure mode is the dominant concern. STW
GC is wrong-or-right — a bug in STW marking produces immediate, loud,
reproducible crashes. Concurrent GC can be subtly wrong for months
before a customer's production traffic finally hits the racy
interleaving.

---

## 9. Open Questions for Future Investigation

1. **Mark queue design**: per-thread MPSC queues drained by the GC
   thread? Per-thread SPSC into a single MPMC? Lock-free Treiber stack?
   The choice has measurable impact on the slow-path cost.

2. **Termination detection**: distributed termination of mark across N
   thread queues is its own subfield. Dijkstra-Scholten? Mattern's
   four-counter? Or simpler: quiesce all threads briefly at the end
   (admitting a *tiny* STW to confirm mark fixpoint)?

3. **Allocation pressure during mark**: cells born black during mark
   stay black until the next cycle, even if they become garbage
   quickly. Wastes some memory. Quantify on realistic workloads.

4. **Interaction with `UnmanagedScope`**: a thread inside `UnmanagedScope`
   is invisible to the STW quorum *and* to the concurrent mark. When
   it returns, its newly-mutated state (if any) needs to be reconciled
   with the in-progress mark. Probably fine — `UnmanagedScope` forbids
   ProtoObject access — but needs explicit specification.

5. **Compaction**: this entire design assumes non-moving GC. If we
   ever want compaction, add load barriers on every read (ZGC-style),
   which is a 5-10% read-path tax. Probably not worth it for protoCore.

6. **Generational extension**: cells born black during mark are a
   degenerate form of a young generation. A real generational design
   could amortize mark cost further. Worth quantifying.

---

## 10. Decision Framework

Before anyone starts implementing this:

1. **Identify a real user workload** where the current sub-millisecond
   STW is the bottleneck. Hard-realtime audio? HFT? A game engine with
   strict frame budgets? If no such workload exists, stop here.

2. **Quantify the current STW**: measure p50/p99/p999 pause times on
   that workload. If p999 < frame budget, stop here.

3. **Prototype 5.2 only** — write barrier on the single mutable slot.
   Measure the inactive-path cost. If it exceeds ~2%, the entire plan
   is too expensive and the priorities are wrong elsewhere.

4. **Build the deterministic concurrent-mark test harness first.**
   Not the implementation. The harness. If you cannot demonstrate that
   you can reproducibly trigger the race in section 4 with the *current*
   GC under instrumentation, you are not yet ready to write the fix.

5. Only then proceed with the full design.

---

## 11. Conclusion

Concurrent GC without STW is achievable in protoCore, and the
single-mutable-slot architecture gives it a genuinely simpler
correctness story than any mainstream runtime. The recipe is known
(handshake + write barrier + allocation barrier). The cost is real but
bounded.

The reason this is research and not a plan is the **silent-failure
mode**. The current STW design is correct by construction. Replacing it
with a concurrent design that is "almost correct" is strictly worse
than the current state. Until there is both (a) a demonstrated workload
need and (b) a verifiable correctness story with deterministic tests,
the right answer is to keep the current GC.

Save this analysis. Revisit when a workload demands it.

---

*Document originated from design discussion 2026-05-25.*
*Author of the race construction: project lead.*
*Author of the document: collaborator notes.*
*Status when written: STW + cooperative quorum + UnmanagedScope across
all three runtimes (protoST, protoPython, protoJS) is in place and
producing sub-millisecond pauses.*
