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
| protoST S15 | the young generation must be submitted | it did not — 848 tests green, 0 of 2,748,398 cells reclaimed |
| protoClojure idle actor worker | a registered thread that blocks does so inside `UnmanagedScope` | a hang, no message, only under load |
| protoClojure `ActorMessage` payloads | no `ProtoObject*` across an allocation in a bare C++ local | intermittent wrong values |
| protoScala `Mailbox::push` | the same rule — a CAS snapshot held across `appendLast` | nothing, until GC pressure |
| protoST `MailboxCursor::adopt` | the same rule — `unique_ptr::reset` releases a live pin | **unreachable until S15 was fixed**; 848 passing tests could not reach it |
| protoClojure's four blocking joins | a blocking join must not hold the stop-the-world quorum | a 90-second hang; the apparent live set was 90.8× the real one |
| protoCore's own `GCRootScope` test (2026-10) | rule 3 — a nested context's result held in a C++ local across the allocation of the list meant to reference it | about 1 run in 110, macOS arm64 only; 40 local Linux runs green |
| protoPython's call frame (2026-10) | rule 3 — the frame of a running call held only in C++ locals across the interpreter's own `safepoint()`, which submits the young chain that was protecting it | a nested function's `NameError` on its enclosing variable, 4–9 runs in 24, only once collections ran; deterministic with `PROTOCORE_GC_CONTEXT_THRESHOLD=200` |

Two of those entries are a count and a ratio, so this is where each comes from.
protoST's suite was **848/848** when S15 was measured (`protoST/docs/STATUS.md`,
"Test suite"; a widely quoted 833 was the earlier S13 state), and protoClojure's
**90.8×** is 196,519 cells against 2,164 at a 400,000-cell ceiling, the pair
recorded in `protoClojure/tests/cli/loop-garbage-is-reclaimed.sh`. A "110×" that
circulated for both of these had no operand pair behind it; see
`docs/FIELD-NOTES.md`, case 2.

The ratio is **run-variant**, so quote it with its run or re-measure it. Running
that fixture on 2026-09-25 gave `cycles=6 reclaimed-total=1,886,332
live-last=3,873` at the same 400,000-cell ceiling — against the same 196,519,
about 51×. Recorded pairs elsewhere give 70.6 and 362. One pair, from one named
run, is the only honest form.

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
| 3 | **No `ProtoObject*` may be held across an allocation, or across the context's own `safepoint()`, only in a C++ local.** | **J**, with R stress + S for named shapes | The window is one GC cycle wide. Without pressure it never opens; with pressure it is a use-after-free that ASan sees and a green suite does not. A young cell is protected by its context's chain until `safepoint()` submits that chain, so a local that survives every allocation is still lost at the safepoint. | `gc.host_stress` (R), checklist C3 (J) |
| 4 | **Attribute keys are canonical: a source name comes from `ProtoString::createSymbol` at module initialization, a name computed at run time goes through `ProtoString::intern` (3.2.0) before the access — never a plain `fromUTF8String` result.** | R + S, **split by destination** | `setAttribute` auto-interns and `getAttribute` falls back to content, so both are merely slow. **`getOwnAttributeDirect` does neither and silently misses** — and it is the Phase-6 fast path. Names of ≤ 6 UTF-8 bytes are inline in the pointer word and canonical by construction, so short keys work and long ones do not. Tutorial: [docs/tutorials/strings/](tutorials/strings/01_attribute_names.md). | `symbol.fast_path_key_hits` (R), `symbol_key_source` (S) |
| 5 | **`ProtoTuple` is never used for transient data** — every tuple node is interned and perennial. | R (measured, not grepped) | Perennial cells are never swept, so the leak is invisible to every reclamation metric. It looks like a working program with a large heap. | `gc.transient_reclaimed` |
| 6 | **The absent-value sentinel is per function.** | S, per-function table | Comparing against the wrong sentinel yields a branch that can never be taken, or one taken for every object. | `attr_sentinel` |
| 7 | **External memory obeys its contract.** | S + R + J | A missing finalizer leaks silently; a blocking finalizer stalls the whole space's sweep; external bytes are invisible to `heapSize` and to the GC trigger. | `external_finalizer` (S), `external.finalizer_runs` (R), checklist C7 (J) |
| 8 | **No heap-ceiling wait may be reachable only by the thread that can free.** | R, isolated process | The only exit is the controlled abort in `ProtoSpace::waitForHeapHeadroom`. Before that, a silent hang. | `heap.ceiling_progress` |
| 9b | **The module list is a GC root.** | R | "Never freed" is not "is a root" — the list survives and its contents are collected under it. | `module.root_survives_cycle` |
| 9c | **A module's identity is provider + path + version.** | R | Keying by path alone aliases two different modules into one, first load winning. Wrong answer, no error. | `module.alias_rejected` |
| 10 | **A GC or concurrency test that cannot fail is worse than no test.** | R, applied to the suite itself | protoST's suite looked green for its whole history. | `ConformanceSelfCheck.*` |
| 11 | **Every OS thread that holds a `ProtoObject*` must be registered**, or everything it holds must be pinned in a `ProtoRootSet`. | R | GC Phase 2 scans roots by walking `space->threads`. A thread absent from that list is **never root-scanned**: its `automaticLocals`, `closureLocals`, `returnValue` and young chain are invisible to the marker, so its live objects are swept under it. This is *worse* than rule 2 — rule 2 hangs, this corrupts. | `thread.registered` |
| 12 | **A `CriticalSection` must not be held across a blocking wait, an `UnmanagedScope`, or a `safepoint()` expected to submit.** | S | `parkForStopTheWorld` skips parking while `criticalSectionDepth > 0`, and `safepoint()` skips submission at depth > 0. So a critical section held across a wait reproduces rule 2's hang *and* rule 1's non-submission, in code that looks correct. | `critsec_across_block` |
| 13 | **Every cycle in the mutable graph must be declared.** With one space in the process an unreachable cycle among mutable objects is collected; while several spaces are live it is retained. | R (exact, not heuristic) + **J** for which cycles are structural | A cycle the runtime did not intend is a back-reference it did not mean to keep: with one space it keeps the whole structure alive for as long as anything reaches any part of it, and with several spaces live the table's states are roots, so it is never collected until the process is back to one space. Nothing errors either way: it looks like a program with a slightly larger live set. | `mutable.graph_cycles`, checklist C13 (J) |
| 14 | **One exception type, selected by prototype.** A language error that unwinds is a `proto::ProtoException` carrying the value; a handler catches the base type, selects by the value's prototype chain and rethrows the rest unchanged; an unhandled error leaves the runtime as the `ProtoException`. Specification: [EXCEPTIONS.md](EXCEPTIONS.md). | R | A runtime that matches by its own C++ type, an integer class id or a message prefix works for every error it raises itself, so its whole suite is green. It fails only when an error comes from a module or another language: the handler does not run, or catches what it should not, or the error arrives as a string. | `exception.kernel_errors_are_proto`, `exception.value_pinned_in_flight`, `exception.foreign_type_caught`, `exception.language_error_escapes` |

### Recommendation: one `ProtoSpace` per process (3.1.0)

Not a rule with a case, a recommendation with a reason.  **Create one
`ProtoSpace` per process and run everything in it**: the runtime's threads,
its thread pool, its workers, and any module loaded from another runtime,
which adopts the caller's space instead of creating its own.  Values then pass
between them as they are, with no copy and no serialization.  protoCore keeps
supporting several spaces in one process, but while more than one is live the
mutable table's states are roots of every collection (chains of mutable
objects are freed one link per cycle, cycles among them are retained),
collections of different spaces wait for each other, and a value handed to
another space must also stay reachable from its own
([GLOBAL_MUTABLE_TABLE.md](GLOBAL_MUTABLE_TABLE.md), "Recommended model").
`ProtoSpace::liveSpaceCount()` reports how many spaces are live, so a test can
assert that a runtime created none of its own.  A thread declared `OwnSpace`
under rule 11 below is conforming, and is the shape this recommendation
advises against.

### Threads still running when their space is destroyed (Unreleased)

Join a space's ProtoThreads before destroying it.  A runtime that cannot -- a
daemon thread still running at process exit -- no longer gets a
use-after-free: `~ProtoSpace` stops every other thread of the space as a
collection would, and never resumes them (the terminal stop).  A thread
blocks for good at its next safepoint, allocation or heap wait, or, if it is
out in an unmanaged region (blocking I/O, `sleepFor`), when that region
returns; it never runs code of the space again, its stack and thread-local
storage are left as they are, and joining it never returns.  The destructor
waits for each such thread to reach one of those points, so a native loop
with no safepoint keeps it waiting.  A thread of the space that is also a
member of another live space is counted out of that space's quorum for good.
Test: `SpaceTeardown.ThreadsStillRunningAreStoppedForGood`.

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

An unregistered thread that builds its own `ProtoContext` is also a data race,
not only a rooting gap: a context with no thread registers itself as
`ProtoSpace::mainContext`, one slot for the whole space, so two such threads
write it concurrently (ThreadSanitizer reports it in `~ProtoContext` and the
constructor).  protoCore's own suite had six tests of that shape until 2.9.4.

**The release flag of `Host::joinBlockingThread` is written by another
thread.**  Read it with `proto::conformance::releaseFlagRaised(releaseFlag)`
(`protoCoreConformance.h`, since 2.9.4), not with `*releaseFlag`: the
`volatile` in the signature orders nothing between threads, and a plain read is
a data race that ThreadSanitizer reports in the host's own suite.

**The joined thread must still be running when `join.parks` probes it.**  The
case demands a collection 150 ms after it calls `joinBlockingThread`, and raises
the release flag as soon as a cycle has completed (or after 10 s).  A host's
thread either waits for the flag or, when the runtime cannot observe a C++ flag,
runs for a few seconds of wall clock -- bounded by the clock, not by an
iteration count, which finishes in under 150 ms on a fast runner.  A join that
returns before the flag is raised and before any cycle completed is reported as
a **Fail** headed "host contract violation, rule 2b not measured", with the
timings (`joinReturnedAtMs`, `collectionRequestedAtMs`); it is the adaptor's
finding, not the join's.  Until 2.9.5 this case reported such a host as a
rule-2b failure ("no collection cycle could complete"), because its own main
thread then waited in an unbracketed `std::thread::join` and itself held the
quorum; the case's waits now run inside an `UnmanagedScope`.

### Rule 12 and protoCore's own callback walks (Unreleased)

Up to 3.3.1, `ProtoSparseList::processElements` ran its whole AVL walk -- every
callback included -- inside a `CriticalSection`, so an embedder callback that
waited on another ProtoThread, or allocated under a hard heap ceiling, broke
rule 12 without containing a critical section of its own.  As of the next
release no callback walk holds one: `ProtoObject::processOwnAttributes`,
`ProtoSparseList::processElements` / `processValues`, `ProtoMap::processElements`
/ `processValues` and `ProtoKeywordArguments::processElements` all run the
callback at the caller's critical-section depth.  The callback may allocate,
call `safepoint()`, park, and wait on other threads.

What the walk stands on stays reachable **for the duration of the walk**:
`processOwnAttributes` pins a mutable receiver's resolved state, and
`ProtoSparseList::processElements` / `processValues` pin their receiver, in a
root set of the space owned by the walking thread: a set only that thread
writes, without a lock, for the space's main thread and its ProtoThreads; one of
16 shared, mutex-guarded sets for any other thread (beyond 16 such threads they
share them, which costs contention, never correctness).  The pin
ends with the walk.  A tree obtained from `getOwnAttributes()` on a mutable
object and kept in a C++ local *after* the walk is rule 3's concern again: root
it as any other value.

### Rule 13 — why the verdict is a declaration and not "no cycles"

**What the collector does with a cycle among mutable objects** (since 3.1.0,
[MemoryModel.md](MemoryModel.md) § 7): with one space in the process, an
unreachable cycle is collected in the cycle that finds it unreachable, like any
other garbage; a reachable one is live data.  While more than one space is
live, the table's states are roots, and a cycle among mutable objects is never
collected until the process is back to one space.  Until 3.1.0 that was the
case in every process.  The rule here is the part an embedder owes.

A cycle costs memory in both cases -- for as long as anything reaches any part
of it with one space, and for good with several -- and that cost is
**bounded**: the cycle's own cells, once, not growth.  What differs is whether
the cycle is a defect, and protoCore cannot tell:

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
| `0` | any found | **Fail** — an undeclared cycle |
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
violation — `H2`'s liveness follows `H1`'s, and both are released in the same
cycle with one space (one cycle apart with several) — and reporting those would
mean one line per object in the program.  The
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
variable **`PROTOCORE_MUTABLE_CYCLE_CHECK`**: every `ProtoSpace` prints one
report as it is destroyed, from any binary that links `libprotoCore`.  `1` or
`stderr` writes to stderr; anything else is a file path the reports are appended
to, which is what makes the recipe that matters work —

```bash
PROTOCORE_MUTABLE_CYCLE_CHECK=/tmp/scan.txt ctest --test-dir <build> < /dev/null
grep -c '  CYCLE ' /tmp/scan.txt
```

— one sweep over every program the suite runs, without disturbing a single test
that diffs stderr.  It is one `getenv` when unset.

The hook has one honest limitation, found by using it: it fires from
`~ProtoSpace`, so **a process that never destroys its space produces no report.**
That is not rare — a test binary that keeps its space in a static or simply lets
the process exit with it alive is a perfectly ordinary design, and one of the five
runtimes' unit-test binaries behaves exactly that way while its interpreter
executable reports normally.  When the file stays empty, the answer is that no
space was destroyed, not that the graph is clean; call the API directly from that
binary instead.  It is deliberately
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

### Rule 14 — what the four cases see

Two cases are kernel characterisations and need no capability:

- `exception.kernel_errors_are_proto`: protoCore's `1 / 0` raises a
  `ProtoException` built by the space's error factory, which is the host's
  factory when one is installed.
- `exception.value_pinned_in_flight`: an exception kept in a
  `std::exception_ptr` keeps its value through collections after the
  context that built it has ended.

Two cases audit the runtime through two `Host` capabilities:

- `catchForeignError(errorType, raising, &caught)`: the runtime runs its
  language's handler for `errorType` around a call of `raising`, a method
  object that raises an instance of a type the case built, standing for a
  module's type.
  - **Pass:** the handler for the base type receives the very value
    raised, and a handler for an unrelated type lets the same value leave as
    a `ProtoException`.
  - **Fail:** the handler does not run, catches the unrelated type, or the
    value changes. That is the shape of a runtime that matches by C++ type,
    by class id, or catches everything.
- `escapeLanguageError()`: the runtime raises one of its own errors and does
  not handle it.
  - **Pass:** it leaves as a `ProtoException`.
  - **Fail:** it leaves as a `std::exception` or another C++ type.

The reference host (`conformance/SelfHost.h`) implements both in C++. The
mutants `NonConformingHost_CatchesEveryValue` and
`NonConformingHost_EscapesAsStdException` prove that the cases fail when
they should. Tutorial
[05](tutorials/exceptions/05_writing_a_runtime_handler.md) shows a runtime's
implementation.

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
`ProtoObject*` in a C++ local and (b) allocates or CASes, or calls
`safepoint()` on the context that allocated it, while holding it, and
stating for each how it is rooted — `setAutomaticLocal`, the young generation
of the context that allocated it (only until that context's `safepoint()` or
destruction), `ProtoRootSet`, a `CriticalSection`, or an argument that no
allocation occurs.

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

## Heap sizing: one call (2.10.0)

A runtime no longer picks a heap limit.  After constructing its space and
before running the program it calls

```cpp
#if defined(PROTOCORE_HAS_ADAPTIVE_HEAP)
    space->enableAdaptiveHeap();
#else
    space->setHeapLimits(0, legacyHardCells);   // protoCore < 2.10
#endif
```

and deletes its own default (protoST's `configureHeap` constant, protoJS's
640 MB or 75 % helper, and the limits protoScala, protoClojure and protoPython
set).  `PROTOCORE_HEAP_LIMIT_CELLS` still overrides the hard limit.  The
conformance cases that need a ceiling (`heap.ceiling_progress`,
`gc.host_stress`) still impose one with `setHeapLimits`, which disables the
controller for the case, and give it back afterwards, resumed from the soft
limit it had reached.  The controller paces cycles
while the program runs, so it depends on rules 1 and 2: a young generation
that is never submitted is live to it too, and a thread that reaches no
safepoint turns every cycle into a stall, which the controller answers by
growing the heap (since 2.10.1 only up to 8 x the live set).  Mechanism,
defaults and measurements: [GarbageCollector.md](GarbageCollector.md)
§ "Adaptive heap controller".  Before switching, measure the runtime's own
workloads under the controller with `PROTOCORE_ADAPTIVE_HEAP=1` (no rebuild
needed): [reports/2026-10-03-adaptive-heap-calibration.md](reports/2026-10-03-adaptive-heap-calibration.md)
lists where it costs time against today's fixed limits.

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
