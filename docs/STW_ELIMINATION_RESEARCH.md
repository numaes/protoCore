# Research Note — Bounding the Stop-The-World GC Pause

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
>
> **Honesty disclaimer.** An earlier draft of this note used phrases
> like "zero STW" and "fully concurrent with zero coordination". That was
> overstated. No production GC achieves true zero pause — ZGC,
> Shenandoah, and Go all retain short residual pauses for root snapshot
> initiation, mark fixpoint detection, and thread handshakes. Their real
> achievement is decoupling pause time from heap size and root-set size,
> not eliminating it. The recipe sketched here would land in the same
> category: shorter, bounded pauses — not zero. Section 12 lists what we
> explicitly **do not know** about the design.

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
**can we restructure the GC so that the residual pause time is bounded
independently of heap size, root-set size, and thread count — landing in
the same category as ZGC / Shenandoah / Go?**

Note the framing. Not "zero STW" — no production GC achieves that. The
realistic target is:

- Pause time **O(1)** in heap size, root-set size, and thread count
  (today our cooperative quorum is O(running threads × time to reach
  next safepoint), which is small in absolute terms but not bounded by
  design).
- Most mutator work runs concurrently with collection, including
  the bulk of marking, sweeping, and freelist refill.
- Residual pauses remain — for initial root scan, mark termination
  detection, thread-list synchronization, and finalization — but each
  one is a fixed-cost operation, not a scan over user data.

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

This is a Dijkstra-style incremental-update barrier. Cost estimates
(**not measured on protoCore — extrapolated from similar systems and
microarchitectural reasoning; could be wrong by a factor of 2-5 either
direction**):

- GC inactive (~95% of wall time): one relaxed load + well-predicted
  branch. Roughly ~1-2 ns on a modern x86. Could be worse under cache
  pressure or branch-prediction failure in real workloads. We have not
  measured this.
- GC active: one atomic push to a thread-local mark queue.
  Roughly ~10-20 ns. Subject to queue contention if mutators outpace
  the collector; the practical upper bound depends on queue design and
  mutator-vs-collector ratio, neither of which we have characterized.

**The architectural payoff of immutability is not the elimination of
write barriers — it is their compression to a single audit-able choke
point.** CLR and JVM must instrument every `obj.field = x` site in the
generated code. protoCore needs to instrument exactly one C++ function.
This is ~100× less instrumented surface, and the entire correctness
argument fits on one page.

### 5.3 Allocation Barrier

Cells allocated during mark are marked black at construction time. The
*assumption* is: a Cell cannot be reachable from anything outside the
allocating thread until it is explicitly published (via field
installation, message send, return value, etc.), so it is safe to
treat it as live for the duration of the current GC cycle.

This assumption needs to be verified against every code path that
constructs a Cell:

- Normal `new(ctx) Cell` from interpreter/native: clearly safe.
- FFI/HPy paths in protoPython: a native extension could in principle
  expose a freshly-allocated Cell via a non-Proto mechanism before
  publication. Needs audit.
- QuickJS bridge in protoJS: GCBridge already mediates this, but the
  exact invariants need to be re-checked under concurrent mark.
- ProtoExternalPointer construction and adoption: needs audit.
- Any path that hands a Cell to a `std::thread` or thread pool worker
  outside the protoCore-managed set: needs explicit handling.

If any of these paths can publish a Cell without going through a
barrier that the GC observes, the allocation barrier is unsound. We
have not done this audit.

Mechanically, this is a one-store change to `new(ctx) Cell` — set the
mark bit in the header inline. Already on the allocation hot path,
already touching the header. The cost is trivial. The correctness
argument is not.

### Combined Invariant — and Its Limits

With (5.1) + (5.2) + (5.3) all in place, the tricolor invariant holds
*for the cells in the heap*:

- No black-to-white edge can exist: either the write barrier promotes
  the target to gray (5.2), or the target was born black (5.3), or the
  target's owning thread will be scanned (5.1).
- The mark queue is non-empty as long as graph mutation introduces new
  references, and empty when the graph reaches a fixed point.

What is **not** eliminated:

- **Initial root snapshot.** Before mark can start, the GC must
  publish "mark active = true, epoch = N" and ensure every thread
  observes that publication before it touches the write barrier path.
  This requires a memory-fence handshake with every active thread —
  a short, fixed-cost synchronization that is not strictly zero. ZGC
  pays this as a sub-microsecond fence; we would pay similar.

- **Termination detection.** Determining that the mark queue is
  globally empty across N thread-local queues is a distributed
  termination problem. The naïve solution (one global atomic counter
  of "queued items") is a contention hotspot. The sophisticated
  solutions (Dijkstra-Scholten, Mattern's four-counter, wave
  propagation) have non-trivial constant factors. The pragmatic
  shortcut is to admit a *short* final-mark STW to confirm fixpoint —
  bounded by the mark queue size, not by heap size. This is what most
  real concurrent collectors actually do, including ZGC. It is not
  zero pause.

- **Sweep coordination.** Sweep is in principle concurrent because
  inmutable Cells can be safely freed when no live mark reaches them.
  But the freelist refill path (per-thread arena getting fresh
  blocks) coordinates with the global free pool — another point
  where contention can spike.

Calling the result "STW-free" would be inaccurate. Calling it
"pause time bounded by O(thread count) rather than O(heap size)"
is accurate.

---

## 6. What This Buys You

If implemented correctly:

- **Pause time decoupled from heap size and root-set size.** Today's
  cooperative-quorum STW is bounded by the slowest running thread
  reaching its next allocation safepoint, which is small in absolute
  terms (sub-ms typical) but is not architecturally bounded — a
  pathological loop without allocations could in principle extend it.
  Under the proposed design, residual pauses are fixed-cost
  operations (root snapshot fence, termination fixpoint) that do not
  scan user data. This is the same category of guarantee that ZGC and
  Shenandoah provide.

- **Better scaling at high thread counts.** Currently a 64-thread
  system has 64 cooperative safepoints to coordinate on every
  collection; the cost grows roughly linearly. Under the proposed
  design that coordination is amortized — mutators run mostly
  uninhibited, and the GC drains its mark queues in parallel.

- **Realtime-friendlier — but not realtime.** A thread with strict
  latency requirements (audio frame, game tick) could run with
  better-bounded tail latency inside a protoCore process that is also
  doing heavy batch work. It still pays the residual pauses; "hard
  realtime" remains the domain of non-GC'd runtimes.

Compared to ZGC / Shenandoah / Go, the *qualitative* result is
similar — bounded pauses decoupled from heap size. The protoCore
advantage is **implementation surface**: the write barrier lives in
one C++ function (`Object::swapInternal`) rather than being injected
by the compiler into every field write site. This is a real
maintainability win, but it is not a different category of
performance guarantee.

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

## 8a. What We Don't Know

This is the section that matters most. The design above is a sketch
informed by knowledge of how ZGC, Shenandoah, Go, and the multi-core
OCaml GC work. It is **not** based on measurements of protoCore or
on a prototype. Before any of this is implemented, the following
unknowns must be characterized:

1. **The actual cost of the write barrier on `setAttribute`.**
   The "~1-2 ns when inactive" estimate is from microarchitectural
   reasoning. Real workloads have cache pressure, branch-prediction
   misses, and surrounding-code interactions that could change this
   by a factor of 2-5. **We have not measured.** A prototype must be
   built and benchmarked on representative workloads (protoPython
   geomean benchmarks, protoJS test262, protoST mt100k) before any
   commitment.

2. **Mark queue contention under realistic mutator load.** A single
   shared queue is a contention nightmare. Per-thread queues are
   simpler but introduce termination-detection complexity. The right
   choice depends on `setAttribute`-rate × `mark window` density,
   which we have not measured.

3. **Termination-detection cost in practice.** The literature offers
   several algorithms (Dijkstra-Scholten, four-counter, wave); each
   has constant factors. The cost of the pragmatic shortcut
   ("short final-mark STW") depends on mark queue depth at fixpoint,
   which depends on mutator behavior — also unmeasured.

4. **Whether the "born during mark = black" assumption holds across
   every code path.** Section 5.3 lists the audit surface. Until
   that audit is done, the assumption is unverified.

5. **The interaction with `UnmanagedScope`.** A thread that enters
   `UnmanagedScope`, performs a syscall, returns, and only *then*
   touches a stale `ProtoObject*` it held before the scope — that is
   already forbidden by the contract, but under concurrent mark a
   *correct* unmanaged scope still raises subtler questions: does the
   write barrier need to fire when the thread returns to managed and
   resumes graph mutation? Probably not (any newly created references
   would go through the barrier), but the argument has not been
   formalized.

6. **The interaction with thread-local arena freelists.** The current
   per-thread arena allocator interacts with the global free pool via
   `getFreeCells`. Under concurrent sweep, the global free pool is
   being mutated by the GC while threads are pulling from it. The
   lock-free coordination here is non-trivial and has not been
   designed.

7. **Worst-case behavior at high thread counts (64+, 256+ cores).**
   We have not stress-tested even the current STW design at these
   counts beyond what fits in CI. The proposed design has more
   coordination points; whether they degrade gracefully or
   pathologically is unknown.

8. **Memory overhead.** Cells born black during mark stay black until
   the next cycle, even if they become garbage immediately. This
   wastes memory proportional to allocation rate × mark window
   duration. The actual cost depends on workload — unmeasured.

9. **The cost of getting it wrong.** A bug in concurrent mark
   manifests as use-after-free, typically far in space and time from
   the actual race. We have no estimate of mean-time-to-detection in
   our test infrastructure. ZGC took multiple years and many JDK
   releases to stabilize; we have far fewer engineering resources.

10. **Whether the structural-sharing patterns in real protoCore
    workloads create hot Cells** that become contention points on
    the mark queue. A widely-shared prototype object mutated
    frequently under concurrent mark is a worst case. Unmeasured.

**The proper response to this list is not "we'll figure it out
during implementation". It is "build the deterministic test
harness and measure the inactive-path cost on a prototype first;
if the numbers do not survive contact with reality, the entire
plan is wrong and we save 6-12 months."**

---

## 9. Open Questions for Future Investigation

1. **Mark queue design**: per-thread MPSC queues drained by the GC
   thread? Per-thread SPSC into a single MPMC? Lock-free Treiber stack?
   The choice has measurable impact on the slow-path cost.

2. **Termination detection**: distributed termination of mark across N
   thread queues is its own subfield. Dijkstra-Scholten? Mattern's
   four-counter? Or simpler: quiesce all threads briefly at the end
   (admitting a short final-mark STW to confirm fixpoint) — this is
   what ZGC and Shenandoah actually do in practice, and it is the most
   honest baseline assumption. The "purely concurrent termination"
   variants are theoretically elegant but their constant factors at
   real thread counts and queue depths are not well-characterized in
   the literature, let alone in protoCore-shaped workloads.

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
   that workload. If p999 < frame budget, stop here. Remember: the
   proposed design does not promise zero pause, only better-bounded
   pause. If the current p999 is already within budget, the proposed
   design buys nothing.

3. **Prototype 5.2 only** — write barrier on the single mutable slot,
   in inactive mode. Measure the inactive-path cost on
   `setAttribute`-heavy workloads (protoPython class instantiation,
   protoJS object construction, protoST message dispatch). If it
   exceeds ~2% geomean, the entire plan is too expensive and the
   priorities are wrong elsewhere.

4. **Complete the code-path audit from Section 5.3.** Every Cell
   construction site must be verified to either go through the
   allocation barrier or be provably unreachable from outside the
   allocating thread until explicit publication. Until this audit is
   done, the design is not even sound on paper.

5. **Build the deterministic concurrent-mark test harness first.**
   Not the implementation. The harness. If you cannot demonstrate that
   you can reproducibly trigger the race in section 4 with the *current*
   GC under instrumentation, you are not yet ready to write the fix.
   ZGC has model-checking infrastructure for exactly this reason — we
   would need analogous tooling.

6. **Only then proceed with the full design.**

---

## 11. Conclusion

A GC with pause time decoupled from heap size — in the same category
as ZGC, Shenandoah, and Go — is plausibly achievable in protoCore.
The single-mutable-slot architecture gives the write barrier a
genuinely simpler correctness story than any mainstream runtime can
offer: one C++ function instead of a compiler pass.

But "plausibly achievable" is doing a lot of work in that sentence,
and Section 8a lists what we don't know. The recipe is *sketched*,
not verified. The cost numbers are *estimated*, not measured. The
correctness argument covers the heap cells but residual pauses
remain (root snapshot fence, termination fixpoint, freelist refill
coordination) — and those are inherent, not engineering laziness.

What we should *not* claim, in either documentation or marketing,
is "zero STW" or "pause-free GC". No production runtime achieves
that. The honest claim, if this is ever implemented and validated,
is "pause time bounded by O(thread count), independent of heap
size or root-set size, with single-function write-barrier surface."

The reason this is research and not a plan is the **silent-failure
mode**. The current STW design is correct by construction. Replacing
it with a concurrent design that is "almost correct" is strictly
worse than the current state. Until there is (a) a demonstrated
workload need, (b) a verifiable correctness story with deterministic
tests, (c) measured cost numbers from a prototype, and
(d) the code-path audits from Section 5.3 completed — the right
answer is to keep the current GC.

Save this analysis. Revisit when a workload demands it. Approach
with humility: the engineering history of concurrent garbage
collection is littered with the silent crashes of designs that
"looked correct".

---

*Document originated from design discussion 2026-05-25.*
*Author of the race construction: project lead.*
*Author of the document: collaborator notes.*
*Status when written: STW + cooperative quorum + UnmanagedScope across
all three runtimes (protoST, protoPython, protoJS) is in place and
producing sub-millisecond pauses.*
*Revision 2026-05-25 (same day): corrected the "zero STW" claim
throughout — no production GC achieves zero pause; the realistic
target is bounded, decoupled pause time. Added Section 8a making
explicit what is unmeasured and unverified.*
