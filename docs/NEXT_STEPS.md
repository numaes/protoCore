# protoCore — Next Steps

Forward-looking notes for work identified but deliberately deferred. Each entry
captures the reasoning chain (alternatives considered and why rejected) so the
intent survives the gap until implementation.

This is not a roadmap and not a commitment. It is a memory aid for design work
that has been done but is not yet justified by measurement.

---

## 1. GC Quorum Latency Reduction via Cooperative Safepoint Densification

**Status:** Designed, not implemented. Awaiting measurement that justifies it.
**Captured:** 2026-05-30.
**Context:** Follow-up to the snapshot-at-STW work that moved Mark out of STW
(see `docs/GarbageCollector.md` § "Concurrent Mark Without Barriers" and
`docs/STW_ELIMINATION_RESEARCH.md` § 13).

### Why this matters

After snapshot-at-STW, the residual STW pause is dominated by two components:

1. **Per-thread context chain walk and cache pinning** (Phase 2 root collection
   work that scales with active threads).
2. **Quorum wait** — time from `stwFlag.store(true)` until every active worker
   thread acknowledges by reaching a safepoint.

Component 2 is unbounded in the current design: a thread executing a tight
computational loop with no allocations may go arbitrarily long without polling
`stwFlag`. The GC blocks until the slowest holdout reaches a natural safepoint
(currently: an allocation or a `goUnmanaged` / `backToManaged` transition).

For soft real-time guarantees, this is the binding constraint. Component 1 is
bounded and small; component 2 is the long tail.

### Alternatives considered and REJECTED

#### Rejected: Asynchronous preemption via signal (Go 1.14+ style)

The idea: when a thread fails to acknowledge within T_max, send `SIGUSR1`; the
signal handler runs on the thread's own stack, walks the `ProtoContext` chain,
publishes a root buffer, and returns.

**Why rejected:** breaks the implicit invariant that makes protoCore's runtime
code safe today.

The current cooperative model has an architectural property worth preserving:
**a C++ local holding a freshly-obtained `ProtoObject*` does not need to be
pinned into the `ProtoContext` while no allocation can occur**. Concretely:

```cpp
auto* obj = someAllocator();          // obj lives in C++ local only
someFunction(obj);                     // passed by value, still C++ local only
auto* obj2 = anotherAllocator();       // SAFEPOINT — but obj was already pinned
                                       // by the caller's protoContext before
                                       // we got here, or it has been published
                                       // into ours
```

Between `someAllocator()` and `anotherAllocator()`, GC cannot fire (suspension
is voluntary). Therefore the bare C++ pointer `obj` is safe even though it is
not registered in any `ProtoContext`. Runtime code throughout protoCore relies
on this implicitly.

Async signal preemption breaks this invariant: the signal can arrive between
the allocation that returned `obj` and any registration of `obj` in a
`ProtoContext`. At that instant the GC sees no root for `obj`, sweep frees its
referent, and the thread resumes holding a dangling pointer.

Recovering correctness under signals requires either pinning every transient
reference (massive code churn and per-call cost) or barriers around every
pointer load (violates the project's "no barriers" principle). Both are worse
than the problem.

The cooperative model's simplicity is therefore not incidental — it is
structurally load-bearing. Preserve it.

#### Rejected: Poll `stwFlag` on every bytecode

The idea: insert an `atomic_load(stwFlag)` in every iteration of the
interpreter dispatch loop and at every loop backedge in compiled code.

**Why rejected:** prohibitive cost for compiled code, marginal for interpreted.

Cost analysis (2 cycles per poll = ~1 ns at 3 GHz):

| Code form          | Per-op cost     | Poll overhead |
|--------------------|-----------------|---------------|
| Interpreted op     | 30-100 ns       | 1-3% — tolerable |
| protoPyC-compiled  | 1-5 ns          | 20-100% — prohibitive |

Beyond raw cycles, polling millions of times per second per core wastes
branch-predictor capacity and i-cache footprint. The intuition that "checking
the flag millions of times per second is excessive" is correct.

### Chosen direction: Counter-amortized voluntary safepoint via the
### line-marker callback

Two design decisions composed.

#### Decision A — Amortize the atomic load behind a thread-local counter

Standard pattern used by HotSpot and similar runtimes:

```cpp
// Per-thread, NOT atomic, lives in ProtoContext or thread-local storage:
uint32_t safepointCounter;  // initialized to SAFEPOINT_INTERVAL

// At each checkpoint:
if (--safepointCounter == 0) {
    safepointCounter = SAFEPOINT_INTERVAL;
    if (space->stwFlag.load(std::memory_order_acquire)) {
        enterSafepointSlow();   // the rare slow path
    }
}
```

Per-checkpoint cost is `dec + jnz`: 2 cycles, zero shared-memory loads, branch
predicted as not-taken with `(SAFEPOINT_INTERVAL - 1) / SAFEPOINT_INTERVAL`
accuracy (essentially perfect). The atomic load happens once per
`SAFEPOINT_INTERVAL` checkpoints.

Trade-off curve at various reset values (compiled code at ~1 ns/op):

| `SAFEPOINT_INTERVAL` | Per-checkpoint overhead | Worst-case quorum wait |
|----------------------|-------------------------|------------------------|
| 256                  | ~0.03%                  | 0.25-1.25 µs          |
| **1024 (recommended default)** | **~0.008%**     | **1-5 µs**            |
| 4096                 | ~0.002%                 | 4-20 µs               |
| 16384                | ~0.0005%                | 16-80 µs              |

At 1024 the overhead is below measurement noise in any realistic benchmark,
and the worst-case quorum wait is better than ZGC's pause-time guarantee.

#### Decision B — Co-opt the existing line-marker callback

Every compiler and interpreter already emits per-line callbacks into the
runtime to maintain `currentLine` in the `ProtoContext` for debugging. This
existing call site has three properties that make it the natural integration
point:

1. **The emission site already exists in every frontend.** No new contract
   imposed on frontend authors who already implement debug line tracking.
2. **The semantic invariant is already guaranteed.** A compiler only emits
   `setLineNumber` at points where the source line is meaningful, which
   implies the `ProtoContext` is in a consistent state. Exactly the invariant
   the safepoint requires, free.
3. **The cost is already being paid.** The call already crosses the function
   boundary and writes the line field. Adding `dec + jnz` to the same call is
   marginal overhead on a cost that already exists.

### Proposed API

Single entry point in `ProtoContext` with two forms:

```cpp
class ProtoContext {
    uint32_t safepointCounter;  // thread-local via context, not atomic
    int      currentLine;
public:
    // Combined form: registers source line AND polls safepoint.
    // Called by frontends that want debug line tracking.
    inline void lineCheckpoint(int lineNo) {
        currentLine = lineNo;
        if (--safepointCounter == 0) {
            safepointCounter = SAFEPOINT_INTERVAL;
            if (space->stwFlag.load(std::memory_order_acquire)) {
                enterSafepointSlow();
            }
        }
    }

    // Safepoint-only form: for release builds or contexts where line
    // tracking is disabled but safepoint coverage is still required.
    inline void safepointTick() {
        if (--safepointCounter == 0) {
            safepointCounter = SAFEPOINT_INTERVAL;
            if (space->stwFlag.load(std::memory_order_acquire)) {
                enterSafepointSlow();
            }
        }
    }
};
```

### Division of responsibility

This is a contract between protoCore and its frontends.

**protoCore provides:**
- The two inline functions `lineCheckpoint(int)` and `safepointTick()`.
- The thread-local counter behind them.
- A configurable `SAFEPOINT_INTERVAL` (default 1024).
- The slow-path implementation `enterSafepointSlow()` with GC handshake.
- Documentation of the contract: "these functions MUST be called only at
  points where the `ProtoContext` chain accurately reflects all live roots".

**protoCore does NOT provide:**
- Decisions about where each frontend emits checkpoints.
- Automatic instrumentation of native C++ primitives.
- Any absolute worst-case quorum-latency guarantee (it depends on how each
  frontend exercises the contract).
- Dynamic detection of pathological allocation-free loops.

**Frontend responsibilities by frontend type:**

- **Interpreters (protoJS, protoPython, protoST interpreter):** call
  `lineCheckpoint` in the dispatch loop, either unconditionally per bytecode
  or via an explicit "set-line" opcode that the compiler emits per source
  line. The counter amortizes the cost regardless.

- **AOT compilers (protoPyC):** emit `lineCheckpoint` at line boundaries in
  debug builds, `safepointTick` at function entry and at every loop backedge
  in release builds. This is one-time work in the code generator (~50 lines
  of backend code).

- **Native C++ primitives:** library authors annotate only the
  potentially-long-running primitives with `safepointTick()` at internal loop
  backedges where the contract holds. Short primitives (arithmetic,
  comparison, hash) need nothing. The list of long-running primitives is
  expected to be short: large-string operations, large-collection traversal,
  multi-level AVL walks.

### Open questions / things to measure first

Before implementing, the empirical questions that change the design:

1. **Does quorum wait actually dominate residual STW in real workloads?**
   Instrument `t_quorum_request → t_quorum_complete` per cycle and per
   slowest-thread. Run against:
   - `mt100k` (protoST messaging benchmark)
   - protoPython benchmark suite at typical worker counts
   - protoJS test262 runs

   If quorum wait is consistently under 100 µs in normal workloads, this
   work is not justified — the natural density of allocations and line
   markers in real code is sufficient. The mechanism then becomes an
   available tool for the rare pathological case, not a systemic change.

2. **What does the distribution of "time between natural safepoints" look
   like per thread?** A histogram from real workloads tells us whether the
   long tail is a few outlier loops (which can be explicitly instrumented)
   or a systemic property (which requires the counter mechanism everywhere).

3. **For native primitives: which ones are actually long-running?** A small
   profiling pass over `protoCore/core/*.cpp` looking for loops that iterate
   over `ProtoList` / `ProtoSparseList` / `TupleDictionary` / large
   `ProtoString` should produce a short inventory.

4. **What value of `SAFEPOINT_INTERVAL` is right?** The trade-off table above
   says 1024 is comfortable, but the actual best value depends on the
   distribution of GC trigger frequency vs the cost of the counter check.
   Start with 1024; tune only if measurement justifies it.

### What this is NOT

- **Not a silver bullet.** The architectural floor remains: a frontend that
  fails to honor the contract cannot benefit from the mechanism. Each
  frontend's worst-case quorum latency is its own design choice.

- **Not a hard real-time guarantee.** protoCore provides a composable
  contract: "if your frontend calls `safepointTick` at least every K
  microseconds of wall time, quorum wait is bounded by K plus cycle
  overhead." This is honest soft real-time positioning, not GC-side promises
  the frontend cannot keep.

- **Not a replacement for `goUnmanaged` / `backToManaged`.** Threads in
  syscalls or extended native operations should continue to use those
  transitions; their roots are pinned and the GC scans them without
  cooperation. The safepoint counter is for threads actively executing in
  managed code.

---

## 2. Other Identified GC Improvements (Not Yet Investigated)

From `docs/GarbageCollector.md` § "Future Improvements", and from the
discussion that produced § 1 above:

### 2a. Soft real-time measurement infrastructure
**Status:** Identified, no design.
**Priority:** Should come FIRST. Provides the dataset that evaluates every
other GC improvement. Without it, all subsequent work is speculation.
**Scope:** Per-component STW timing (handshake / cache pinning / shard
snapshot / tuple root / clear), per-thread quorum-ack latency, histogram of
time between natural safepoints per thread. Exported via existing
`PROTOCORE_GC_PROFILE` instrumentation hook.

### 2b. Parallel mark phase
**Status:** Conceptually clear, design open.
**Why straightforward:** the marked graph is immutable post-construction, so
multiple marker threads can traverse disjoint subgraphs without coordination
beyond the work-stealing deque or static partitioning.
**Open design question:** how aggressively should marker threads compete with
user threads for CPU? Options range from "GC owns N dedicated threads" to
"marker work yields on every chunk if user threads are runnable."

### 2c. Decentralized root scan
**Status:** Designed in this conversation, deprioritized pending measurement.
**Idea:** each thread walks its own `ProtoContext` chain at safepoint entry
and publishes a thread-local root buffer; the GC just concatenates. Work
done where data is hot in cache; scales linearly with threads.
**Why deferred:** the per-thread context walk is probably 1-5 µs and may not
be the bottleneck after § 1 above. Confirm with measurement.

### 2d. Page-protect trick for safepoint poll
**Status:** Considered and tentatively deprioritized.
**Idea:** all threads load from a single mmap'd page in their safepoint poll;
GC `mprotect`s the page to no-access when STW is desired; the next load
faults, the SIGSEGV handler diverts to safepoint.
**Why deferred:** the counter-amortized design in § 1 already brings overhead
below measurement noise. The page-protect trick adds significant complexity
(SIGSEGV handler discipline, interaction with other signals) for a marginal
further reduction. Revisit only if measurement shows the counter check is
≥3% of interpreter cost — unlikely given the trade-off table above.

---

## How to use this file

Add entries when an idea is sufficiently developed to capture but not yet
justified for implementation. Each entry should preserve:

- The reasoning that led to the chosen approach.
- The alternatives considered and **why they were rejected**.
- What measurements would justify implementation.
- What measurements would invalidate the approach.

Remove entries when they are either implemented (move the reasoning to the
implementation's documentation) or definitively abandoned (note the reason).
