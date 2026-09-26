# Field notes: fourteen defects, 2026-09-23 to 2026-09-25

This document records real defects found in protoCore and the five runtimes built
on it, over three days. For each one it states the symptom as it appeared, what
everyone believed before, what actually disproved that belief, the measurement,
a command a reader can run, the fix, and what remains unproven.

**It is written to be uncomfortable.** Several cases are corrections of our own
earlier conclusions, one is a benchmark comparison we published and retracted the
same day, and one is a defect whose two candidate fixes have never been written
down anywhere despite being discussed as though they had. A document that showed
only the parts that worked would be advocacy, and useless for teaching.

`docs/EMBEDDER-CONFORMANCE.md` is the normative companion: it states as rules
what these cases found the hard way, and makes most of them executable. Read that
for what you must do; read this for what happens when you do not, and for how the
evidence was actually obtained.

---

## How to read a case

Every case has the same skeleton, and the skeleton is the method:

| Heading | What it must contain |
|---|---|
| **Symptom** | what a human saw, in the words it was first reported in |
| **What was believed** | the explanation everybody held, including the filed diagnosis |
| **What disproved it** | the specific measurement or reading that broke the belief |
| **Measurement** | numbers, each traceable to a file, a log or a commit body |
| **Reproduction** | a command, not a description of one |
| **Fix** | the commit |
| **Unproven** | what the case still does not establish |

Where a reproduction needs a heap ceiling to be visible, the ceiling value is
given. That is not a convenience: **protoCore starts a collection cycle only
under cell pressure, so without a ceiling most of the defects below are
completely invisible, and three of them survived months of green tests for
exactly that reason.**

## The machine and the versions

Every measurement quoted was taken on one machine: **AMD Ryzen 5 5500U, 6
physical cores / 12 logical, single socket**, Linux 7.0.0-31-generic, on a shared
daily-driver desktop whose load average floor is 2–3. Where load matters to a
result, the case says so — twice it changed what a run reported, and once it is
the reason a benchmark conclusion had to be retracted.

protoCore moved 2.0.0 → **2.4.0** across these three days (`CHANGELOG.md`), with
`PROTOCORE_ABI_SOVERSION` going 2 → 3 once, at 2.2.0. Repository heads used
below: protoCore `d7d03b42`, protoST `945fd8e`, protoClojure `f1d8b89`,
protoPython `944dd628`, protoJS `e4e80430`, protoScala `ddb0038`.

## Running the reproductions

The protoCore commands assume a configured Release build in
`protoCore/build_release`. The embedder commands assume each project's own
`build_release`. Two rules apply to every `ctest` invocation in this document:

```bash
ctest ... < /dev/null        # rules 2, 2b, 8 and 11 fail by hanging, and a
                            # runner waiting on stdin is indistinguishable
ctest ... -V                # several cases print their measurement; plain
                            # ctest shows only "Passed"
```

On this machine `protoJS` must be built without `-j`, and the test262 sweep must
run sequentially; both hang the host otherwise.

---

# 1. protoST reclaimed nothing for its entire history

**Symptom.** Filed as *"A forced garbage collection never runs."* The report is
worth quoting, because its first sentence is the part that was wrong
(`git show 3dbd039^:docs/STATUS.md`, line 296):

> **A forced garbage collection never runs.** `ProtoSpace::triggerGC()` called
> eight times from the main thread leaves `ProtoSpace::getGCCycleCount()` at 0 —
> with actors, without actors, and after 20 000 throwaway allocations — so no
> collection cycle ever completes. […] Probably the same root cause as S3 […]
> plus the root context never submitting its young generation, but that is a
> hypothesis, not a diagnosis.

**What was believed.** Two things, both wrong in different ways. First, that no
cycle completed. Second — since protoClojure had just been found hanging on an
idle actor worker — that protoST's actor worker was holding the stop-the-world
quorum. The young-generation explanation was in the report, but listed second
and explicitly unproven.

**What disproved it.** Measurement of the cycle counter rather than inference
from the abort message. Cycles **did** run — 10, then 17, then 22 of them across
four rounds — and every one reclaimed exactly 0 cells. The actor hypothesis died
at the same time and for a simpler reason: the symptom was byte-identical with
and without actors, because protoST's worker already waits inside a
`ProtoContext::UnmanagedScope`.

The real cause is one omission. A context's young cell chain reaches the
collector in exactly two places: when the context is destroyed
(`core/ProtoContext.cpp:262`) and from `ProtoContext::safepoint()` past
`maxAllocatedCellsPerContext` (`core/ProtoContext.cpp:376-387`). Phase 2 records
a young chain as a **root handle**, not as a candidate
(`core/ProtoSpace.cpp:364-375`), so an unsubmitted chain is live by construction.
protoST creates no `ProtoContext` of its own — a program runs on the runtime's
root context and an actor turn on its worker's root context, both alive for the
life of the process — and the interpreter called `safepoint()` nowhere at all.

**Measurement.** After four 20,000-iteration allocating rounds the root context's
chain held **2,748,398 cells**, with 0 reclaimed. After the fix the same
workload holds the heap flat at 1,048,576 cells with a ~242,000-cell live set,
reclaiming ~730,000 cells per cycle. Interpreter cost of the added safepoint:
+2.7 % instructions, +4 % cycles on a 20,000,000-iteration integer loop
(`perf stat -r 3`).

> **Two figures to distrust.** The 2,748,398 and the per-cycle numbers are
> recorded only in the body of commit `3dbd039` and in documents quoting it; no
> raw measurement log survives. And the widely repeated pairing *"833 tests
> green"* mixes two moments: 833/833 was protoST's suite size at the earlier S13
> fix. When S15 was measured the suite was **848/848**, and `3dbd039` reports
> 851/851. It is 854 today. protoCore's own source comment
> (`conformance/CaseGC.cpp:90`) and `CHANGELOG.md:218` both still say 833.

**Reproduction.** protoST has no GC statistics switch; the only black-box signal
is protoCore's abort under a ceiling. The fixture does it both ways:

```bash
cd /home/gamarino/Documentos/proyectos/protoST/build_release
ctest -R cli_gc_reclaims -V < /dev/null
```

It runs an allocating loop under a heap ceiling, then runs the same workload
again with `PROTOST_NO_GC_SAFEPOINT=1` and **fails if that second run succeeds**.
With the hook off, the unit case reports 5 cycles, 0 cells reclaimed, and a heap
grown from 1,048,576 to 3,866,624 cells.

**Fix.** protoST `3dbd039` — `ExecutionEngine::gcSafepoint` calls
`ProtoContext::safepoint()` at the loop back-edge
(`src/runtime/ExecutionEngine.cpp:1799`, in `case Op::JUMP_BACK`) and at engine
entry (`:441`). Those are the two points at which the engine holds no half-built
value in a C++ local. Kill switch at `:426-427`.

**Unproven.** Bug S3 — that an *allocation-free* loop stalls a collection — is
addressed by the same mechanism and is **still open**, because it could not be
falsified: no allocation-free loop can be written in protoST. Every loop shape
measured allocates about three cells per iteration even when its body only adds
SmallIntegers, so every such loop already parks at `allocCell`'s
every-64-allocations poll. A test written against a loop of that shape passed
against a build with both safepoints removed, and was withdrawn rather than kept
(`protoST/docs/STATUS.md:305`).

**Lesson.** "No cycle completes" and "every cycle reclaims nothing" produce the
same user-visible abort, and only the second is what happened; a bug report is a
symptom, and taking its first sentence as the diagnosis cost the first day.

---

# 2. protoClojure's apparent live set was ninety times the real one

**Symptom.** Under a heap ceiling, an ordinary allocating loop aborted with
`last cycle reclaimed 0 — out of memory`. Without a ceiling the same program ran
to completion and looked healthy.

**What was believed.** Nothing, for about three months. protoClojure's loop/recur
VM landed on 2026-06-14 (`d25ba50`) and there was no reason to think anything was
wrong: the suite was green, and every benchmark finished.

**What disproved it.** Setting a heap ceiling. This is the whole teaching point
of the case and it deserves stating plainly: **with no ceiling the difference
between the bug and the fix is invisible.** Measured on the current binary, same
workload, same machine:

```
PROTOCLJ_GC_STATS=1 protoclj churn.clj
  protoclj gc: cycles=0 reclaimed-total=0 ... heap=2621440 heap-start=262144
PROTOCLJ_NO_GC_SAFEPOINT=1 PROTOCLJ_GC_STATS=1 protoclj churn.clj
  identical line, byte for byte
```

Zero cycles either way. The collector is never asked to run, because protoCore
starts a cycle only from `ProtoSpace::waitForHeapHeadroom` under cell pressure —
`triggerGC()` is advisory. So the defect was behind green tests not because the
tests were weak about reclamation, but because **nothing in the project had ever
created the condition under which reclamation happens at all.**

**Measurement.** The fixture's own recorded pair, at a 400,000-cell ceiling:
apparent live set **196,519 cells** without the safepoint, against a real live
set of **2,164 cells** measured with it — a factor of **90.8**. The bug hides two
orders of magnitude of garbage inside a figure labelled "live set".

> **Numbers circulating in this project that I could not confirm.** A "246,523"
> apparent live set: the real figure at a 500,000-cell ceiling is **246,519**. A
> "2,375,888 cells reclaimed": the measured totals are 1,886,332 at a 400,000
> ceiling and 1,898,028 at 500,000. A "true live set of 2,245": recorded values
> are 2,164 (the fixture) and 2,753 (a larger run); measured today, 3,873 and
> 3,493. "Six cycles at a 500,000 ceiling": six is the 400,000 result, 500,000
> gives five. And **"110×", which protoCore itself asserts twice —
> `conformance/CaseGC.cpp:90` and `CHANGELOG.md:219` — has no recorded operand
> pair anywhere.** Depending on which recorded pair you use, the ratio is 70.6,
> 90.8 or 362. Use one pair from one run, and say which run.
>
> "Two months behind green tests" is also wrong: it is **three months and eleven
> days** from the loop VM (2026-06-14) to the fix (2026-09-25).

**Reproduction.** The ceiling is the instrument, and its value matters:

```bash
cd /home/gamarino/Documentos/proyectos/protoClojure/build_release
ctest -R 'cli/loop-garbage-is-reclaimed' -V < /dev/null
```

The fixture asserts on numbers, not on "more than zero": the program's own
computed answer (so a crash cannot read as a pass), cells reclaimed against the
~2,360,000 cells of garbage the workload actually creates, the last cycle's live
set against the loop's real live set, and the heap high-water mark. It then runs
the same workload with `PROTOCLJ_NO_GC_SAFEPOINT=1` and fails if that succeeds.

**Fix.** protoClojure `c0bc512` — `gcSafepoint` at the `JUMP_BACK` back-edge
(`src/runtime/ExecutionEngine.cpp:946`) and at frame entry (`:519`), on a stride
of 64 (`src/runtime/ExecutionEngine.h:194`). Preceded deliberately by `b6267ee`,
which added `PROTOCLJ_GC_STATS` so the collector could be measured before it was
changed.

**Unproven.** protoClojure still installs no heap limit of its own, so in normal
use nothing is ever collected. That is recorded as a high-severity known issue
(`protoClojure/docs/STATUS.md:500-516`), not as fixed.

**Lesson.** A garbage-collection bug is only observable under memory pressure, so
a test suite that never creates memory pressure cannot detect one, however many
tests it has.

---

# 3. `ProtoThread::join` was a deadlock, not slow shutdown

**Symptom.** protoClojure hung. Under load, with no message, in `future` deref,
in `pmap`, and at shutdown. Each hang ran to a 90-second test timeout.

**What was believed.** That shutdown was slow — the sort of thing you fix by
raising a timeout.

**What disproved it.** A live backtrace, read under gdb and preserved. Three
frames are the whole diagnosis:

```
Thread 2 (main):   #4 std::thread::join()   #5 protoClojure::prim_deref
Thread 4 (future): #5 ProtoThreadImplementation::implSynchToGC()
                   #7 ProtoSpace::waitForHeapHeadroom(...)
                   #8 ProtoContext::fromUTF8String(...)   #9 prim_str
Thread 3 (GC):     #5 gcThreadLoop(ProtoSpace*)
```

The joiner holds the quorum; the joinee waits for memory only a cycle can free;
the collector waits for a quorum that can never be met. The thread waiting for
memory **is** the thread being joined. That combination is arithmetically a
deadlock rather than a delay:

- `runningThreads` starts at 1 — the main thread is counted from `ProtoSpace`
  construction (`core/ProtoSpace.cpp:1123`) — and every managed thread adds one.
- A stop-the-world phase cannot begin until `parkedThreads >= runningThreads`
  (`core/ProtoSpace.cpp:322`).
- A bare `std::thread::join` reaches no safepoint, so a registered thread blocked
  there **still counts as running**. The quorum can never be met, no cycle can
  start, and every thread that then needs memory waits for a cycle that cannot
  begin — usually including the very thread being joined, which is why the join
  never returns either.

**Measurement.** Four unbracketed sites in protoClojure: `future` deref
(`src/runtime/Primitives.cpp:1613`), `pmap` (`:2172`), `shutdownFutures`
(`:1958`) and `ActorScheduler::shutdown`
(`src/runtime/ActorScheduler.cpp:94`). All four hung to the fixture's 90-second
deadline; all four complete in a few seconds once bracketed, reclaiming roughly
3–6 million cells over 7–13 cycles. **Four, and the pre-registered prediction said
three** — it named the right three files and missed `pmap` entirely, which is
recorded as a wrong prediction rather than quietly corrected.

The three-leg experiment that separates the kernel fix from the embedder guards
(protoClojure `036761f`):

| configuration | result |
|---|---|
| guards present, protoCore 2.3 | 393/393, fixture passes |
| all four guards removed, protoCore 2.3 | fixture passes, 8.3 s |
| all four removed, protoCore `9e85a4c0` (no kernel fix) | killed at 90 s, exit 124 |

**Reproduction.**

```bash
cd /home/gamarino/Documentos/proyectos/protoClojure/build_release
ctest -R 'cli/blocking-joins-park-for-gc' -V < /dev/null    # ~6 s

cd /home/gamarino/Documentos/proyectos/protoCore/build_release
ctest -R 'ConformanceSelfCheck.(ProtoThreadJoinLeavesTheRunningSetWhileBlocked|JoinInsideCriticalSectionStillJoinsAndDoesNotPark)' -V < /dev/null
```

The protoClojure fixture needs a heap ceiling, because a ceiling is what makes a
cycle necessary at all; each of its four cases asserts the value its program
computed *and* that cycles ran and reclaimed while it waited, so a killed child
cannot read as a pass.

**Fix.** protoCore `7c91345f` — `ProtoThread::join` (`core/Thread.cpp:639-682`)
now brackets itself in `ProtoContext::UnmanagedScope`. The pre-fix function was
four lines and called `osThread->join()` directly. The undocumented half of
`ProtoContext::safepoint()` — that it is the only place a young generation is
submitted — was written down in the same commit
(the `safepoint()` doc block in `headers/protoCore.h`).

The guard went in the kernel rather than into five embedders because **`join` is
protoCore's own blocking call**, so an embedder had no cue that it needed
bracketing against the kernel's own quorum.

> It is often said that no documentation had ever stated the requirement. That is
> true of `ProtoThread::join` specifically — its declaration carried **no doc
> comment at all** before the fix — and false of the general rule. Pre-fix
> `DESIGN.md:126-202` documented `UnmanagedScope` fully, including the quorum
> formula, with a "when to use it" table whose rows are `read`/`write`, `sleep`,
> `poll`, `accept` and "calling into a third-party C library that may block".
> **There was no row for a thread join.** The principle was written down; the
> instance was not, and the instance was the kernel's own API.

**What the fix refuses to do.** At `criticalSectionDepth > 0` the new join
declines to leave the running set (`core/Thread.cpp:647-663`), joins as before,
and prints one diagnostic on stderr. A critical section means the caller holds
cells reachable only from C++ locals; announcing itself parked there would let a
root scan proceed without them and the sweep free them. That trades a deadlock
for memory corruption, which is the worse trade.

**Unproven.** protoClojure's four now-redundant guards were deliberately kept.
The reason is not nesting — nesting is idempotent — but that protoClojure's build
**cannot detect the fix**: it checks `SOVERSION`, which is 3 either way, because
the fix changed no ABI. A protoClojure linked against 2.2.0 with the guards
removed would deadlock with no warning.

**Lesson.** A hang that always resolves at the test timeout looks like slowness
and is usually a deadlock; the difference is one backtrace away, and nobody took
it for three months.

---

# 4. Tests that could not fail

This is the case with the widest reach, because it is not about protoCore. It is
also the case where I have to correct the framing before starting, and the
correction is itself the first lesson.

**"Six tests that could not fail" is not a number anybody measured.** The
project's own running figure is five — `conformance/CycleDriver.h:8` says *"The
same vacuity has now been found five times in this project"* — and it is
**enumerated nowhere**. "Six" most plausibly comes from a different sentence in a
different file: `test/ConformanceSelfCheckTests.cpp:7`, *"protoScala shipped six
map fixtures that could not detect a broken key classification"* — six fixtures
for **one** defect, not six cases.

Counted strictly over 2026-09-20 to 25, there are **thirteen distinct items**: ten
real tests observed passing under a named mutation or removal, two assertions
structurally incapable of failing (no mutation needed), and one static checker
that reported clean where real defects were. So a document about tests that cannot
fail contained, as its headline, a count that nobody had checked. That is exactly
the failure mode it is about.

What follows is the ten that are tests, with the mutation each survived.

### 4.1 The conformance driver submitted the garbage it was measuring

The worst of the seven, and the one now preserved as a comment in shipped source
(`conformance/CycleDriver.cpp:48-58`). The first draft of the cycle driver met
the stop-the-world quorum by polling `ProtoContext::safepoint()` in a loop.
`safepoint()` also submits the calling context's young generation. So the harness
submitted the young chain of the very context it was measuring, on the host's
behalf, and **rule 1 could never fail**:

> the self-check's `NoSafepoint` mutant PASSED `gc.young_submitted` with
> **378,016 cells reclaimed**, because the harness had submitted them on the
> host's behalf.

Fixed by parking in `ProtoContext::UnmanagedScope` instead
(`conformance/CycleDriver.cpp:59-63`), which meets the quorum without submitting
anything. Both routes satisfy the collector; only one of them leaves the
measurement intact. The vacuous draft was never committed — the fix and the
confession landed together in `cd3c5e75`, and the structural remedy is that
`CycleDriver.h` offers **no** "did it reclaim anything" call at all: the only
reclamation verdict it exposes takes the denominator as an argument.

### 4.2 `reclaimed > 0` was true in both worlds, by a factor of 30,000

The protoScala cross-runtime GC test
(`protoScala/tests/unit/protost_interop.cpp:286`) asserted that a collection
reclaimed something. Its `makeGarbage` helper (`:71`) allocated on a long-lived
context and never called `safepoint()` — the same defect as protoST's S15, in a
test written to detect that class of defect.

It passed, and the margin is the reason this is the best example in the document:
**4–7 cells reclaimed against 205,120 created.** A ratio of 3 × 10⁻⁵, and
`> 0` is satisfied in both the conforming and the non-conforming world. With
submission restored the same run reclaims 197,572. The test now asserts
`EXPECT_GE(scalaReclaimed, 20000)` and prints the number (`6dedb259`).

This is why `reclaimed > 0` is **forbidden** in protoCore's conformance suite, and
why the API is shaped so it cannot be written: a non-conforming runtime returns a
rounding error, not zero.

### 4.3 The bulk-build survival test that still cannot fail

Two sibling tests, and only one of them is fixed.
`BulkListBuild.LargeBuildSurvivesForcedCollections` shipped **with** its
anti-vacuity guard — `EXPECT_GE(cyclesDuringBuild, 1u)`
(`test/BulkListBuildTests.cpp:175-179`) — and that guard is what went red against
the unfixed builder, reporting 0 cycles completed mid-build over 40 builds. Its
own commit says why the guard is not optional: without it "the survival check
proves nothing", because a builder the collector cannot interrupt is trivially
safe from it.

`BulkListBuild.ConcurrentBuildersSurviveForcedCollections`
(`test/BulkListBuildTests.cpp:183`) has **no such guard, passed against the
unfixed builder, and is still in that state today.** The file states the reason at
`:246-250`: *"the builder's own tests cannot discriminate: while a build is in
flight the context's young chain has not been submitted, and it keeps the same
cells reachable on its own."* The response was to cover the mechanism from a
different angle — `PendingRootKeepsASubmittedValueReachable` (`:251`) — rather
than to make the concurrent test able to fail. That is a defensible choice and it
is not a fix, and this document is not going to describe it as one.

### 4.4 Three of the global-interning tests

All in `test/GlobalInterningTests.cpp`, all committed already strengthened, and
recorded rather than quietly repaired as protoScala decision D17:

| Test | Mutation | Why it still passed |
|---|---|---|
| `SymbolsSurviveManyCollectionCycles` (`:170`) | make `createSymbol` take the caller's context, so symbols stop being perennial | the draft interned in the **long-lived** context, whose unsubmitted young chain kept the cells alive anyway |
| `ASecondSpaceInternsNothingItsPredecessorAlreadyDid` (`:268`) | give each space its own `SymbolTable` | it asserted only that `globalSymbolCount()` was unchanged — and a per-space table stops populating the global one at all, so the count is unchanged under **exactly** the mutation the test exists to catch |
| `ConcurrentInterningAcrossSpacesAgreesOnOnePointer` (`:384`) | drop the double-checked re-check inside the shard lock | caught in **1 run of 3** |

The decision entry's own sentence is the rule: *"A mutation that is only sometimes
caught is only sometimes a test."*

### 4.5 Two withdrawn park-point tests, in two different runtimes

protoST's test for bug S3 passed against a build with **both** safepoints removed,
because no protoST loop is actually allocation-free: every shape allocates about
three cells per iteration and therefore parks at `allocCell`'s poll regardless.
Withdrawn rather than kept, never committed, S3 left open
(`protoST/docs/STATUS.md:305`).

protoClojure's exact analogue: `22-futures/spin-loop-allows-gc.clj` was removed
earlier, re-examined on 2026-09-25, and **deliberately not restored** — *"its
program was re-run with the hook disabled and completed, twice, rather than
deadlocking… the `@stop` in the spin loop is a primitive call, every primitive
call builds a child context, and that context allocates"* (`c0bc512`).

Two runtimes, independently, could not write a test for a defect that requires an
allocation-free loop, because neither runtime can express one. Both said so
instead of shipping the test.

### 4.6 protoST's mailbox test, unreachable behind another bug

Not a vacuous test — a test that *could not be written*. `MailboxCursor::adopt`
released a live pin, which a collection mid-turn would then free. But a runtime
that reclaims nothing cannot expose a lost root, so while bug S15 was present the
defect was unreachable and 848 passing tests could not touch it. The test was
written the day S15 was fixed, and found S16 on its first outing
(`protoST/docs/STATUS.md:331`). **The third bug of a class was invisible because
the first bug of the class was still present.**

### 4.7 protoClojure's join fixture, whose premise expired

`tests/cli/blocking-joins-park-for-gc.sh` opens by asserting that
`ProtoThread::join` is a bare `std::thread::join`. True when written, false an
hour later when the kernel fix landed — after which the fixture **passes with all
four of protoClojure's guards removed**, in 8.3 s, because the kernel now carries
it. It can no longer fail on protoClojure's own code. The header was rewritten
rather than the assertions, and the fixture is now the family's only end-to-end
evidence that the *kernel* guard works under real GC pressure (`036761f`).

### 4.8 Six protoScala map fixtures for one defect

Sabotage: make `scalaIsIdentityKey` always answer `false`. All six fixtures in
`tests/conformance/18-maps-and-sets/` stayed green, because identity keys and
value keys are **observationally identical through the language** — `scalaHash`
falls back to the identity hash and `valuesEqual` falls back to identity for a
default `equals`. The fixtures do catch other sabotages; they cannot catch this
one, and nothing at the language level can.

The remedy was a white-box discriminator, `tests/unit/test_collections.cpp:221`,
whose comment is the whole argument: *"Answering `false` for every key … passes
every conformance fixture and fails here."*

### Two more, not tests but the same failure

An assertion can be incapable of failing without needing a mutation at all: an
old protoCore case compared `o->getHash(c)` with `o->getHash(c)` — a value with
itself. And a protoScala module test printed `Setup.ready`, which a module loaded
twice still answers `true` to; it now asserts `Setup eq Again`.

And the static checker itself reported clean where real defects were, twice over:
a greedy regex parsed `fromExternalPointer(this, nullptr))` as a finalizer named
`"nullptr)"`, hiding every null-finalizer site in one runtime; and a `<path>:*`
allowlist entry added to silence about 200 heuristic warnings also silenced the
**two real errors** in that file (`2fd42072`).

**Reproduction.** The general recipe is the only thing that generalises: break
the mechanism, run the test, and require red.

```bash
cd /home/gamarino/Documentos/proyectos/protoCore/build_release
ctest -R ConformanceSelfCheck -V < /dev/null      # 14 cases, rule 10 applied to the suite

cd /home/gamarino/Documentos/proyectos/protoCore
python3 scripts/conformance/check_static.py --self-test
# 8 positive fixtures must fire, 4 negative must stay quiet
```

`docs/EMBEDDER-CONFORMANCE.md` states this as rule 10: *a GC or concurrency test
that cannot fail is worse than no test.* The suite applies it to itself. It is
the only rule in that document aimed at the tester rather than the embedder.

> `CHANGELOG.md:200` says the static checker self-tests with "seven positive
> fixtures and four negative". Running it prints eight positive and four
> negative. The count in the changelog is wrong.

**Unproven.** `ConcurrentBuildersSurviveForcedCollections` is still uncovered
(4.3). The project's asserted figure of five instances is still unenumerated. And
seven of the twelve conformance cases have no negative control at all, which is in
"What remains unproven" below rather than here, because it is the same defect one
level up.

**Lesson.** A green test proves the assertion held, never that the assertion
could have failed; the only thing that establishes the second is breaking the
mechanism and watching the test go red.

---

# 5. A critical section protecting what was already protected

**Symptom.** Stop-the-world pauses in the tens of milliseconds, proportional to
the size of the collection being built.

**What was believed.** Exactly what the code's own comment said, pre-fix
(`core/ProtoContext.cpp:632-635`):

```
// n > 5: produce the AVL form.  Build it in a single critical
// section via repeated appendLast over an empty AVL list — every
// intermediate is held in a C++ local, no half-built tree leaks
// to the GC's view between allocations.
```

`ProtoContext::newList(n, items)` wrapped its whole O(n) AVL construction in a
`ProtoContext::CriticalSection`. While that section is open
`criticalSectionDepth > 0`, so the cooperative stop-the-world poll in `allocCell`
and `safepoint` skips parking: the collector cannot begin its pause until the
last element is in.

**What disproved it.** The counter-argument was already in the same file,
twenty-six lines above the critical section, written a week earlier for the
string builder (pre-fix `core/ProtoContext.cpp:598-610`):

> No CriticalSection around the build, deliberately. A section here would
> suppress this thread's stop-the-world parking for the whole O(N) construction
> (1.38 s at 1 MiB on the old route) while **protecting nothing that is not
> already protected**: every cell the builder allocates is on this context's
> young chain […], the collector records that chain as a root during
> stop-the-world and traces its outgoing references, and a young chain can only
> become a sweep candidate once it is handed to `dirtySegments` — which happens
> in `ProtoContext::safepoint()` and at context destruction, neither of which the
> builder calls.

The section bought reachability, and reachability was already there. Nobody had
read one function against the other.

**Measurement.** Interleaved before and after, five runs per variant, nine samples
per run, load average 6.5–7.1 throughout. Median stop-the-world pause during a
100,000-element build: **39.2 ms → 31 µs**. Ranges: before, 35.9–50.7 ms across
run medians with a single-sample max of 114 ms; after, 27–41 µs with a
single-sample max of 280 µs. The collector's own instrumented P1 total fell from
~47 ms per cycle to ~0.05 ms, and the cells it managed to mark over the same run
rose from **2,772 to about 11.6 million**: before the fix the collector was not
merely slow to start, it was barely running. Construction throughput moved by
−3 % to +1 % at 10,000 and 100,000 elements, and about +6.6 % at 1,000.

**Reproduction.** `-V` is required; the test prints the pause on every run, pass
or fail, because that is the number the change is justified by.

```bash
cd /home/gamarino/Documentos/proyectos/protoCore/build_release
ctest -R '^BulkListBuild\.LargeBuildDoesNotBlockStopTheWorld$' -V < /dev/null
```

Its bound is a quarter of one measured build, so it does not depend on the speed
or the load of the machine.

**Fix.** protoCore `ab0d9501`. Every intermediate is anchored in
`ProtoContext::pendingRoot`, which the stop-the-world root scan reads, by an RAII
guard that saves and restores the slot's previous occupant. `pendingRoot` rather
than `automaticLocals` because `newList` receives the *caller's* context, so the
anchor must not disturb the caller's frame. The loop then parks explicitly every
16 elements — and parks rather than calling `safepoint()`, because `safepoint()`
would also submit the young generation, and the elements not yet appended are
reachable only from the caller's array and that chain.

**A methodological detail worth more than the fix.** An earlier version of the
pause test measured *cycle completion* instead of the interval over which
`stwFlag` is observed raised, and **still failed after the fix** — `46 vs 24`. A
cycle also carries the concurrent mark and the sweep, which are proportional to
the heap and run with the world going. The wrong instrument made a fixed bug look
unfixed for as long as it took to notice.

**Lesson.** A critical section is a claim that something is not otherwise
reachable, and that claim has to be checked against the file it is written in
before it is checked against anything else.

---

# 6. Bulk list building allocates nineteen cells per element to leave one

**Symptom.** Not a bug report. A sizing question: how much memory does a process
embedding protoCore need?

**What was believed.** That an *n*-element list costs about *n* cells. At rest it
does: `ProtoListImplementation` is one cell holding one value and two child
pointers, so a 100,000-element list rests in 100,000 cells — 6.10 MiB.

**What disproved it.** Measuring `ProtoContext::allocatedCellsCount` across the
call instead of the structure afterwards. `newList` produces the AVL form by
repeated `appendLast`, and each `appendLast` path-copies a new node at every
level it descends and allocates further nodes for rotations on the way out. The
old spine is not mutated — that is the immutability guarantee — so every
superseded node is garbage the instant the next one is built.

**Measurement** (`docs/MemoryModel.md:223-227`):

| n | cells allocated by `newList` | per element | log₂(n) |
|---|---|---|---|
| 1,000 | 11,958 | 11.96 | 9.97 |
| 10,000 | 153,590 | 15.36 | 13.29 |
| 100,000 | **1,868,896** | **18.69** | 16.61 |

1,868,896 / 100,000 = 18.68896 exactly. The analytic `n·log₂(n)` figure for
n = 100,000 is 1,660,964, so **the measurement is 12.5 % above what `n·log₂(n)`
predicts** (the document rounds this to 13 %); the excess is the rebalancing
allocations on top of the path copy. The consequence for sizing is that the
100,000-element list drove the heap to **128 MiB** and, because the heap never
shrinks, that growth is permanent.

> An "17:1" figure has circulated. I could find no source for it in any
> repository, any commit or any scratch log. The only number the project has ever
> recorded is 18.69, i.e. 19:1; 17 is what `log₂(100000) = 16.61` rounds to, and
> that is the last column of the same table. Treat "17:1" as a misremembering of
> the analytic term, not as a superseded measurement.

**Reproduction.** This is a gap: **no test or benchmark in protoCore prints cells
allocated for a bulk *list* build.** The string analogue exists
(`StringBuildTest.AllocationIsLinearInLength`); the list analogue does not, and
the number the memory model rests on is reproducible only from an untracked
scratch probe. `ProtoContext::allocatedCellsCount` is public
(`headers/protoCore.h`), so the measurement is four lines of C++ — but note
`headers/protoCoreConformance.h:65`, which warns against using a delta of that
counter in general, because `safepoint()` zeroes it on submission and an unsigned
subtraction underflows. The delta is valid for `newList` **specifically** because
`newList` never calls `safepoint()`, which is exactly the property case 5's fix
was careful to preserve.

**What is not true about this case.** It has been said that the obvious fixes do
not apply because protoCore cells are a fixed 64 bytes with no variable-size
allocation. The fixed-64-byte property is real and load-bearing — `BigCell` is a
union over `char byteData[64]`, with `static_assert(sizeof(BigCell) <= 64)`
(`headers/proto_internal.h:2229`, `:2260`), and tagged pointers depend on the
6-bit alignment slack — **but it does not block a fix, and the repository proves
it does not.** The identical amplification existed in the string builder (at
1 MiB: 23.1 million cells to produce a 65,536-cell rope, 99.7 % of them dead
before the function returned) and was removed *within the same 64-byte model* by
building bottom-up: the replacement allocates exactly the cells the rope needs
and nothing else. The same move has not been made for `ProtoList`, and nothing
rules it out. `docs/MemoryModel.md` does not claim otherwise — it offers
embedder-side mitigations (build in a child context and destroy it, call
`safepoint()` in long loops, set a hard ceiling so the builder waits instead of
growing the heap) and leaves the builder alone.

**Lesson.** Peak allocation, not live data, is what a machine must hold, and a
measurement taken after a bulk build undersizes the process by an order of
magnitude.

---

# 7. Half-global interning

**Symptom.** In a process holding two protoCore runtimes, an attribute written in
one `ProtoSpace` and read from the other came back missing — but only for some
names. `value` worked. `Counter` did not.

**What was believed.** That interning being per-`ProtoSpace` was an internal
detail, because cross-space attribute access worked in every case anyone had
tried.

**What disproved it.** A fixture written to fail before anything was changed.
An attribute key is the **address** of an interned symbol, and interning was per
space, so two spaces disagreed about the key for the same name — *except* for
names protoCore embeds in the pointer word, which match by accident. The boundary
is `INLINE_STRING_MAX_BYTES 6` (`headers/proto_internal.h:300`), and only for
pure ASCII (`core/ProtoString.cpp:1337-1341`): a 6-byte name containing any byte
≥ 0x80 missed too.

The failure was **silent**. A missed symbol lookup returns `PROTO_NONE`
(`core/ProtoObject.cpp:798`), which is also a legitimate attribute value, so
nothing errored. In the recorded pre-fix run the program continued and died
later, elsewhere, with `"Object is not an integer type."`

**Measurement.** Two of three cases failed on purpose at commit `874d82dc`, with
the two distinct pointers printed: `0x653ced648016` against `0x653ced648216` for
`"Counter"`, and `0x141 vs 0x141` — `PROTO_NONE` against `PROTO_NONE` — for the
attribute read.

**Reproduction.**

```bash
cd /home/gamarino/Documentos/proyectos/protoCore/build_release
ctest -R 'GlobalInterning' -V < /dev/null       # 10 cases
```

Two of them are the case itself, and they are named for what they mean:
`ShortNameMatchedAcrossSpacesEvenBeforeP3` (which documents the accident, using
the 5-byte `"value"`) and `LongNameIsOnePointerAcrossSpaces` (the 7-byte
`"Counter"`).

**Fix.** protoCore `be6b554e` — one process-global `SymbolTable`, as a leaked
function-local static (`core/SymbolTable.cpp:40-43`), with
`ProtoSpace::symbolTable` kept as a borrowed pointer so no layout and no call
site moved. 64 shards, unchanged. Released as 2.2.0 (`9ff3564e`).

**What was rejected, and why.** Keeping per-space tables and adding a global one
they fall back to — *"Two tables means two possible canonical pointers for one
name, which is the bug this phase closes, restated"*
(`protoScala/docs/plans/2026-09-24-phase-p3-global-interning.md:147`). Tuple
interning deliberately stays **per space** (decision D5), an explicit deviation
from the ruling, because a tuple's key is slot addresses. Making embedded strings
per-space, so that both tiers fail consistently, appears nowhere in the record; if
it was considered, it was not written down.

**Unproven.** Global interning fixed names, not prototypes. Prototypes remain per
space, so a protoST object handed straight to another runtime still carries parent
links into protoST's chain; deleting the now-redundant key rebuild at that
boundary would trade a silent attribute miss for a silent prototype mismatch
(`protoST 2b80365`).

**Lesson.** Half-global identity is worse than either extreme, because the half
that works by accident is the half you test with.

---

# 8. `takeAll`'s retain window

**Symptom.** In protoScala, one message in a batch of 182 lost every one of its
own attributes. Always exactly one, and always the last. Downstream it surfaced
as `ClassCastException: + expects a number, got Null`.

**What was believed.** That `ProtoMPSCQueue::takeAll`'s GC safety was settled, and
settled *by a test*. It was, partly: an earlier commit had established the retain
chain's necessity with a real negative control (`ee5753b1`):

> removing the retain chain makes the mark-race test fail — one run freed 86,590
> of 200,000 items under a live `ProtoList` — so the test can fail on exactly the
> defect the design closes.

That is a good proof. It is a proof that the retain chain must **exist**. It says
nothing about how **wide** the published window is.

**What disproved it.** Reading the window's own argument against the code's
control flow. `takeAll` published its retain cell with the chain it had **loaded**
(`core/ProtoMPSCQueue.cpp:365`), then detached a possibly **longer** one
(`:382`). Every node a producer prepended in between was in the detached chain
and in no retain cell. The file's argument for those nodes — that they are young
cells of the pushing context and not candidates of the running cycle — is true
and insufficient: it covers only the cycle running when the window closed, and
the walk that follows parks for stop-the-world every 64 nodes, so the call
routinely spans a cycle **boundary**. For the next cycle those nodes are ordinary
candidates hanging off nothing but a C++ local, and the collector has no view of
C++ locals. The chain is LIFO, so the node prepended inside the window is the
last item out — hence "always the last".

**Measurement, and the two figures are different measurements.**

- **The field symptom**, from protoScala's instrumented run
  (`protoScala/docs/CONFORMANCE.md:116-120`):
  `DIAG-C … cursor=182 batch=182 lost=1 first=181 last=181`. Index 181 of a
  182-element batch: the first and only lost element is the last one.
- **The lab invariant**, from protoCore's new deterministic test: batch 101,
  reachable-from-retained 100; canary `traced=0 finalized=1`. Removing the
  widening store fails on 10 runs out of 10 with identical numbers.

Neither figure substitutes for the other, and the first attempt to prove the fix
statistically was rejected rather than published: racing a producer against a
consumer gave 4 failures in 40 runs against 0 in 40 — Fisher one-sided p ≈ 0.12,
which cannot distinguish a fix from luck.

**Reproduction.**

```bash
cd /home/gamarino/Documentos/proyectos/protoCore/build_release
ctest -R MPSCQueueWindow -V < /dev/null         # 3 cases
```

The test **enters** the window through a test-only hook
(`proto::pmqTakeAllWindowHook`, declared in the internal header only, null in
every build, one relaxed load per `takeAll`) and, with a pause armed from inside
the window, asserts both that every node the detach took is reachable from
`retained` and that a canary cell reachable only through the window node is
traced and never finalized.

**Fix.** protoCore `3a8f6608` — one store, after the detaching exchange
(`core/ProtoMPSCQueue.cpp:402`), republishing the chain that was actually
detached. It cannot narrow anything, because only prepends happen; it leaves the
publish-before-detach ordering the GC-safety proof depends on untouched; and it
is still inside the window, so it is visible before this thread can park.
Released as 2.4.0.

**Unproven, and recorded in the source as such.** `be3bc821` adds 33 comment
lines headed *"OPEN, NOT FIXED: the release gate is one cycle too eager"*. The
gate releases when the cycle counter differs from `retainedEpoch`, which proves
cycle C finished but says nothing about C+1's mark still running. The candidate
remedy — require the counter to advance by two — is written down together with
the reason not to apply it: *"Neither the hazard nor the remedy has a test; do not
change the gate without one."*

**Lesson.** A test that proves a mechanism is necessary does not bound the
mechanism's parameters, and the retain chain's existence had been proven while
its width had never been measured.

---

# 9. One conformance case, misattributed five times

**Symptom.** protoCore's conformance rule 8, `heap.ceiling_progress`, aborted in
protoPython:
`heap hard limit 249152 cells reached; live set 196471 cells, last cycle reclaimed 0`.

**What was believed.** Five successive readings of one case. The first two were
miscalibrations that named no mechanism, and are recorded in `2fd42072`: a ceiling
set at `heapSize + 32768`, which aborted at live sets of 230,977 and 280,922
cells, and an adaptor workload with an unbounded backlog, which was live by
construction. Then three that each named a mechanism and each was documented as
established fact:

1. **The kernel.** `protoPython/docs/CONFORMANCE.md` stated the failure was
   protoCore's: every exiting thread permanently losing a 4,096-cell free-cell
   batch, and that *"no protoPython-side change can make this case pass"*. That
   kernel leak was real and was fixed in 2.4.0 (case 13's sibling in the
   changelog). It was not this.
2. **protoPython's own per-thread retention.** A genuine and substantial finding
   — ~62 marked cells retained per function object, 117 per `Thread`, from
   reference cycles through mutable objects (see case 12). Three separate fixes
   landed. It was not this either.
3. **Ceiling calibration.** Which it was.

**What disproved the first two.** For the kernel: an A/B on the *identical*
binary, pre-fix and post-fix library, produced a **byte-identical** abort
message. And the reasoning had a hole visible without any measurement — the
escalation fires on `reclaimedLastCycle == 0`, not on heap growth, and the
printed `live set` is the **marked** count. A cell in a leaked batch is on no
free list and is never marked, so leaked batches cannot appear in that figure at
all.

For the retention: the thread count was set to **zero** — items produced inline,
no `threading.Thread` anywhere — with the item count, the bounded backlog and the
ceiling all unchanged. The case still aborted, at a live set of **201,595 cells
against 201,600 with its two threads per round.** Per-thread retention moved the
number by five cells.

**What it actually was.** The case settles the space, reads the live set the
runtime itself needs, and sets the hard ceiling 200,000 cells above it.
protoPython imports its stdlib lazily, so a cold host settled at **49,152
cells** — the interpreter before it has ever seen `threading` — and then the
workload's own source did `import threading` and
`from collections import deque` **inside the measured window**, taking `inUse` to
159,503. That is **110,351 cells, 55 % of the whole 200,000-cell headroom**, and
it is permanently and legitimately live: protoPython's threading stack, not
garbage.

**Fix.** It landed in **protoPython's conformance adaptor** — not in protoPython's
runtime, and not in the case. `conformance/CaseHeap.cpp` has not changed since
`2fd42072`; the threshold is still `settled + 200000`.
`PythonConformanceHost::warmRuntime()` now imports `threading`, `time` and
`collections.deque` in the constructor, so "the live set the runtime needs"
includes the runtime (`protoPython c4901169`,
`test/library/ConformanceHost.h:246-253`). Result: PASS, with about 30 cycles
completed while the workload ran. Mutation: delete the `warmRuntime()` call →
SIGABRT, `live set 196534 cells, last cycle reclaimed 0`.

> Do not quote this case's reclaimed figure as a constant. It appears three times
> with three values — 1,306,839, 1,293,491 and 1,303,077 on a re-run — and the
> settled live set likewise (166,421 against 158,520). They are run-variant.

**Reproduction.**

```bash
cd /home/gamarino/Documentos/proyectos/protoPython/build_release
ctest -R 'conformance_isolate.heap.ceiling_progress' -V < /dev/null
```

It runs in its own process, because a case whose failure mode is `std::abort()`
cannot share one.

**A second finding from the same commit, about tests and machine load.** Two other
conformance cases, `join.parks` and `stw.quorum_completes`, were bounded by an
*iteration count*. They passed at 278.08 s and 278.43 s against a 300-second
budget — a 22-second margin on an idle machine — and were observed timing out
whenever the machine carried other load. **A test that fails because another
process is busy measures the other process.** The budget was left alone and the
workload changed: the worker now allocates until `time.monotonic()` passes 20 s,
which is twice the case's own release timer and longer than every deadline either
case sets. Neither case ever cared how much arithmetic the worker got through —
both need only that a registered, allocating thread is still up while a collection
is demanded. 278.1 s → 20.7 s and 278.4 s → 20.7 s.

**Unproven, and stated by the fix itself.** A warm host's `heapSize` has already
grown past settled + 200,000, so the ceiling caps no further growth. The case
wants a `Host::prepare()` hook it calls before settling, so the calibration is
sound for any runtime with a lazily-imported stdlib instead of relying on each
adaptor to warm itself. That hook does not exist. And the case is intermittent
where it is green: in protoScala it fails about 2 runs in 10 when run alone
(`protoScala/docs/CONFORMANCE.md:99-103`).

**Lesson.** A case that reliably detects something real but cannot say what it
detected will be attributed to whatever the investigator is currently looking
at — five times, here, each time with a number that was consistent with the
hypothesis and evidence for none of it.

---

# 10. A benchmark comparison that was invalid, and published

**Symptom.** A published report concluded that protoScala's actor scheduler
regressed with more workers while protoClojure's improved — 97,516 msg/s at one
worker down to 54,122 at eight, against protoClojure rising.

**What was believed.** That this was a protoScala scheduler defect.

**What disproved it.** Measuring the rest of the curve, and then equalising the
benchmark. Three separate errors compounded:

1. **The two `fan-out` scripts measured different work.** protoScala's rotates
   its target on every send, so nearly every message is a cold wake;
   protoClojure's sends 1,000 consecutive messages to one actor, so 999 of every
   1,000 coalesce into an already-claimed actor. Same name, different workload.
2. **Two points cannot describe a curve.** The `ProtoMPSCQueue` variant was
   measured at **only w=1 and w=16**. On a machine with 6 physical cores and 12
   logical, the distinction between "does not scale" and "scales, then pays
   oversubscription" lives entirely between w=1 and w=6. But note the ordering
   here, because it is the part that matters: the two earlier reports measured
   **six** worker counts (1, 2, 4, 6, 8, 16) and carried the **identical** wrong
   conclusion. Endpoint-only measurement made the third report worse; it was not
   the root cause. The root cause was the shape mismatch, which no number of
   worker counts would have exposed.
3. **The machine was loaded, and the two runtimes were not sampled together.**
   The first run took one sample per cell and ran protoClojure's whole suite as a
   separate block afterwards, while the load average rose from 3.59 to 9.09
   across the run.

An honest asymmetry about that third point: load did **not** make the original
wrong. The suspect run's control agreed to within 2 % and its medians reproduce
exactly. The *corrective* run was the noisier one — load spiked to 18.93 and
`earlyoom` sent SIGTERM to round 4.

**Measurement, once the shapes were equalised.** Run with protoScala's rotating
shape, **protoClojure collapses further than protoScala did**: 143,900 → 97,100 →
78,400 → 64,100 msg/s at w=1, 4, 6, 16 — a fall of **−55 %**, against the −32 %
of protoScala's `ProtoMPSCQueue` variant over the same window. Its own batched
script, on the same binary minutes apart, kept its 231k → 448k → 418k rise.
Equalising the shape **reverses the ranking from w=2 up**.

Two further corrections came out of the same re-measurement. "protoScala's actors
do not scale" is too broad: on the same scheduler and the same binaries, `MPMC`
rises **+113 %** from w=1 to its w=4 peak (154,322 → 329,335 msg/s). And
`fan-out` is **producer-bound in its send path** — 78–99 % of the measurable
window is inside the sender's loop — so it was never measuring the scheduler at
all.

**How long it stood.** All of it inside one calendar day. The first report went in
at 01:47 on 2026-09-23 (`859dff5`) and the retraction banner at 14:15
(`6f7cdb5`): **12 h 28 min**. The claim also reached `README.md` at 05:44
(`646ff28`), so the *public* exposure was **8 h 31 min**. The two-point
w=1/w=16 report stood 1 h 27 min, and the corrective curve preceded the
retraction by under six minutes. "Published for a day" is an overstatement of
each of those figures.

**Reproduction.**

```bash
cd /home/gamarino/Documentos/proyectos/protoScala
python3 benchmarks/run_actor_benchmarks.py    # see the report for worker sets
nproc && lscpu | grep -i 'core\|Model name'   # 12 logical / 6 physical here
```

Read `benchmarks/reports/2026-09-23-actors-v4-curve.md` first: it sets out the
method the earlier reports lacked — worker counts 1, 2, 3, 4, 5, 6, 8, 12, 16
with no gaps below 6, one sample of every cell per invocation round-robin across
modes *and* runtimes, four pooled rounds, medians with `[min-max]`, and a
tabulated `ldd` resolution per binary.

**How it was handled.** Nothing was rewritten. Each superseded report carries a
banner at the top listing, numbered, exactly what is overturned and what still
stands, and `README.md` carries the correction too. The reports also now state a
fourth thing: **no mode in the suite can exhibit a rise up to 6 physical cores**,
because each is capped by its own structural concurrency, so no conclusion about a
6-core ceiling can be drawn from any table in them. That gap was closed
structurally rather than by retraction: CPU-bound `saturation-8` and
`saturation-32` benchmarks were added to both runtimes, and they finally produce a
rise-to-six-cores shape with both runtimes on one curve.

The correction was also kept narrow, in its own words: *"At w=1 on the identical
shape protoScala is still 14 % behind."* A retraction that overshoots into the
opposite claim is the same error with the sign flipped.

**Unproven, and it is the worst part of the case.** The command for the
**decisive** measurement — protoClojure under protoScala's rotating shape — does
not survive. No driver was kept, and of the three claimed rotating samples only
two are retained; the batched row reproduces exactly from its logs, and the
rotating row is merely arithmetically consistent with a third sample that is gone.
The single measurement that overturned the published conclusion is the least
reproducible number in the whole episode.

A smaller one found on the way: `protoClojure/benchmarks/actor-fanout.clj:1` says
"100 inc messages each = 100_000" while `:15` sets `MSGS-EACH 1000`. The header
comment is stale by a factor of ten.

**Lesson.** Two points on a curve, on a loaded machine, against a benchmark of
the same name but not the same shape, is three independent ways to be wrong, and
they all pointed at the same innocent component.

---

# 11. 1,263 of our own tests against 5 % of Scala's

**Symptom.** None. protoScala's suite was 1,263 tests, 1,262 of them green — the
single failure being the conformance case of case 9.

**What was believed.** That the suite measured conformance to Scala.

**What disproved it.** Running tests nobody here wrote: the Scala 3 compiler's
own `tests/run` corpus — **1,654 single-file programs**, dotty `a68b419c` — one
file per process against `build_release/protoscala`, scored by dotty's own rule
(match the `.check` file, or exit 0 when there is none). Two harness adaptations,
neither a change to protoScala: a driver line is appended because protoScala runs
a file rather than a class, and the corpus is triaged into three buckets, of which
**bucket 3, n = 601, is the in-scope set** — the files in which no rule finds a
construct protoScala does not claim to have. Every rule names the construct it
matched, and nothing is excluded from the run.

**Measurement — and read the two denominators before the numbers.**

| measurement | whole corpus | in-scope subset |
|---|---|---|
| what the run found | **82/1654 = 5.0 %** | **75/601 = 12.5 %** |
| after the Predef surface | — | 178/601 = **29.6 %** |
| after member imports as well | 206/1654 = 12.5 % | 183/601 = **30.4 %** |

Zero regressions: no test that passed anywhere in the 1,654-file corpus before
this work fails after it.

**257 of the corpus's disagreements with real Scala were anticipated by no
document in this repository.** That figure is exact, and it is exact in a way
worth spelling out, because it is the one number here that a reader can
recompute: of the 424 in-scope failures remaining after the Predef fix,
`failures_inscope.jsonl` classifies 164 as deviations the documents already
declared, 2 as internal errors and 1 as a timeout; the residue is 241 classified
as divergences nothing anticipated plus 16 unclassified. 241 + 16 = 257. (The
repository's own prose says 423 remaining rather than 424; the data file has 424
rows.)

The single largest of the 257 — six missing Predef names — cost 103 in-scope
tests and was invisible to a suite that had never needed them. The largest
remaining groups are syntax the parser does not accept (96), further stdlib names
(52), `C(...)` on a plain class (34), classes nested in a class (34) and the
absent `scala.*` namespace (33).

> **"29.5 % of Scala's" is wrong twice over.** It is a *post-fix* rate, and it is
> a rate over the 601-file in-scope subset, not over Scala's corpus. What the
> measurement found, before any fix, was **5.0 % of the corpus and 12.5 % of the
> part in scope**. Quoting the post-fix subset rate as the discovery understates
> the finding by a factor of six, which is an unusual direction for a number to
> drift in.
>
> Two smaller ones. The suite figure is **1,262** passing at the P4 baseline, not
> 1,261; a 1,261 state did exist briefly, and it was **self-inflicted** — a rework
> of the conformance adaptor to reuse one persistent actor turned a second case
> red, and both were closed by one fix (`6cb5389`, a per-turn context leak that
> retained ~6 cells per message; `liveCellsLastCycle` 130,769 → 22,920 at 20,000
> messages). And the corpus is dotty `a68b419c`, whose tree reports
> `developedVersion = 3.10.1` — but the *expected message texts* were validated
> against **scalac 3.9.0**, an older compiler. That mismatch is not known to have
> affected a result, and it has not been checked.

**Reproduction.** Not part of `ctest`, because it needs the corpus checked out.
The fixtures the findings produced are:

```bash
cd /home/gamarino/Documentos/proyectos/protoScala/build_release
ctest -R '27-predef|28-member-imports' -V < /dev/null
```

32 fixtures, each shown capable of failing by a named mutation of the
implementation — 11 mutations for the Predef half, 12 for the import half, every
fixture turned red by at least one.

**Fix.** protoScala `bc7eafc` and the Track X merge — `assert`, `assume`,
`require`, `???` with Scala's exact exception types and message text; `App`; and
plain Scala's member import alongside the module-loading form.

**A second finding, about documentation rather than code.**
`docs/LANGUAGE.md` §3.2 had documented `import` **only** as file-modules and
never said the ordinary member form had gone. No deviation had been recorded
claiming it was absent by design, so there was no deviation to retract: *the
omission was silent, which is worse.*

**Lesson.** A test you wrote encodes your belief, so a suite of your own tests
measures faithfulness to your own model and cannot detect a misunderstanding you
share with it.

---

# 12. A cycle among mutable objects is never collected

**Symptom.** In protoPython, every function object created at run time was
immortal: ~62 marked cells retained per function object, flat across 30 further
collection cycles, and **not** reclaimed by making the object immutable nor by
removing the metadata-cache pin.

**What was believed.** That protoCore's Phase 5b release of mutable-table entries
was complete. Its documented soundness argument is correct, and
`docs/GarbageCollector.md` states the only limitation it knows about — that with
`PROTOCORE_GC_REINCLUDE_SURVIVORS=OFF` a handle surviving one cycle is never
collected.

**What disproved it.** A breadth-first search from protoCore's actual GC roots,
which found the dropped function reachable in one hop from a
`ProtoSpace::mutableRoot` shard. Two ordinary design decisions had collided:

```
fn    --__closure_frames__-->  frame      (createUserFunction)
frame --co_name------------->  fn         (OP_BUILD_FUNCTION binds the new
                                           function's name in the defining frame)
```

Both objects mutable. The mechanism, in two lines of protoCore:

- **The table originates marking.** Phase 2 pushes every non-null shard root onto
  the worklist as a root, unconditionally — `core/ProtoSpace.cpp:520-524`, a loop
  over all 256 shards (`MUTABLE_ROOT_SHARDS`, `headers/protoCore.h`).
- **An entry is released only on handle finalization.**
  `ProtoObjectCell::finalize` appends `mutable_ref` to `gcFinalizedMutableRefs`
  (`core/ProtoObject.cpp:515-520`), consumed after sweep by
  `releaseFinalizedMutableEntries` (`core/ProtoSpace.cpp:232`).

Put together: in a cycle `A → B → A`, each handle stays marked through the other
object's shard entry, so neither is ever finalized, so neither entry is ever
released. Ordinary cycles among immutable cells are collected correctly. **The
release rule is sound; the gap is that it cannot fire for a cycle.**

**Measurement.** In a bare `ProtoSpace` with no runtime, 1,000 objects of 8
attributes each, created and dropped, cycles driven to convergence — marked cells
retained per object:

| population | retained per object |
|---|---|
| immutable | 2.73 |
| mutable, no self-reference | 2.54 |
| mutable, one attribute a `ProtoMethod` bound back to the object | **12.29** |

protoPython's instance went from ~62 marked cells per function object to ~4.6, by
stopping protoPython from *creating* the cycle — gating `__closure_frames__` on
non-empty `co_freevars` and dropping a per-instance `__call__`/`__get__` that was
a direct self-reference. Also 117 marked cells per `Thread`, from one captured
closure. The mutation that reds the fix: force the gate true, and the slope
returns to 58.5.

> **A figure from a probe that held its own roots.** The same probe at 2,000
> self-referencing mutable objects aborted inside the collector with
> `CRITICAL TAGGED POINTER (Phase4(young)): … type 23`, reproducibly and at the
> same address. It is recorded as a low-confidence secondary observation and not
> as a defect, because **the probe pins a `ProtoMethod` cell in a
> `ProtoRootSet`, which may not be a supported thing to do.** A number produced
> by a probe that holds its own roots measures the probe.

**The shortest instance, and the one that shows the retention is structural.** In
protoScala a captured `var` is boxed in a protoCore **mutable** object:
`Op::MAKE_CELL` is `L.cellProto->newChild(&frame, /*isMutable=*/true)`
(`protoScala/src/runtime/ExecutionEngine.cpp:962-963`), the value lives in its
`__value__` attribute, and `Op::MAKE_FN` stores a closure's captures as a list
attribute on the function object (`:989-1002`). So an ordinary recursive lambda:

```scala
var f: () => Unit = null
f = () => f()
```

makes the cell's value the closure and the closure's capture the cell. Value →
handle → value: a permanent cycle, in an idiom nobody would think twice about.

**And the usual remedy is unavailable here.** Everywhere else in this family the
answer to an accidental cycle is "store a snapshot instead of the object" — which
is how protoPython's function objects were fixed. For a captured `var` that
remedy is definitionally wrong: **observing later writes is the entire purpose of
boxing the variable.** A snapshot would be a different language. So for a captured
`var`, and for any genuinely cyclic object graph, the retention is **structural**
rather than an embedder's oversight — which is the difference between this case and
every other case in this document.

**Reproduction — at the time of writing there was none, and that was the state of
the case.** `test/MutableRootReclaimTests.cpp` has four cases and **not one builds
a cycle**; `conformance/CaseGC.cpp` had no mutable-cycle case;
`docs/EMBEDDER-CONFORMANCE.md` had no rule about it. The only executable
demonstration was an uncommitted scratch probe. A detector, a conformance case and
the normative write-up are being added separately; when they land, prefer them and
prefer `docs/MemoryModel.md` and `docs/GarbageCollector.md` over this paragraph.

Until then, the smallest program a reader could write: create two objects with
`newChild(ctx, /*isMutable=*/true)`, `setAttribute` each to the other, drop every
C++ and root-set reference, drive cycles to convergence with
`conformance/CycleDriver.h`, and compare marked cells against the same run with
both objects immutable. The mutable pair's cells never come back.

**The two rejected fixes — and this is the honest part.** Two directions are
routinely cited as having been considered and rejected: **ephemeron semantics**,
whose objection is that it needs a fixpoint inside a concurrent mark and whose
failure mode is a use-after-free; and **destination-dependent assignment**, whose
objection is that it would silently freeze legitimate cyclic structures, turning
retention into stale reads. Both objections are sound. **Neither appears anywhere
in any repository.** I searched every Markdown file, source file and commit body
across all six projects and the scratch directories: the words "ephemeron" and
"destination-dependent" occur nowhere outside a vendored syntax-highlighter
keyword list. If these were reasoned about, they were reasoned about verbally,
and the reasoning is unrecorded. They are written here for the first time, and
should be read as arguments, not as decisions.

Where the finding was first recorded is protoPython, not protoCore:
`protoPython/docs/CONFORMANCE.md:300`, headed *"Kernel finding: a reference cycle
through a mutable object is uncollectable"*, marked **Severity: high. Reported,
not fixed — protoCore is out of scope for this phase, and this needs the
collector's owner.** At the time this document was written, protoCore's own
committed documentation stated only the weaker, configuration-specific limitation
and never the cycle case.

> **The canonical statement belongs elsewhere.** `docs/MemoryModel.md` and
> `docs/GarbageCollector.md` are where this property is being written up
> normatively, together with a diagnostic API for finding such cycles. Read them
> for the authoritative version; this case is the evidence and the history behind
> it, not the specification.

**Lesson.** A root table that originates marking makes reachability-from-the-table
unfalsifiable from inside the graph, so any release rule keyed on finalization has
a hole exactly the shape of a cycle — and the kernel's own documentation described
the rule's soundness without noticing the hole.

---

# 13. Installers that had never worked

**Symptom.** None, for months. Nobody had installed anything.

**What was believed.** That the packaging worked, because `cpack` produced files
and CMake reported success.

**What disproved it.** Installing into a scratch prefix and trying to use the
result. Three independent defects, one per project:

### 13.1 protoPython's package never contained the interpreter

Pre-fix `protoPython/CMakeLists.txt:1027-1032`:

```cmake
install(TARGETS protopy protoPython protopyc
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT protoPython          # <-- the defect
)
```

In `install(TARGETS)`, once an artifact-kind keyword appears, every keyword after
it binds to **that group** until the next kind keyword. The trailing `COMPONENT`
therefore bound to `ARCHIVE` alone — and these targets have no `ARCHIVE`
artifact, so the one `COMPONENT` that was parsed did nothing at all. `protopy`
(RUNTIME) and `libprotoPython` (LIBRARY) fell to `Unspecified`, and with
`CPACK_COMPONENTS_ALL protoPython` every package shipped the headers, the stdlib
tree and `protopyc`, but neither the interpreter nor its shared library.
`protopyc` survived only because it has a second, well-formed install rule in
`src/compiler/CMakeLists.txt`. Fixed in `fd98031c`.

### 13.2 protoJS could not compile against an installed protoCore

**89 translation units** in protoJS wrote `#include "headers/protoCore.h"` —
protoCore's *source-tree* path. An installed prefix has no `include/headers/` at
all, so the first translation unit failed with
`fatal error: headers/protoCore.h`. The count is exact: 89 directives removed, 89
added, in 89 distinct files (of 94 occurrences in 93 files; the 5 left are
documentation, on purpose). Fixed in `eb98159d8`, which in the same commit raised
every hardcoded protoCore `>= 1.0.0` floor in `packaging/` to `[2.0.0, 3.0.0)`
with a soname check — *"against a 2.x protoCore those checks did not merely fail
to help, they certified a stale 1.0.0 as adequate."*

> This is a compile-time include-path defect, not a packaging-payload defect.
> Nothing globbed protoCore's source layout into any package: protoJS installs
> one target and its Debian script copies one file. If you have heard this
> described as "protoJS's package included protoCore's source layout in 89
> files", that framing is wrong.

### 13.3 An installed protoST could not find its standard library — on Linux too

Pre-fix `protoST/src/runtime/STRuntime.cpp:1977-1984` probed candidates in this
order:

```cpp
dir / "lib",
dir.parent_path() / "lib",                        // <-- the Linux bug
dir.parent_path().parent_path() / "lib",
dir.parent_path() / "share" / "protoST" / "lib",
```

For an installed `<prefix>/bin/protost`, the second candidate is `<prefix>/lib` —
the *library* directory. It exists, because it holds `libprotoCore.so.2`, so
`is_directory` succeeded, the search stopped, and `Import from: 'stream'` failed
with `module not found: stream` while the stdlib sat in
`<prefix>/share/protoST/lib`. Nothing platform-specific about it: the Linux
`/proc/self/exe` branch ran, found the wrong directory, and returned it. The
macOS and Windows half is separate and additive — the function read
`/proc/self/exe` directly, so on those platforms the step was skipped entirely.
Fixed in `83a4267`, which inverts the order and adds `_NSGetExecutablePath` and
`GetModuleFileNameA`.

**Reproduction.** Install into a scratch prefix inside the workspace — never a
system prefix, never `sudo` — and then check the two things CMake's success does
not tell you: that the binary is present, and that it runs.

```bash
W=/home/gamarino/Documentos/proyectos
S=$W/.agent_scratch/repro-installers
rm -rf "$S"; mkdir -p "$S/stage"

cmake -S $W/protoCore -B $W/protoCore/build_release \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$S/stage
cmake --build $W/protoCore/build_release --target protoCore
cmake --install $W/protoCore/build_release --component protoCore

for r in protoPython protoST protoClojure protoScala protoJS; do
  cmake -S $W/$r -B $W/$r/build_pkg -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=$S/stage -DPROTO_CORE_PREFIX=$S/stage \
        -DPROTOCORE_REQUIRE_PACKAGE=ON
  cmake --build $W/$r/build_pkg          # protoJS: no -j on this machine
  cmake --install $W/$r/build_pkg --component $r
done

ls $S/stage/bin                                   # 13.1: protopy must be here
env -u LD_LIBRARY_PATH $S/stage/bin/protopy -c "import json; print(json.dumps({'ok':2}))"
env -u LD_LIBRARY_PATH -u PROTOST_LIB $S/stage/bin/protost \
    $W/protoST/tests/conformance/13-stdlib/stream-write-nextput.st; echo "exit=$?"   # 13.3
```

**Unproven.** What was retained as evidence is the package metadata, the bounded
`Depends:` relations, an `ldd` table per binary, the suites, and the `/usr/local`
refusal. What was **not** retained is the output of the installed binaries
actually running: the plan required it and the verification report has no such
section. The claim that each installed binary does real work is asserted in prose
and in commit messages, not captured. macOS and Windows packaging is configured
and reviewed but marked **UNVERIFIED**, because no such host was available.

**Lesson.** A build system reports that it did what you wrote, and all three of
these defects are cases where what was written was not what was meant — so the
only test of an installer is installing and then using the result.

---

# 14. A stale library on the default loader path

This case is included **with its claims reduced**, because the version of it that
circulates is stronger than the evidence.

**What is confirmed.** A root-owned protoCore 1.0.0 sits on the default loader
path right now:

```
/usr/local/lib/libprotoCore.so.1.0.0    root:root  0644  2026-02-09
/usr/local/lib/libprotoCore.so.1     -> libprotoCore.so.1.0.0    (SONAME libprotoCore.so.1)
/usr/local/include/protoCore.h          33,310 bytes
```

There is no `/usr/local/lib/cmake`, which is exactly why the old
`find_library`/`find_path` discovery could not tell 1.0.0 from 2.x. The hazard is
real and has bitten: a `/tmp`-installed protoPython did resolve
`libprotoCore.so.1 => /usr/local/lib/libprotoCore.so.1` and died with
`undefined symbol`. The same class of defect hit protoST from a different
direction and is recorded as bug S14 — a fresh configure searched
`../protoCore/build` before `build_release` and picked a three-month-old library
over the current one.

**What is not confirmed.** That the benchmark build directory linked it, and that
the numbers published from that directory are affected. The only source for
either is one prose line in a task file
(`protoScala/tasks/todo.md:75-76`): *"Caught first: `build_bench` was linked
against the stale protoCore 1.0.0 in `/usr/local/lib` and crashed on
`--version`; rebuilt clean against 2.0.0."* I could find **no `ldd` output
anywhere** showing that directory resolving `/usr/local`, and the pre-rebuild
CMake cache was overwritten, so the original state is unrecoverable. The note's
own wording — "caught first" — places the rebuild *before* the re-measurement it
belongs to. **Do not say published numbers came from it.** The crash on
`--version` is likewise prose only; the nearest captured crash of that shape is a
different build directory against a different library.

What the reports *do* record is the opposite, and they record it because of this
scare: `2026-09-23-actors-v4-curve.md:44` and `-v5-saturation.md:68` both tabulate
the `ldd` resolution of every binary and state that **none resolves the stale
root-owned 1.0.0 in `/usr/local/lib`**. The P3 and P4 sweeps re-run that check
across the whole family and both came back clean.

**Remediation — and it is not an RPATH change.** protoCore now emits a real CMake
package configuration (`983bbf98`): `install(EXPORT)`,
`configure_package_config_file`, `write_basic_package_version_file` with
`SameMajorVersion`, an explicit `SOVERSION` assertion, and a pkg-config file.
Previously `install(TARGETS protoCore EXPORT protoCoreTargets ...)` named an
export set nothing ever wrote out, so `protoCoreConfig.cmake` did not exist and no
consumer could check a version at all. Every runtime now prefers
`find_package(protoCore <floor> CONFIG)`, with the hand-rolled discovery demoted
to a warning-emitting developer fallback that `-DPROTOCORE_REQUIRE_PACKAGE=ON`
forbids. The gate is proven against the actual stale library: configuring
protoScala against `/usr/local` alone now raises `FATAL_ERROR` where the same
command used to succeed.

**Deliberately not remediated:** the library itself. It is left in place and used
as the test fixture for the new version gate.

**The diagnostic, which is two commands and not one.**

```bash
ls -la /usr/local/lib/libprotoCore* 2>/dev/null
objdump -p /usr/local/lib/libprotoCore.so.1.0.0 | grep SONAME

# -u LD_LIBRARY_PATH is essential: a benchmark harness exports it to its
# children, which masks the real RUNPATH.
for b in protoST/build_release/protost protoClojure/build_release/protoclj \
         protoScala/build_release/protoscala protoJS/build_release/protojs; do
  printf '%-44s ' "$b"
  env -u LD_LIBRARY_PATH ldd "/home/gamarino/Documentos/proyectos/$b" 2>/dev/null \
    | grep libprotoCore || echo 'NO libprotoCore'
done

# then make each binary prove it can start, and say which protoCore it got
protost --version; protoclj --version; protoscala --version
```

The second command is not an afterthought. A shadow copy with the **same soname**
passes `ldd` cleanly and shows up only as a crash constructing the `ProtoSpace`,
which is precisely the window this defect lived in: protoCore's workspace soname
was also `.so.1` at the time. Today it is `.so.3`, so the stale library can no
longer be silently substituted — it would be "not found".

**Lesson.** A version print is part of the diagnostic, because a stale library
with a matching soname is invisible to `ldd` and visible only at the first call
into it.

---

# What remains unproven

This section is the reason the document is worth reading twice. Everything above
has a measurement; everything below does not, and is stated at the same volume.

## Seven of the twelve conformance cases have no negative control

The suite has 12 runtime cases (`conformance/Runner.cpp:20-42`) and 6 mutation
classes (`conformance/SelfHost.h`) covering 5 of them. The other seven have never
been shown capable of failing:

| Case | Rule | Ever observed red against a real runtime? |
|---|---|---|
| `gc.host_stress` | 3 | yes — protoScala, on its first run |
| `heap.ceiling_progress` | 8 | yes — protoScala and protoPython |
| `stw.quorum_completes` | 2 | no; only `NotApplicable`, never `Fail` |
| `external.finalizer_runs` | 7 | never |
| `external.bytes_accounted` | 7 | **it has no `Fail` path by design** |
| `module.root_survives_cycle` | 9b | never |
| `module.alias_rejected` | 9c | never |

`external.bytes_accounted` returns only `NeedsReview` or `Pass`
(`conformance/CaseExternal.cpp:114-134`); that is deliberate and argued in the
source, because whether an external byte count is *right* is unknowable to
protoCore. The mutation `NonConformingHost_AllThreadsWaitForHeadroom`, which the
plan's own done-when clause said `heap.ceiling_progress` "is not complete
without", **was never written** — the gate was not met and the case shipped.

Two further honest notes. Both red observations have since gone green, which
weakens them as evidence *for the case* rather than strengthening it. And the
"cases to distrust" analysis exists only in an untracked scratch file, while the
shipped commit message for the suite asserts the opposite: *"the seven cases for
which no mutation was found are listed as such"* — they are not listed anywhere in
the repository. Moving that table into `docs/EMBEDDER-CONFORMANCE.md`, and
asserting that every case id appears either in the mutation table or in a named
"no negative control" list, would settle it.

## Three runtimes run only the static half

protoST, protoClojure and protoJS have no `proto::conformance::Host`, so none of
the twelve runtime cases has run against them. Each says so in its own words, and
each says **UNVERIFIED, not passed**:

| runtime | rules UNVERIFIED | count |
|---|---|---|
| protoST | 1, 2, 2b, 3, 5, 8, 9b, 9c, 11 | 9 |
| protoClojure | 1, 3, 4, 5, 8, 9b, 9c, 11 | 8 |
| protoJS | 1, 2, 2b, 3, 4-runtime, 5, 8, 9b, 9c, 11 | 10 |

protoClojure's is 8 rather than 9 because rules 2 and 2b are covered end to end by
a CLI fixture instead. protoJS is also not clean on the static half: its ratchet
*"exits 1 on purpose: 14 uncovered errors"*. And the runtime with no adaptor at
all, protoST, is the one the whole conformance phase came from.

The two that do have an adaptor each leave **rule 11 unverified**, because
`forEachThreadKind` is deliberately unimplemented in both. protoScala substitutes
a static census and names it correctly: *"That is a reading, not a measurement,
and it is recorded as such."* So the one rule with *two* mutations has no runtime
that exercises it.

## The exception-unwinding rooting claim rests on argument

protoScala re-roots an exception payload into `frame.returnValue` as the stack
unwinds (`src/runtime/ExecutionEngine.cpp:881`, with siblings at `:891`,
`src/runtime/ActorScheduler.cpp:296` and `:369`). Two conformance fixtures
exercise the path — 400 and 200 throws through 60- and 40-frame chains, under
collector pressure, with a second thread allocating — and both are green.

The decision log says the rest plainly (`protoScala/docs/DECISIONS-LOG.md:198`):

> **Not proved by breaking the premise, and this entry says so rather than
> claiming otherwise.** […] **deleting the re-rooting line leaves them green
> too.** […] an attempt to force a collection inside it by calling `triggerGC()`
> from the C++ catch crashed with and without the re-rooting, so it proved
> nothing.

One thing to add that the entry does not: the deletion experiment is described as
run, but **no log of it survives**, and the three `ExecutionEngine.cpp` snapshots
in the scratch directory all contain the line. So even the negative result is
asserted rather than retained. What would settle it is a hook that forces a full
cycle *between* one frame's `~ProtoContext` and the next frame's catch — or the
cheaper alternative the entry names itself, a per-thread pin stack, "additive, one
attribute write per throw".

The claim is restated in `protoScala/docs/DESIGN.md:530-543` **without** the
caveat, which is how an unproven claim becomes a believed one.

## ThreadSanitizer has never been run against protoScala's actors

Confirmed three independent ways. protoScala has **no TSan build option at all** —
its `CMakeLists.txt` defines two options, neither a sanitizer, and all four
existing build trees have empty `CMAKE_CXX_FLAGS`; the only recipe is an unchecked
box in a plan. Its own documents say so: *"ThreadSanitizer was never run against
the actors"*, and *"acknowledged by the maintainer as a known gap, not approved as
done: the TSan run is still owed"*. And no TSan log in the workspace mentions
protoScala at all.

What *has* been TSan-tested is protoCore's gtest binary, across four campaigns
(the MPSC queue, global interning, the newList fix, the kernel fixes), all of it
inside protoCore, all of it ad hoc via raw flags — **protoCore has no TSan CMake
option either, and there is no CI in any of the six repositories**, so nothing
runs TSan automatically. Those runs also left a known 40–116-warning baseline
accepted as benign, and an open recommendation to make `ProtoSpace::softHeapLimit`
and `maxHeapSize` atomic has not been taken.

Never instrumented: protoScala's entire concurrency surface — `ActorScheduler`,
`Mailbox`, `ReadyStack`, `Futures`, `ActorPrimitives`, `FutureYield` — and
protoST's and protoClojure's, which have no option, no log and no statement.

The obstacle is worth recording because it is mechanical, not a matter of will:
TSan aborts on this kernel with `FATAL: ThreadSanitizer: unexpected memory
mapping`. Raising `vm.mmap_rnd_bits` needs root, so every TSan run here is wrapped
in `setarch -R`, and a second workaround is needed
(`-DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST`) because
`gtest_discover_tests` runs the binary at build time and the abort made CMake
delete it.

And the un-instrumented code already has a live bug in it: a protoScala mailbox
test aborts under `PROTOCORE_HEAP_LIMIT_CELLS=20000`, "left undiagnosed", in
exactly the code TSan has never seen.

## Smaller unproven items, each recorded where it was found

| Claim | Where | What is missing |
|---|---|---|
| A protoPython `OP_LIST_TO_TUPLE` defect leaves `tupObj` unrooted across three allocating calls, while `OP_BUILD_TUPLE` directly above does it correctly. **Severity: high, unproven** | `protoPython/docs/CONFORMANCE.md:480-490` | no failing test attached |
| A protoJS TDZ branch is dead, because `PROTO_NONE` is `321UL` and truthy, so a lexical read before initialization does not raise `ReferenceError` on that path. **Severity: medium, unproven** | `protoJS/docs/CONFORMANCE.md:141-147` | no test262 case attached |
| Rule 3's pass is explicitly not evidence: `gc.host_stress` has **not** been run under AddressSanitizer in either runtime that ran it, and both record that the one bug of that class they met was invisible without a sanitizer | `protoScala/docs/CONFORMANCE.md:184`, `protoPython/docs/CONFORMANCE.md:472` | the ASan run the checker names as the mechanism |
| protoScala installs no heap limit, so no cycle ever starts by itself — which qualifies every GC measurement in that repository | `protoScala/docs/CONFORMANCE.md:141-146` | reported, deliberately not failed |
| protoCore's stop-the-world pause figures are **estimates, not measured percentiles** | `README.md` | measured percentiles |
| protoCore's stop-the-world-elimination research is entirely estimate — *"We have not measured."* | `docs/STW_ELIMINATION_RESEARCH.md` | a prototype |
| `ProtoThread::getCurrentThread` and `ProtoSpace::getCurrentThread` are declared in `headers/protoCore.h` and **defined nowhere**; an embedder calling either gets an undefined reference | `CHANGELOG.md`, "Known, and reported rather than fixed" | reported, not fixed |
| `ProtoSpace::newThread`'s scratch context is never destroyed — a handful of cells per thread, and the reason a thread's release cannot be driven from a finalizer | `docs/GarbageCollector.md:852-860` | reported, not fixed |
| `getOwnAttributeDirect` cannot distinguish "absent" from "not an object cell" | `docs/EMBEDDER-CONFORMANCE.md` | changing a return convention is ABI-visible; documented, not closed |

The project's own standard for all of this is written down in one line, in
protoScala's interning specification: *"because a claim whose mutation stays green
proves nothing."* By that standard, seven of twelve conformance cases, the
exception-rooting line, and every protoScala actor race are outside it.

---

# Numbers this document corrects

Every figure below was circulating in this project's documents, commit messages or
conversation, and is wrong. They are collected here so that a reader who has seen
the earlier version knows which one to keep.

| Circulating | Correct | Where the wrong one is written |
|---|---|---|
| protoST: 833 tests green while 2,748,398 cells went unreclaimed | the suite was **848/848** when S15 was measured; 833 was its size at the earlier S13 fix | `conformance/CaseGC.cpp:90`, `CHANGELOG.md:218` |
| protoClojure apparent live set 246,523 | **246,519** at a 500,000-cell ceiling | nowhere in the repositories; measurement only |
| protoClojure: 2,375,888 cells reclaimed | **1,886,332** at 400,000, **1,898,028** at 500,000 | unsourced |
| protoClojure true live set 2,245 | **2,164** (the fixture's own measured figure) | unsourced |
| "110×" | **90.8×** from the fixture's recorded pair (196,519 / 2,164). No operand pair exists for 110 | `conformance/CaseGC.cpp:90`, `CHANGELOG.md:219` |
| six cycles at a 500,000-cell ceiling | six is the **400,000** result; 500,000 gives **five** | — |
| the protoClojure bug survived "two months" | **three months and eleven days** (2026-06-14 → 2026-09-25) | — |
| `newList`'s counter-argument was "twenty lines above" | **26 lines** above, measured from the end of the comment block (38 from its start) | — |
| bulk list amplification 17:1 | **18.69**, i.e. 19:1. No source for 17 exists; 17 is `log₂(100000) = 16.61` rounded | — |
| `n·log₂(n)` predicts 13 % less | the measurement is **12.5 %** above the prediction | `docs/MemoryModel.md:232` says 13 % |
| "six tests that could not fail" | the project's own figure is **five**, unenumerated (`conformance/CycleDriver.h:8`); counted strictly there are **13** items, 10 of them tests. "Six" is the count of protoScala **map fixtures for one defect** | `test/ConformanceSelfCheckTests.cpp:7` is the six-fixture sentence |
| the `newList` **test** could not fail | the shipped `LargeBuildSurvivesForcedCollections` **could** and did go red; the one that cannot is `ConcurrentBuildersSurviveForcedCollections` (`test/BulkListBuildTests.cpp:183`), still uncovered | — |
| protoST's **S15** test could not fail | S15's tests are two-directional and sound; the withdrawn one is the **S3** test, never committed, no file:line | — |
| protoClojure had three unbracketed joins | **four**; the prediction missed `pmap` | superseded prediction file |
| "no documentation had ever stated the requirement" (join) | true of `ProtoThread::join`, which had no doc comment; **false** of the general blocking-call rule, documented pre-fix at `DESIGN.md:126-202` with a table that had no row for a join | — |
| `join` **refuses** at `criticalSectionDepth > 0` | it refuses to **leave the running set**; it still joins, and warns once | — |
| protoClojure's joins reclaimed 2.9M–5.9M cells over 8–13 cycles | a re-run gives **2.81M–5.94M over 7–13**; treat as run-variant | `628d034` |
| rule 8 was misattributed **three** times | **five** invalid readings, three of them naming a mechanism | — |
| rule 8's fix was to the case | to **protoPython's conformance adaptor**; the case is unchanged and its `prepare()` hook is still un-actioned | — |
| rule 8 reclaimed 1,293,491 cells | run-variant: 1,306,839 / 1,293,491 / 1,303,077 | — |
| the fan-out error was "only w=1 and w=16" | true of the third report only; the two earlier ones measured six worker counts and carried the **same** wrong conclusion | — |
| protoScala's suite was 1263 with 1261 passing | **1262** at the P4 baseline; a 1,261 state existed briefly and was self-inflicted | superseded report |
| Scala 3 corpus: 29.5 % | **5.0 %** of the corpus, **12.5 %** in-scope, as found; 29.6 % / 30.4 % are post-fix subset rates | — |
| protoJS: 248 key-shaped `fromUTF8String` sites | **~596** | superseded plan |
| protoJS: protoCore's *source layout* in 89 package files | 89 **`#include` directives** in 89 source files; nothing was globbed into a package | — |
| the static checker self-tests with seven positive fixtures | **eight** positive, four negative | `CHANGELOG.md:200` |
| rule 8's workload imported 55 % of the heap — of what? | 110,351 cells against the **200,000-cell headroom** the case adds above the settled live set: 55.2 %. Against the 249,152 total ceiling it is 44.3 %, which is the wrong denominator | — |
| the unfixed kernel RED'd `join.parks` at a 110-second timeout | recorded in a commit body, **no raw log**, and it is not a checked-in bound — the ctest timeout has always been 240 s. It describes a superseded draft of the case; today's case returns `Fail` in about 10 s | `5090d3c1` body |
| protoScala's `gc.host_stress` live set 316,230 | **unconfirmed**; it appears only in prose, in two places that are one source cited twice. The recorded mutation figures (325,030 / 313,358 / 307,125) have no surviving log either | `protoScala/docs/CONFORMANCE.md:18` |
| the benchmark comparison stood "for a day" | **12 h 28 min** for the original conclusion; 1 h 27 min for the two-point measurement | — |
| `build_bench` linked the stale `/usr/local` library and produced published numbers | the stale library is real; **the linkage and the tainted numbers are unestablished** and should not be claimed | `protoScala/tasks/todo.md:75-76` |
| protoCore version 2.0.0, 412 CTest cases | **2.4.0**, **488** cases (`ctest -N`, 2026-09-25) | `README.md`, Project Status |

Two of these are wrong *inside protoCore's own shipped source and changelog*
(the 833 and the 110×), which is the general shape of the problem: a figure
quoted once becomes a figure quoted forever, and neither of those two has an
operand pair behind it.

---

# How to re-verify everything here

```bash
cd /home/gamarino/Documentos/proyectos/protoCore/build_release
ctest -N < /dev/null                      # 488 cases
ctest < /dev/null                         # the whole suite
python3 ../scripts/conformance/check_static.py --self-test
```

For a specific case, the reproduction command is in that case. Three of them need
a heap ceiling and say which value; two of them need `-V` to print the number
they measured; all of them need stdin at EOF.

Where a number here rests on a commit body rather than on a retained log, the
case says so. Where a claim rests on argument rather than on a measurement, it is
in "What remains unproven" rather than in a case. That division is the only thing
in this document worth copying.
