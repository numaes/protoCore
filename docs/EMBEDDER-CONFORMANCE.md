# Embedder conformance — protoCore's participation obligations, in executable form

protoCore's value proposition is that the embedder does not manage memory, does
not take a global lock and does not write barriers. The price is a handful of
**participation obligations**, and the failure mode of skipping one is almost
never a crash at the point of the mistake — it is unbounded RSS, a hang under
contention, or a use-after-free weeks later.

**This document is normative.** Every rule below is executable: as a case in
`libprotoCoreConformance` (**R**), as a check in
`scripts/conformance/check_static.py` (**S**), or as a checklist item a human
answers in the embedder's own `docs/CONFORMANCE.md` (**J**). Labelling a rule
**J** is a result, not a gap: a suite that claims to mechanise a judgement call
is worse than one that admits the split, because it will be believed.

## Why this exists, in one table

Every deep bug found across this family in 2026-09 was a documented protoCore
rule that an embedder simply did not follow, and **not one of them failed
loudly**.

| Bug | The obligation that was omitted | How it announced itself |
|---|---|---|
| protoST S15 | the young generation must be submitted | it did not — 833 tests green, 0 of 2,748,398 cells reclaimed |
| protoClojure idle actor worker | a registered thread that blocks does so inside `UnmanagedScope` | a hang, no message, only under load |
| protoClojure `ActorMessage` payloads | no `ProtoObject*` across an allocation in a bare C++ local | intermittent wrong values |
| protoScala `Mailbox::push` | the same rule — a CAS snapshot held across `appendLast` | nothing, until GC pressure |
| protoST `MailboxCursor::adopt` | the same rule — `unique_ptr::reset` releases a live pin | **unreachable until S15 was fixed**; 848 passing tests could not reach it |
| protoClojure's four blocking joins | a blocking join must not hold the stop-the-world quorum | a 90-second hang; the apparent live set was 110× the real one |

That fifth row is the argument for a suite rather than a review, in one line:
**the third bug of the class was invisible because the first bug of the class
was still present.** A reviewer on 2026-09-23 would have found neither, because
they would have been reading a program in which the collector never ran.

## The rule table

| # | Rule | Mechanism | Why omitting it is silent | Case / check |
|---|---|---|---|---|
| 1 | **The young generation must be submitted.** A context's young chain reaches the collector only through `ProtoContext::safepoint()` or the context's destruction. | R | An unsubmitted chain is live by construction. The heap grows; nothing errors; `gcCycleCount` still advances, so instrumentation that counts *cycles* looks healthy. | `gc.young_submitted` |
| 2 | **Every registered protoCore thread must park.** A thread that blocks does so inside `ProtoContext::UnmanagedScope`. | R + S | STW Phase 1 waits for `parkedThreads >= runningThreads` forever. No collection completes, so the symptom is a hang under load, not a failure. | `stw.quorum_completes`, `blocking_join_unbracketed` |
| 2b | **A blocking join must not hold the quorum.** `ProtoThread::join` brackets itself as of 2026-09-25; a direct `std::thread::join` or `pthread_join` on a registered thread does not. | R + S | See rule 2. The joined thread usually cannot finish either, because it needs memory that only a cycle would free. | `join.parks`, `blocking_join_unbracketed` |
| 3 | **No `ProtoObject*` may be held across an allocation only in a C++ local.** | **J**, with R stress + S for named shapes | The window is one GC cycle wide. Without pressure it never opens; with pressure it is a use-after-free that ASan sees and a green suite does not. | `gc.host_stress` (R), checklist C3 (J) |
| 4 | **Attribute keys come from `ProtoString::createSymbol`, never `fromUTF8String`.** | R + S, **split by destination** | `setAttribute` auto-interns and `getAttribute` falls back to content, so both are merely slow. **`getOwnAttributeDirect` does neither and silently misses** — and it is the Phase-6 fast path. Names of ≤ 6 ASCII bytes are inline in the pointer word and match by accident, so short keys work and long ones do not. | `symbol.fast_path_key_hits` (R), `symbol_key_source` (S) |
| 5 | **`ProtoTuple` is never used for transient data** — every tuple node is interned and perennial. | R (measured, not grepped) | Perennial cells are never swept, so the leak is invisible to every reclamation metric. It looks like a working program with a large heap. | `gc.transient_reclaimed` |
| 6 | **The absent-value sentinel is per function.** | S, per-function table | Comparing against the wrong sentinel yields a branch that can never be taken, or one taken for every object. | `attr_sentinel` |
| 7 | **External memory obeys its contract.** | S + R + J | A missing finalizer leaks silently; a blocking finalizer stalls the whole space's sweep; external bytes are invisible to `heapSize` and to the GC trigger. | `external_finalizer` (S), `external.finalizer_runs` (R), checklist C7 (J) |
| 8 | **No heap-ceiling wait may be reachable only by the thread that can free.** | R, isolated process | The only exit is the controlled abort in `ProtoSpace::waitForHeapHeadroom`. Before that, a silent hang. | `heap.ceiling_progress` |
| 9b | **The module list is a GC root.** | R | "Never freed" is not "is a root" — the list survives and its contents are collected under it. | `module.root_survives_cycle` |
| 9c | **A module's identity is provider + path + version.** | R | Keying by path alone aliases two different modules into one, first load winning. Wrong answer, no error. | `module.alias_rejected` |
| 10 | **A GC or concurrency test that cannot fail is worse than no test.** | R, applied to the suite itself | protoST's suite looked green for its whole history. | `ConformanceSelfCheck.*` |
| 11 | **Every OS thread that holds a `ProtoObject*` must be registered**, or everything it holds must be pinned in a `ProtoRootSet`. | R | GC Phase 2 scans roots by walking `space->threads`. A thread absent from that list is **never root-scanned**: its `automaticLocals`, `returnValue`, `pendingRoot` and young chain are invisible to the marker, so its live objects are swept under it. This is *worse* than rule 2 — rule 2 hangs, this corrupts. | `thread.registered` |
| 12 | **A `CriticalSection` must not be held across a blocking wait, an `UnmanagedScope`, or a `safepoint()` expected to submit.** | S | `parkForStopTheWorld` skips parking while `criticalSectionDepth > 0`, and `safepoint()` skips submission at depth > 0. So a critical section held across a wait reproduces rule 2's hang *and* rule 1's non-submission, in code that looks correct. | `critsec_across_block` |
| 13 | **Every cycle in the mutable graph is permanent retention, and must be declared.** A cycle among mutable objects is never collected. | R (exact, not heuristic) + **J** for which cycles are structural | The table is a GC root unconditionally and an entry is released only once its handle has been finalized, so a value that reaches its own handle keeps it marked for ever. Nothing errors, nothing grows without bound, and no reclamation metric moves: it looks like a program with a slightly larger live set, for ever. | `mutable.graph_cycles`, checklist C13 (J) |

### Rule 11 accepts three conforming shapes

The naive form — "every thread is created through `ProtoSpace::newThread`" —
would report a correct runtime as broken. Registering a thread that holds
nothing adds it to the STW quorum for no benefit, making every pause wait for a
thread with nothing to contribute. So `Host::ThreadKind` carries a **declared
verdict**, and the case's job is to **verify the declaration rather than take
it**:

| Verdict | Meaning | What the case checks |
|---|---|---|
| `Registered` | created through `ProtoSpace::newThread` | `ctx->thread != nullptr` **and** the thread is present in `space->threads` |
| `HoldsNothing` | declares it touches no `ProtoObject*` | `ctx->allocatedCellsCount == 0` after the probe ran |
| `OwnSpace` | is the main thread of its own `ProtoSpace` | the thread's `space` differs from `mainContext()->space` |

A kind that declares `HoldsNothing` and then allocates is a **Fail**, and that
is the finding worth having: a declaration the code contradicts.

### Rule 13 — why the verdict is a declaration and not "no cycles"

**A cycle among mutable objects is never collected.**  The property, its proof
against the two collector sites that produce it, and the fixes that were
considered and rejected are in [MemoryModel.md](MemoryModel.md) § 7.  The rule
here is the part an embedder owes.

The retention is real in every case, and **bounded**: the cycle's own cells,
once, not growth.  What differs is whether the cycle is a defect, and protoCore
cannot tell:

| Cycle | Verdict |
|---|---|
| protoPython's `fn → __closure_frames__ → frame → co_name → fn` | **incidental.** `co_name` was a diagnostic pointer. Removable, and removed |
| protoScala's captured `var` — `var f = null; f = () => f()` | **structural.** The cell is a mutable because sharing a `var` between closures is what it is for; a snapshot would freeze it at `null` and the program would be wrong |
| a doubly-linked list of mutable nodes; two actors referencing each other | **structural.** The back-edge is the data structure |

So the rule is **not** "the mutable graph must be acyclic", which is false.  It
is: *where a back-reference is incidental, store the current value — an
immutable snapshot — instead of the mutable, and make taking that snapshot an
explicit operation at the use site* (the Clojure distinction between a reference
and `@ref`, which protoClojure already ships); *and where it is structural,
declare it.*

The case therefore verifies a **declaration against a measurement**, the same
shape as rule 11's `ThreadVerdict`:

| `Host::declaredMutableCycles()` | Scan result | Status |
|---|---|---|
| `0` | none found | **Pass** |
| `0` | any found | **Fail** — undeclared permanent retention |
| `n > 0` | at most `n` found | **Pass**, with each path in `detail` |
| `n > 0` | more than `n` found | **Fail** |
| `-1` (not declared) | any found | **NeedsReview**, with the paths |
| any | table did not grow across `makeMutableGraph()` | **NotApplicable** — the scan covered nothing this runtime built |

**The detector is exact, not heuristic**, and this is the one rule where that
can be said.  The mutables table enumerates every written handle in the space,
and every cell field is `const` after construction, so the only edge in the heap
that can close a loop is the handle → state indirection the table implements.
`ProtoSpace::findMutableCycles` builds the augmented cell graph and runs one
Tarjan pass over it: O(cells + references), no sampling, no threshold.

**Only cycles are reported.**  An acyclic handle-to-handle edge is not a
violation — `H2`'s liveness follows `H1`'s and Phase 5b releases it a cycle
later — and reporting those would mean one line per object in the program.  The
commonest shape in this family is exactly that: every mutable instance of a
mutable class prototype.

**Two ways to ask the question.**  The rule applies to all five runtimes, and
only two have a Host adaptor, so the detector is a **public protoCore API** and
not only a conformance case:

```cpp
const proto::MutableGraphReport r = space.findMutableCycles(ctx);
if (!r.cycles.empty()) std::cerr << r.summary();
```

and, for a runtime with no adaptor and no wish to add code, the environment
variable **`PROTOCORE_MUTABLE_CYCLE_CHECK`**: set it to anything and every
`ProtoSpace` prints one report to `stderr` as it is destroyed, from any binary
that links `libprotoCore`.  It is one `getenv` when unset.  It is deliberately
not hooked into the end of a GC cycle: the walk is O(live mutable graph), and a
per-cycle hook would need a frequency policy and would stall the collector on a
schedule nobody chose.  A process that never exits calls the API itself.

**`truncated` is not `clean`.**  A scan that exhausts its cell budget still
reports every cycle it found — an edge it found is an edge that exists — but the
absence of others is not established.  `acyclicAndComplete()` is the only
reading that means clean, and the case Fails on a truncated scan rather than
passing it.

### Rule 6 — the per-function sentinel table

A naive check ("flag `== nullptr` near an attribute call") would flag **correct**
code. The conventions genuinely differ:

| Function | Not found | Invalid input | Citation |
|---|---|---|---|
| `getAttribute` | `PROTO_NONE` | `nullptr` | `core/ProtoObject.cpp:773-781` — *"nullptr is reserved for invalid input, not 'missing'"* |
| `getOwnAttribute` | `PROTO_NONE` | `nullptr` | same convention |
| `getOwnAttributeDirect` | **`nullptr`** | `nullptr` | `headers/protoCore.h` |
| `hasAttribute` / `hasOwnAttribute` | `PROTO_FALSE` | — | a boolean **object**, never a C++ `bool` |

So `getOwnAttributeDirect(...) == nullptr` is **right**, and a checker that
flagged it would train everyone to ignore the tool. The check is a table lookup:
the sentinel compared against must match the function called.

Two consequences worth a maintainer's attention:

- **`getOwnAttributeDirect` cannot distinguish "absent" from "not an object
  cell".** That is a genuine expressive gap in protoCore, it is why this class
  of confusion exists at all, and the documented way round it is to probe with
  `hasOwnAttribute` or `hasAttribute` first. P4 documents the gap and does not
  close it: changing a return convention is an ABI-visible semantic change.
- **`hasAttribute` returns `PROTO_TRUE` (`1217UL`) or `PROTO_FALSE` (`193UL`),
  both non-null.** So `if (obj->hasAttribute(ctx, k))` is **always true**. This
  is the cheapest silent bug in protoCore's whole surface, and the first run of
  this checker found three live instances of it in one runtime.

### Reclamation assertions

**`reclaimed > 0` is forbidden in this suite, and the API makes it
unwritable.** `conformance/CycleDriver.h` offers no "did it reclaim anything"
call; the only reclamation verdict it exposes takes the denominator as an
argument. The denominator is the **space's own in-use cell count**
(`heapSize - freeCellsCount`), not `ProtoContext::allocatedCellsCount`:
`safepoint()` sets that counter to 0 every time it submits, so a delta of it is
smallest exactly when a runtime is conforming, and an unsigned subtraction
underflows into an astronomic one.

`kReclaimFraction` is **0.50** and tuning it per runtime is forbidden. A
conforming runtime does not return 100% of what it took, for three legitimate
reasons (late submission belongs to the next cycle; survivor stagger; the
driver's own allocations and each thread's unused free-cell batch). A
**non**-conforming runtime returns a rounding error: 0 of 2,748,398 in one
measured case, 4–7 of 205,120 in another. The threshold discriminates anywhere
in `[0.05, 0.9]`; 0.50 is a round number nobody will be tempted to tune.

A workload that grows the heap by less than `kMinWorkloadCells` (200,000)
reports **NotApplicable**, not Pass: below that the fixed lag is not a small
fraction of the denominator.

## The judgement items

Recorded per embedder in that embedder's `docs/CONFORMANCE.md`, each with a
`Reviewed:` line.

### C3 — Is any `ProtoObject*` held across an allocation only in a C++ local?

**Mechanised:** `gc.host_stress` under a heap ceiling and under
AddressSanitizer, which is what actually caught all three met instances.

**Not mechanised:** the general case. Deciding it requires knowing, for an
arbitrary call, whether it can allocate, and whether an arbitrary local is still
live afterwards. That is **escape analysis over the whole call graph**, and a
checker that claimed to do it would produce a false-negative rate nobody could
estimate — which is worse than a checklist, because it would be *believed*.

**Answer by:** naming every path in this runtime that (a) holds a
`ProtoObject*` in a C++ local and (b) allocates or CASes while holding it, and
stating for each how it is rooted — `setAutomaticLocal`, `pendingRoot`,
`ProtoRootSet`, a `CriticalSection`, or an argument that no allocation occurs.

### C5 — Which structure is rule 5 measured on?

The measurement is mechanical; choosing *which* of the runtime's types is "the
sequence a user gets from a literal" is a reading of the runtime. The Host
answers it by implementing `makeSequenceGarbage`, and the checklist records which
type was chosen **and why**. Choosing the type whose representation is known to
be safe, when the runtime has two, is choosing the workload to get the answer.

### C13 — Which cycles in the mutable graph are structural?

**Mechanised:** finding them.  `ProtoSpace::findMutableCycles` is exact and
reports every one with its path, and `mutable.graph_cycles` fails on any cycle
beyond the declared count.

**Not mechanised:** deciding which ones *should* be there.  A cycle between a
closure and the cell it captured is what a recursive `var` means; a cycle
through a diagnostic back-pointer is an oversight.  Distinguishing them requires
knowing what the language guarantees its users, which protoCore does not and
must not know — and a checker that guessed would call a correct runtime broken,
which is how a tool gets switched off.

**Answer by:** listing every cycle the scan reports, with its path, and for each
saying whether it is *incidental* (and then removing it, by storing the current
value instead of the mutable) or *structural* (and then what bounds how many of
them the program creates).  Set `Host::declaredMutableCycles()` to the count of
the structural ones, so a new one cannot appear silently.

### C7 — Is external memory correctly accounted and correctly released?

`externalBytesAccounted()` mechanises *"does an accounting exist at all"*.
Whether the number is **right** — whether a region reached through two wrappers
is double-counted, whether a null finalizer is correct because the embedder frees
it elsewhere — is unknowable to protoCore by construction. `docs/MemoryModel.md`
§5 is the normative statement of exactly this: *"External size is whatever the
embedder declares… A collection policy driven by that number is a policy driven
by a figure that can drift arbitrarily from reality, and the kernel has no way
to detect the drift."* A conformance suite is in the same position, and says so.

## How an embedder runs the suite

Three lines. protoCore ships the cases as a framework-free static library, and
the embedder's own test framework does the asserting.

```cpp
// GoogleTest
#include <protoCoreConformanceGTest.h>
PROTOCORE_CONFORMANCE_GTEST(MyConformanceHost)

// Catch2
#include <protoCoreConformanceCatch2.h>
PROTOCORE_CONFORMANCE_CATCH2(MyConformanceHost)

// One case per process, for the case whose failure mode is std::abort()
PROTOCORE_CONFORMANCE_ISOLATE_MAIN(MyConformanceHost)
```

The static half:

```bash
python3 <protoCore>/scripts/conformance/check_static.py --repo <embedder>
```

**Run every `ctest` with stdin at EOF** (`< /dev/null`). A conformance runner
that hangs is indistinguishable from a conformance failure that hangs, and
rules 2, 2b, 8 and 11 are exactly the rules whose failure mode *is* a hang.

## What a status means

| Status | Meaning |
|---|---|
| `PASS` | the invariant was observed, with the numbers in `detail` |
| `FAIL` | the invariant was violated. There is no reading of this that is acceptable |
| `NOTAPPLICABLE` | a capability the case needs is not implemented. **The rule is UNVERIFIED for this runtime.** Never a pass |
| `SKIPPED` | the case aborts on failure and must be run through the isolate binary |
| `NEEDSREVIEW` | the mechanised half passed and a judgement item is outstanding |

**`NotApplicable` is never `Pass`.** A suite that reports `NotApplicable` for
`makeGarbage` has told the maintainer exactly why its GC cases mean nothing —
which is the thing 833 green protoST tests never said.

## protoCore names capabilities, never runtimes

`Host` declares *capabilities*. Grep for a runtime's name in `conformance/` and
in `headers/protoCoreConformance*.h` returns nothing, and
`ConformanceSelfCheck.ProtoCoreNamesNoRuntimeInAnyCaseDetail` asserts it rather
than intending it. A per-embedder special case inside the library is the failure
mode the adaptor exists to prevent.
