# Object attributes as a list of 64-byte cells (exploration, 2026-10-28)

Branch `explore/attr-list` (not merged, not tagged, not released), based on
protoCore master 5a8cd7c7 (3.3.1). Commits:
- 1efee14f: the prototype;
- 3280380c: GC-pressure test;
- d27cb71c: head-cell overwrite;
- 0d015c80: getOwnAttributes shares the AVL.

The public headers (`protoCore.h`) are byte-identical to master's. Every
runtime was rebuilt from clean against the branch with no source change.

## Verdict in one paragraph

The list form does what it was designed to do for objects that are built
once and read or extended at the head. Compared with master:

- Writing a new attribute costs 2 cells instead of 3-9.
- Live memory per object is n/2+1 cells instead of n+1.
- A cold read of a recent attribute is up to 8x cheaper.
- A grouped 5-field construction takes 6 cells instead of 8; five separate
  writes take 12 instead of 24.

Those wins are real in protoCore but small or invisible in the runtimes. On
whole programs the branch moves from about −15 % to +9 %, and most results
are within ±3 %. The reason is that the runtimes' costs are dominated by other
things, as the phase 1 breakdown already showed: allocation, page faults and
the mutable table.

The design also has structural losers that grow with n:
- overwriting old attributes: round-robin field updates are 1.3x at n=8 and
  1.74x at n=16;
- misses that walk the whole list;
- the promotion step at exactly n = T, which costs 2-7x on that write;
- `getOwnAttributes`, which must now build an AVL for list-form objects: 9-50x
  slower on n ≤ 8, and n cells per call.

**Recommendation:** keep the branch as an exploration. Do not merge as is. If
a variant goes forward, use T = 8 (not 16 or 32), and first fix the
`getOwnAttributes` conversion cost and the promotion-at-threshold cliff
(details in "Recommendation").

## 1. Phase 1: where the ~350 ns of a write goes (master 3.3.1)

`bench/decomp.cpp`. Mutator thread only; n = 100 attributes; 1 M operations
per repetition; perf_event_open counters. Default heap unless stated.

| Row | ns | instr | cycles | cells |
|---|---|---|---|---|
| W1 mutable setAttribute (the floor's pattern) | 374-381 | 1397 | 503-513 | 7.86 |
| W2 immutable setAttribute | 311 | 1128 | 405 | 6.86 |
| C1 AVL replace (path copy + rebalance), n=100 | 289 | 960 | 340 | 5.86 |
| C1b AVL insert 100→101 | 568 | 2292 | 725 | 11 |
| C2 new ProtoObjectCell (the state/version cell) | 45 | 157 | 52 | 1 |
| C3 mutable-table shard setAt, M = 1 / 4 / 64 / 400 live mutables | 58 / 59 / 158 / 226 | 329 / 288 / 666 / 947 | 86 / 82 / 220 / 321 | 1 / 1 / 3 / 4.5 |
| C4 attribute-cache invalidation | 3 | 44 | 10 | 0 |
| C5 bare cell allocation (default heap / adaptive) | 165 / 47 | 181 | 59-64 | 1 |
| C6 critical section | 4.6 | 30 | 15 | 0 |
| C7 loop + fromInteger | 2.2 | 20 | 7 | 0 |

The components add up to W1. Resolve, CAS and refresh are negligible when
there is no contention. On the default heap, perf stat of W1 shows the
following:
- kernel cycles 4.29 G against user cycles 2.6 G;
- 614 k page faults per 5 M writes;
- system time 1.43 s against user time 0.73 s.

So about 62 % of the mutator's wall time is the kernel supplying fresh pages
for new cells. The default heap never collects in such a loop.

Within user time, the symbol split is:

| Category | Share | Symbols |
|---|---|---|
| Allocation | about 46 % | implAllocCell 22.4 %, allocCell 13 %, addCell2Context 7.3 %, Cell ctor 2.3 % |
| AVL logic | about 28 % | replaceValue 23.6 %, implSetAt 4.2 % |
| setAttribute body | 7.8 % | |

The cost is cells. A cheaper attribute structure can only cut the attribute
tree's share, 5.86 of 7.86 cells at n=100. The mutable-table path copy (1 to
4.5 cells, growing with the number of live mutables) and the version cell
are untouched by this exploration.

## 2. The prototype

### Design (headers/proto_internal.h, core/ProtoAttributeList.cpp)

**Storage.** `ProtoObjectCell::attributes` is now `AttributeStorage`: one
word, which is either the AVL pointer (`ProtoSparseListImplementation`) or the
list pointer with bit 0 set. Both are 64-byte-aligned cells. No field was
added: ProtoLazyObjectCell is still exactly 64 B.

**The new type.** `ProtoAttributeListImplementation` has its own
`CellType::AttributeList`, its own GC tracing (`processReferences`: next,
value0, value1), MutableCycles labelling and a typeName.
`ProtoSparseListSmallImplementation` was not reused. Each cell holds:

| Field | Meaning |
|---|---|
| `next` | the next cell |
| `count` | pairs in this cell and all later cells |
| `key0`, `value0` | the first pair |
| `key1`, `value1` | the second pair; `value1 == nullptr` marks a one-pair cell (`key1 = 0`) |

The cell is 64 B, enforced by `static_assert`. Only the head may hold a
single pair; every other cell is full.

**Lookup.** A sequential walk compares the interned key pointers. A null value
means absent, as in the AVL.

**New attribute.** It goes at the head. When the head holds one pair, the head
is copied with the new pair added ("pack", the default). Otherwise a new
one-pair head is created. Either way it costs 1 cell. Packing halves the live
cells; `PROTOCORE_ATTR_LIST_PACK=0` disables it for the measurements.

**Overwrite.** No shadowing:
1. copy the cells in front of the old pair;
2. drop the old pair;
3. put the new pair at the head;
4. share the tail.

An overwrite inside the head cell builds its single new cell directly
(d27cb71c).

**Removal.** Copies the prefix and shares the tail.

**Form rules.**
- The mutator chooses the form from the resulting size.
- Growth past `T` promotes to the AVL.
- A removal that leaves ≤ T/2 attributes demotes to the list. The hysteresis
  prevents an object oscillating around T from rebuilding on every write.
- Every other mutation keeps the form it finds; reads never change it.
- `T = 0` disables the list entirely.

**Grouped writes.** `setAttributes` (setSorted) handles both forms, including
group promotion and demotion.

**Empty objects.** They still start with master's empty AVL cell, so
`newObject` allocates the same number of cells as on master. The empty tree
has no form. Saving that cell (1 cell per `newObject`) is a possible
follow-up; it was left out to keep the A/B unconfounded and the GC tests'
cell accounting unchanged.

**Configuration.**
- `T` comes from `PROTOCORE_ATTR_LIST_MAX`, default 8, clamped to 0..64. The
  internal `setAttributeListMax()` is used by the tests.
- Enumeration order is unspecified and no guarantee was added. The list
  visits newest first. The AVL visits in key (address) order.

### Tests

| Suite | Result |
|---|---|
| protoCore ctest on the branch | **779/779** pass at T = 0, 2, 8, 32, 64 (master has 770; 9 new) |
| Final head 0d015c80 | 779/779 at T=8 |
| ASan + LSan (`detect_leaks=1`), full suite, T=8 | **779/779** |
| ASan, attribute, object and mutable suites, T=2 and T=64 | 119/119 each |
| AddressSanitizer or LeakSanitizer errors | none in any log |

The nine new tests are in `test/AttributeListTests.cpp`:
- form follows size on growth;
- demotion at T/2;
- overwrite at every position (head changes, tail shared, cells ≤ depth+2);
- a same-value write keeps the storage;
- removal at every position;
- prototype chain across forms;
- random operations against a `std::map` model for T ∈ {0,1,2,3,4,8,13,32,64}, mutable and immutable, with setAttribute, removeAttribute and setAttributes;
- 4 ProtoThreads × 2000 concurrent writes on a mutable list, plus a `setAttributeIfEqual` CAS counter;
- values survive at least 10 GC cycles under a tight heap limit.

`SetAttributesTests` also checks the list invariants.

## 3. Microbenchmarks (A/B against master, same build)

### Method

Both libraries are built RelWithDebInfo `-O3 -g -DNDEBUG`. The benchmark is
`bench/attrbench.cpp`. It uses only the public API, so one binary runs
against either library through `LD_LIBRARY_PATH`.

Each run loads a fresh copy of the .so (ldd recorded) in its own process.
Each n runs in a separate process, so the mutable table starts clean.
Configurations:
- `master`;
- `br0` (branch, list disabled);
- `br4`, `br8`, `br12`, `br16`, `br24`, `br32`;
- `br8np`, `br16np` (no head packing).

There are 3 rounds, with the order reversed on the middle round. Every
measured repetition runs in a fresh child context. Every loop's checksum is
asserted: 300/300 runs printed `RESULT OK`.

Heap: the adaptive controller with a 60 M-cell hard limit. The default heap
never collects in these loops, and one process reached 18 GB RSS.

Metrics: instructions (deterministic) and cycles of the calling thread only;
the median over rounds of the median of 5 repetitions. Cells per operation
are counted one operation at a time.

Positions follow insertion order: "new" is the last key written (the list
head), "old" is the first (the list tail). Keys k0..k(n−1) are interned
strings whose addresses also ascend, so they sit in a balanced AVL on master.

The A/A control is `bench/sweep2.sh`: master, masterB (a second copy of
master), br0, br8 and br16, for n ∈ {2,4,8,16,100}, over 5 rounds.
- **masterB/master is 1.00 ± 0.04** on every group, for instructions and
  cycles.
- **br0** (branch code, list off) costs **+1-3 % instructions** everywhere:
  the tagged-word dispatch. Its cycles are +4-6 % on immutable writes and
  **+8-22 % on mutable-write rows**.
- That mutable-write cycle gap does **not** show at whole-process level:
  `perf stat` of a full `attrbench 100` run gives master/br0 within 1 % on
  cycles, L1 misses, dTLB misses and page faults over 3 alternating pairs.
  It stays an unexplained row-level effect and is reported as such.

Full tables for every configuration: `bench/tabulate.py data/sweep in|cy|cells <ops>`.
A condensed set is in the Appendix.

### Summary by access pattern (ratio branch/master; < 1 means the branch is better)

| Pattern | n ≤ T (list form) | Notes |
|---|---|---|
| Read, attribute cache hot | 0.99 | The per-thread attribute cache in front of the storage hides the form. |
| Read, cache cold, newest attribute | cycles 0.43 (n=2), 0.26 (n=4), **0.13** (n=8), 0.12 (n=16) | One cell versus an AVL path. |
| Read, cache cold, oldest attribute | cycles 0.62-0.74 (n ≤ 4), **1.61** (n=8), 1.20-1.39 (n=16-32 at T ≥ n) | A walk of n/2 dependent cells. |
| Miss (attribute found in the parent), cache cold | cycles 0.3-0.5 (n ≤ 4), 0.84 (n=8), **1.3-2.6** (n=12-32) | A full walk before the parent. |
| Immutable new attribute | instructions 0.30-0.40, **2 cells** against 3-9 | **At n = T it is 2.1x (T=8), 3.4x (T=16), 7.3x (T=32)**: the promotion builds the whole AVL. |
| Immutable overwrite, head | instructions 0.44-0.63 (n ≥ 4), 1.09 (n=1) | |
| Immutable overwrite, tail | instructions **1.14 (n=2), 1.17 (n=4), 1.36-1.41 (n=8), 1.80 (n=16), 2.86 (n=32)** | Cells n/2+1 against log n. |
| Mutable round-robin overwrite (the floor's pattern) | instructions 0.96 (n=2), **1.12 (n=4), 1.30 (n=8), 1.74 (n=16), 2.73 (n=32)** | Each write hits the tail after a cycle. |
| Mutable hot field (same key) | instructions **0.71 (n=8), 0.63 (n=16)** | |
| Mutable new attribute (20,000 live mutables) | instructions 0.74-0.82, cells 7.45 against 11.5-14.5 | The table copy dominates. |
| Group geomean over cold reads, misses and all writes, n ≤ 8 | instructions 0.92 (T=8), 0.87 (T=16) / cycles 0.84 (T=8), 0.79 (T=16) | Weights every pattern equally; real programs do not. |
| Same geomean, n = 12..32 | instructions 1.03 (T=8), 1.01 (T=16) / cycles 1.02 (T=8), 0.96 (T=16) | |

`getOwnAttributes` (public API, `bench/enumbench.cpp`):

| n | master | branch |
|---|---|---|
| any | **49 instructions, 0 cells** | |
| 1 | | 444 instr, 1 cell |
| 2 | | 735 instr, 2 cells |
| 4 | | 1337 instr, 4 cells |
| 8 | | 2566 instr, 8 cells |
| AVL form | | 64 instr |

The result type is a `ProtoSparseList`, which is public API. A list-form
object must therefore be converted into a new tree on every call. Every
runtime enumeration (`Object.keys`, `for-in`, `JSON.stringify`, `vars()`,
`__dict__`, structured clone) pays this. The fix in 0d015c80 removed the
critical section from the AVL path, which had been 102 instructions.

Head packing (np = off) slightly worsens cold reads (more cells to walk) and
otherwise changes little. Packing is kept.

### Memory: heap cells per live object (`bench/livecells.cpp`)

| n | 1 | 2 | 4 | 8 | 12 | 16 | 24 | 32 | 64 | 100 |
|---|---|---|---|---|---|---|---|---|---|---|
| master | 2 | 3 | 5 | 9 | 13 | 17 | 25 | 33 | 65 | 101 |
| T=8 | 2 | 2 | 3 | 5 | 13 | 17 | 25 | 33 | 65 | 101 |
| T=16 | 2 | 2 | 3 | 5 | 7 | 9 | 25 | 33 | 65 | 101 |
| T=32 | 2 | 2 | 3 | 5 | 7 | 9 | 13 | 17 | 65 | 101 |

Counts are the object cell plus storage cells; values are SmallIntegers. They
are identical after 3n random overwrites. This is an indicative figure, not a
headline.

Cells to build an object with n successive writes:

| n | master | T=8 | T=16 | T=32 |
|---|---|---|---|---|
| 8 | 44 | 19 | 19 | 19 |
| 16 | 106 | 84 (promoted at 9) | 35 | 35 |
| 32 | 226 | 200 | 157 | 67 |

### Crossover and best threshold

The crossover depends on the access pattern, not on n alone:
- Head-biased use (construction, hot fields, recent reads) wins up to n = 32.
- Tail-biased use (round-robin updates, oldest-field overwrites, misses) loses
  from **n ≈ 2-4 for writes** and **n ≈ 8-12 for cold reads and misses**.
- Equal weighting of all measured patterns crosses at about **n ≈ 10-12**.

**Best threshold: T = 8.** T=16 has a slightly better equal-weight geomean,
but its losses on tail and round-robin patterns reach 1.7-1.8x at n=16 and the
promotion cliff costs 3.4x at n=16. T=8 keeps every loss at or under about
1.4x, except the one-time promotion (2.1x on the write that crosses). T ≥ 24
is clearly worse. T=4 is too timid: it loses n=5-8, where most of the
construction win lies.

## 4. Runtimes (rebuilt from clean; no source changes)

Worktrees are detached at:

| Runtime | Commit |
|---|---|
| protoJS | c97525242 |
| protoPython | 5c923968 |
| protoClojure | 758bef4 |
| protoScala | 61cb2410 |
| protoIO | 7920332 (sibling of protoClojure and protoScala) |

Each was configured with `-DCMAKE_PREFIX_PATH=prefix-{base,branch}
-DPROTOCORE_REQUIRE_PACKAGE=ON`. ldd shows every binary links its own
prefix's `libprotoCore.so.4`, never /usr's (`data/runtime/ldd.txt`).

The A/B driver is `runtime/abperf.py`:
- A/B/A arms (base, branch, base);
- a fresh .so copy per run, ldd-asserted;
- `perf stat -r 5`, or `-r 3` for the standard suites;
- every arm's output must agree (CHECK lines, protoScala `EXPECT`, or output
  with timings masked).

All results below are OK on that check.

### ctest

| Runtime | base | branch T=8 | branch T=2 | branch T=64 |
|---|---|---|---|---|
| protoJS | 201/201 | 201/201 (again after the clean rebuild) | 201/201 | 201/201 |
| protoPython | 235/235 | 235/235 | 235/235 | 235/235 |
| protoClojure | 885/885 | 885/885 | 885/885 | 885/885 |
| protoScala | 2522/2522 | **2521/2522** | – | – |

**protoScala `FieldGroups.AGroupedConstructionAllocatesFewerCells` fails on the
branch at T=8 and T=16. It passes at T=0 and T=4.** It is not a correctness
failure: the test asserts that grouped construction of a five-field case class
uses at least 5 cells fewer than five separate writes. The branch makes both
paths much cheaper:

| Construction | master | branch |
|---|---|---|
| Per field | 44.1-44.9 | 24.9-26.1 |
| Grouped | 22.9-24.1 | 20.0-21.0 |
| Margin | | 4.9 cells |

The margin was calibrated on the AVL cost model.

In protoCore alone (`bench/groupcells.cpp`), a grouped 5-field construction is
optimal on the branch: 6 cells (empty object 2 + object 1 + 3 list cells),
against 8 on master. Five `setAttribute` calls take 12 against 24.

I did not change the test: the instructions allow test changes only for
order-dependent tests. The protoScala maintainers should restate its margin
relative to the storage, or accept T ≤ 4. **No order-dependent test failed in
any runtime at T = 2, 8 or 64**, so there is no "order-dependent test fixed"
entry.

### protoJS per-op harness (c97525242; `runtime/jsops`, (400k − 200k) / 200k per operation)

| op | base instr | branch instr | Δ instr | Δ cycles | A/A cycles |
|---|---|---|---|---|---|
| fieldwrite | 3226 | 3155 | −2.2 % | −2.1 % | −0.4 % |
| globalwrite | 2218 | 2075 | **−6.4 %** | −10.7 % | +1.8 % |
| globalcall | 4153 | 4001 | −3.6 % | −5.8 % | −3.5 % |
| push | 10450 | 10119 | −3.2 % | +5.3 % | +2.6 % |
| keyread | 1822 | 1796 | −1.4 % | −4.5 % | +4.9 % |
| fieldread, fieldreadloc, idxread, keyreadloc, globalread, localcall, closurecall, method, concat, loop, loopacc | | | −0.2 % to 0.0 % | ±10 % (A/A up to 12 %) | |

### Write and read probes (whole program; from the 2026-10-18 write-path report, plus 8-field and construction variants)

| probe | Δ instructions | Δ cycles | A/A cycles |
|---|---|---|---|
| py_base / py_write / **py_write8** / **py_new** | −0.3 / −5.3 / **−11.8** / −5.0 % | −0.7 / −1.5 / **−13.4** / **−12.3** % | ≤ 5.4 % |
| js_base / js_write / **js_write8** / **js_new** | −0.1 / −1.9 / **−7.6** / **−11.5** % | −0.1 / −0.8 / −6.7 / **−14.5** % | ≤ 2.2 % |
| clj_base / clj_write | −0.1 / −0.1 % | +0.2 / +1.9 % | ≤ 0.4 % |

protoClojure maps are `ProtoSparseList`s, not object attributes, so its write
probe does not exercise the change.

### Standard suites (whole program, Δ branch vs base; A/A in brackets)

**protoJS `tests/benchmarks/standard`:**

| Benchmark | Δ instructions | Δ cycles | A/A cycles |
|---|---|---|---|
| json_transform_tiny | −9.4 % | −11.7 % | |
| json_transform_small | −6.1 % | −3.6 % | |
| json_transform | −5.5 % | −5.2 % | |
| tree_traversal | −5.6 % | −5.3 % | |
| function_calls | −4.3 % | −9.3 % | −4.9 % |
| array_literal | −3.4 % | −3.8 % | |
| string_processing | −3.0 % | | |
| string_repeated_doubling | −3.0 % | | |
| list_snapshot_history | −1.6 % | | |
| object_property | +0.4 % | +2.1 % | |
| object_write_only | +0.6 % | +1.2 % | |
| numeric_loop, control_flow, object_read_only, string_concat | within ±1.1 % | | |
| string_concat_large_chunks | −7.8 % | | −7.0 % (noisy) |
| string_insert_middle | **+94.4 %** | **+42.1 %** | |

The `string_insert_middle` figure is an artefact; see Surprises.

**protoPython:** these are flat within ±2 % instructions:
- int_sum_loop, list_append_loop, str_concat_loop, range_iterate;
- call_recursion, fib, nqueens, sieve, binary_trees (−1.8 %).

The others:

| Benchmark | Δ instructions | Δ cycles | Note |
|---|---|---|---|
| attr_lookup | −1.3 % | +2.9 % | |
| richards_lite | +0.1 % | | First run +16 % cycles. A 10-repetition recheck gave −1.5 % (A/A +0.7 %): noise. |
| **memory_pressure** | **+8.8 %** | **+5.9 %** | Confirmed with `-r 10`; see Surprises. |

**protoClojure** (fib, tak, sum-loop, reduce-list, sum-squares, factorial-100):
±0.1 % instructions, cycles within A/A.

**protoScala comparable suite** (`perf stat -r 5`, EXPECT verified):

| file | base instr | Δ instr | Δ cycles | A/A cycles |
|---|---|---|---|---|
| **object_tree** | 1727 M | **−2.5 %** | **−4.4 %** | −3.4 % |
| **case_copy** | 4185 M | **−2.3 %** | +1.2 % | +1.1 % |
| map_build | 1042 M | +0.1 % | +2.0 % | +0.3 % |
| list_ops | 1687 M | +1.0 % | +2.2 % | −0.0 % |
| attr_lookup | 202 M | −1.7 % | +1.3 % | −0.5 % |
| tak / fib30 / sum_loop / factorial_100 | | −0.4 / −0.2 / −0.0 / −0.9 % | +2.2 / −2.1 / +0.5 / −3.6 % | ≤ 8.5 % |
| int_sum_loop / fib / str_concat / range_iterate | | −0.3 / −0.2 / −0.7 / −0.3 % | within A/A | |

`case_copy` makes 500,000 `copy` updates of a four-field case class, which is
exactly the target. It gains only 2.3 % in instructions: the attribute storage
is a small part of protoScala's per-copy work. That matches the protoScala
test above, where construction runs at 20-25 cells per object, of which at
most about 6 are attribute storage.

### protoScala vs protoClojure vs babashka (shared workloads)

`runtime/bbcmp.py`:
- 5 interleaved rounds, arm order reversed on alternate rounds;
- whole process including start-up;
- instructions and cycles from perf (user);
- wall time from perf `duration_time`, measured inside the build lock.

bb is babashka 1.x, a GraalVM native image.

**tak**: result 7 in every arm.

| arm | instructions | cycles | wall ms median (min-max) |
|---|---|---|---|
| protoScala base | 105 M | 55 M | 33.5 (32.7-34.3) |
| protoScala branch | 104 M | 53 M | 34.3 (32.8-35.2) |
| protoClojure base | 153 M | 67 M | 34.4 (33.2-35.8) |
| protoClojure branch | 153 M | 68 M | 34.8 (33.9-37.2) |
| bb | 115 M | 50 M | 22.5 (19.8-128.6) |

**fib30**: result 832040.

| arm | instructions | cycles | wall ms |
|---|---|---|---|
| protoScala base | 3157 M | 1254 M | 400.0 (384.5-445.2) |
| protoScala branch | 3151 M | 1246 M | 391.7 (385.1-409.5) |
| protoClojure base | 3708 M | 1328 M | 412.4 (408.9-438.6) |
| protoClojure branch | 3703 M | 1316 M | 415.2 (405.8-420.7) |
| bb | 3771 M | 1456 M | 478.3 (441.5-501.5) |

**sum_loop**: result 500000500000.

| arm | instructions | cycles | wall ms |
|---|---|---|---|
| protoScala base | 564 M | 193 M | 76.8 (72.2-77.9) |
| protoScala branch | 563 M | 194 M | 77.0 (73.3-83.3) |
| protoClojure base | 689 M | 179 M | 70.2 (68.9-75.0) |
| protoClojure branch | 689 M | 184 M | 72.1 (70.1-74.4) |
| bb | 1516 M | 483 M | 170.9 (169.2-178.3) |

**factorial_100**: 158-digit result.

| arm | instructions | cycles | wall ms |
|---|---|---|---|
| protoScala base | 31 M | 24 M | 26.5 (25.5-29.4) |
| protoScala branch | 31 M | 24 M | 26.2 (25.0-27.8) |
| protoClojure base | 28 M | 21 M | 22.0 (20.5-22.1) |
| protoClojure branch | 28 M | 21 M | 21.3 (20.7-35.1) |
| bb, same file | – | – | fails: `long overflow` (Clojure's `*` does not promote) |
| bb, twin with `*'` (reference only, not the same source) | 9.9 M | 8.0 M | 8.8 (8.6-20.1) |

The protoClojure and protoScala columns show no branch effect on these
workloads (all within ±1 % instructions). They are integer and call loops,
not attribute work.

**protoClojure `bench.sh` against bb** (wall-clock, best of 3; two runs per
side, alternated):

| workload | base protoclj / bb ms | branch protoclj / bb ms |
|---|---|---|
| fib | 420 / 473, 411 / 448 | 405 / 459, 409 / 442 |
| tak | 38 / 28, 39 / 26 | 39 / 26, 38 / 26 |
| sum-loop | 75 / 175, 75 / 174 | 76 / 172, 75 / 173 |
| reduce-list | 29 / 17, 28 / 17 | 28 / 16, 28 / 17 |
| sum-squares | 28 / 16, 28 / 15 | 26 / 15, 26 / 16 |

No change outside noise. protoClojure beats bb on fib (about 0.9x) and
sum-loop (0.43x). It trails on the short workloads (tak, reduce-list,
sum-squares: 1.4-1.9x), where bb's start-up advantage dominates.

### protoJS structure workloads (`benchmarks/structures/run.py`, N = 1, wall-clock medians)

These are noisy (±20-30 %); the raw logs are in `data/runtime/struct2-*`.

| Workload | Result |
|---|---|
| records, join, doctree, wordfreq | No consistent difference. |
| graph seq | Branch −2.7 % instructions, cycles equal (3 pairs). |
| graph par | Branch **+8 % instructions and about +25 % cycles** (medians of 8-9 runs each; the ranges overlap: base 19.9-27.2 G and branch 22.7-25.5 G instructions). |

The `graph par` cause was not isolated. No attribute-list or conversion symbol
shows up in its profile; the extra work is spread over the collector and
mutator threads.

## 5. Ordering effects

Enumeration order is unspecified and the branch adds no guarantee. Two facts
explain why no runtime observable changed:

- `getOwnAttributes` and `getAttributes` return a `ProtoSparseList`, so a list
  form is converted into an AVL. Callers therefore still see address order.
  Only `processOwnAttributes` walks the list directly (newest first). Its two
  runtime callers are protoJS `dap/DapSession.cpp:1764`, which sorts, and
  protoPython `PythonEnvironment.cpp:28782`, a foreign-module copy that does
  not depend on order.
- Probes on both builds print identical output: protoJS `Object.keys`,
  `for-in`, `JSON.stringify`; protoPython `vars()`, class `__dict__`, `dict`.

**Latent runtime bugs** (present on master, not caused by the branch).

**protoJS.** Named string keys come out in protoCore order, which is
interned-pointer address order, not insertion order. Examples:
- `({alpha,beta,gamma})` plus `.delta` gives `["beta","alpha","gamma","delta"]`;
- `p.zz9, p.aa1, p.mm5` gives `["aa1","mm5","zz9"]`.

The affected paths:
- `collectOwnKeys`, `Object.keys/values/entries/getOwnPropertyNames`;
- `Reflect.ownKeys`, `for-in`;
- `JSON.stringify`, the JSON.parse reviver order;
- `Object.assign`, spread, `Object.defineProperties` getter and trap order;
- Proxy `freeze`/`seal` trap order, `Iterator.zipKeyed`;
- `console.log`, structured clone, HTTP header emission order.

This contradicts ECMAScript insertion order, and `runtime/PropertyDictionary.h`
documents it as a deviation. It silently depends on protoCore's address order
and would change under any storage that enumerates differently.

**protoPython.** It keeps `__keys__` for dict and vars order, but these paths
walk protoCore attributes directly:
- class `__dict__` (mappingproxy keys, values, items);
- `frame.f_locals`;
- `__dict__` snapshots of built-in subclasses and static/class methods;
- native-module `items/keys/values/copy` without `__keys__`;
- `module.update`, `IMPORT_STAR` from native modules;
- `object.__dir__` (unsorted);
- `_complete`;
- `initDictStorage` back-fill;
- tie-breaking in "did you mean";
- `registerNativeMethodNames` ("first registration wins" when one function has two names).

Even on master, `vars(c)` after `self.alpha, self.beta, self.gamma` prints
`['beta','alpha','gamma','delta']`.

## 6. Surprises

1. **The cost of a write is mostly the kernel.** On the default heap, 62 % of
   the mutator's write time is page faults on fresh cells (phase 1). A smaller
   attribute structure cannot touch that.
2. **`string_insert_middle` (+94 % instructions) is not caused by the
   branch.** The program runs in two modes that depend on address layout:
   - Distributions over 12 alternating runs:
     - base: 37.3 M ×9 and 112.9 M ×3;
     - branch: 34.9 M ×5 and 91.3 M ×7;
     - branch at T=0: 37.5 M ×10, then 53.6 M and 113 M once each.
   - **With ASLR disabled (`setarch -R`) every run is in the fast mode on both
     sides:** base 37.3 M, branch 34.9 M (−6.4 %).
   - No object crosses the threshold in it. In the fast mode the branch is
     better, and in the slow mode it is also better (91 against 113 M). The
     A/B "+94 %" compared a slow branch run with a fast base run.
   - No threshold or rule change fixes the bimodality, because it exists on
     master. The slow mode's profile is dominated by `SymbolTable::lookupByContent`
     and string node construction. Something in protoJS or protoCore depends
     on pointer values there.
   - The branch's slow-mode count (7 of 12 against 3 of 12) is not
     statistically established (p ≈ 0.2).
3. **protoPython `memory_pressure` (+8.8 %) is a layout lottery in the
   attribute cache, not list cost.**
   - On the same branch binary at T=8, the default `n` gives 8.17 G
     instructions; passing `100000` explicitly gives 7.45 G.
   - T=12 gives 8.13 G either way, while T=0, 4 and 16 give 7.44-7.55 G.
   - An instrumented build (scratch only, not committed) counted the same list
     activity in the slow and fast cases: about 5.1-5.6 M list lookups, about
     4.0 M misses of about 2 cells, and 99-118 promotions.
   - The extra instructions are all in the attribute-lookup slow path
     (`ProtoSparseListImplementation::implGetAt`, `getFirstParent`,
     `isInstanceOf`, `isActuallyAClass`). That means more misses in the
     address-indexed per-thread attribute cache, after allocation sizes
     changed the heap layout.
   - It is real for this script as written, and it is a property of the cache
     indexing, not of the list.
4. **A crash on both sides.** protoJS `structures records par` with
   `PROTOCORE_HEAP_LIMIT_CELLS=40000000` crashes 2 of 10 runs on master too
   (SIGSEGV). On the branch the rate is the same (2 of 10, SIGSEGV or SIGABRT
   with "CRITICAL TAGGED POINTER (Phase4(mark)) … from parent … type 23": a
   corrupted `ParentLink`). It is not a branch regression; it needs its own
   investigation.
5. **Promotion produces better-shaped trees.** An object promoted from the
   list is built by `buildSorted` in one burst: perfectly balanced and
   contiguous. That is why some n > T cold reads on the branch are faster than
   master's incrementally grown AVL (for example read_cold_old at n=24-100,
   0.63-0.88 at T=8).
6. **The A/B methodology needed child contexts.** The root context's young
   chain is traced, so cells allocated in the root context in a benchmark
   never die. The first sweep held a 30 M-cell live set and was killed by
   earlyoom at 18 GB.

## 7. Recommendation and risks

**Do not merge as is.** The idea is sound for protoCore's own cost model:
- fewer cells per construction and per live object;
- much cheaper head writes, and cold reads of recent attributes.

The measured whole-program effect on today's runtimes, however, ranges from
about −15 % (construction-heavy JS and Python probes, JSON transforms) to
+9 % (one Python script, by a cache-layout effect). Most results are within
±3 %, and protoScala's case-class workloads gain only 2-4 %. The runtimes'
costs sit elsewhere: the mutable table, allocation and page faults.

If it goes forward:

1. Use **T = 8**, not 16 or 32. Larger T values multiply the tail-overwrite,
   miss and promotion losses for little additional gain.
2. **Fix `getOwnAttributes`.** Either add an internal enumeration path that
   the runtimes' hot enumerations can use without materialising an AVL (a
   public-API addition, outside this exploration's mandate), or cache
   nothing and accept n cells per enumeration. As is, enumeration of small
   objects is 9-50x more expensive.
3. **Soften the promotion cliff at n = T.** For example, promote to an AVL
   built from the list once and reuse it for repeated derivations, or raise
   T only for objects that keep growing. Today every derivation of an object
   sitting at exactly T attributes pays a full rebuild (2-7x).
4. **Round-robin updates are the worst case and are common:** mutable objects
   whose fields are all updated in turn, as in game loops, simulations and
   records. A move-to-front-free variant (overwrite in place in the copied
   prefix, without moving the pair to the head) would make the repeated
   pattern cheaper, but would break the "newest first" invariant the head
   insert relies on. That needs its own measurement.

**Risks:**
- Layout and cache lotteries make whole-program A/B results swing by ±10 %
  (memory_pressure, string_insert_middle). A single A/B run can show a
  "regression" or "win" that is neither.
- Embedders that cell-count protoCore (protoScala's FieldGroups test) see
  changed figures.
- The runtimes' silent dependence on address order (protoJS key order,
  several protoPython paths) is unaffected today only because
  `getOwnAttributes` converts back to the AVL. A future cheaper enumeration
  that walks the list would make those latent bugs visible immediately.
- The +1-3 % instruction overhead of the tagged dispatch is paid even with
  the list disabled.

## Files

| Path | Contents |
|---|---|
| `bench/` | attrbench (microbenchmark), decomp (phase 1), floor, livecells, enumbench, groupcells, perfcount.h, sweep.sh, sweep2.sh (A/A), tabulate.py, summary.py |
| `runtime/` | abperf.py (A/B/A driver), bbcmp.py (protoScala/protoClojure/bb), the write probes, the protoJS per-op scripts, the orchestration scripts |
| `data/sweep`, `data/sweep-aa` | raw microbenchmark outputs |
| `data/decomp-*.txt` | phase 1 |
| `data/livecells-*.txt` | cells per live object |
| `data/runtime/` | every runtime result (txt and jsonl), ldd checks, string_insert_middle distribution, structure logs |

## Appendix: condensed microbenchmark tables

Master is shown in absolute units (cycles or instructions per operation, or
cells). The other columns are ratios to master, except cells, which are
absolute. Median of 3 rounds.

Read, attribute cache cold, newest attribute (list head): cycles per read (master), ratio branch/master

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 53 | 1.04 | 1.15 | 1.02 | 0.96 | 0.98 |
| 2 | 116 | 1.10 | 0.43 | 0.50 | 0.47 | 0.44 |
| 4 | 207 | 1.09 | 0.27 | 0.26 | 0.25 | 0.25 |
| 8 | 423 | 1.01 | 1.06 | 0.13 | 0.14 | 0.17 |
| 12 | 458 | 0.94 | 0.98 | 0.79 | 0.14 | 0.12 |
| 16 | 496 | 1.03 | 1.08 | 1.01 | 0.12 | 0.13 |
| 24 | 547 | 0.97 | 0.90 | 0.76 | 1.06 | 0.13 |
| 32 | 660 | 0.96 | 0.89 | 0.93 | 0.88 | 0.09 |
| 64 | 951 | 1.00 | 0.99 | 1.00 | 0.99 | 0.90 |
| 100 | 1194 | 0.99 | 1.00 | 0.84 | 0.94 | 0.84 |

Read, cache cold, oldest attribute (list tail): cycles, ratio

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 54 | 1.00 | 1.00 | 0.98 | 0.91 | 0.96 |
| 2 | 76 | 1.07 | 0.71 | 0.74 | 0.71 | 0.68 |
| 4 | 151 | 1.05 | 0.62 | 0.64 | 0.64 | 0.66 |
| 8 | 311 | 0.94 | 1.05 | 1.61 | 1.61 | 1.71 |
| 12 | 601 | 0.82 | 1.13 | 0.79 | 1.26 | 1.25 |
| 16 | 752 | 0.97 | 0.86 | 0.61 | 1.20 | 1.22 |
| 24 | 1142 | 0.93 | 0.80 | 0.63 | 0.63 | 1.04 |
| 32 | 1113 | 1.00 | 0.84 | 0.68 | 0.67 | 1.39 |
| 64 | 1278 | 1.04 | 0.83 | 0.71 | 0.85 | 0.83 |
| 100 | 1551 | 1.02 | 1.08 | 0.77 | 0.88 | 0.74 |

Miss on own attributes, found in the parent, cache cold: cycles, ratio

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 88 | 0.94 | 0.88 | 0.85 | 0.82 | 0.76 |
| 2 | 232 | 1.10 | 0.34 | 0.37 | 0.31 | 0.29 |
| 4 | 355 | 1.05 | 0.39 | 0.48 | 0.46 | 0.41 |
| 8 | 522 | 1.02 | 1.01 | 0.84 | 0.82 | 0.84 |
| 12 | 487 | 0.99 | 1.21 | 1.01 | 1.45 | 1.43 |
| 16 | 621 | 1.07 | 0.99 | 1.19 | 1.34 | 1.34 |
| 24 | 708 | 1.03 | 1.21 | 0.82 | 1.31 | 1.89 |
| 32 | 607 | 0.99 | 1.52 | 1.13 | 1.25 | 2.58 |
| 64 | 1050 | 1.05 | 0.94 | 1.09 | 1.20 | 0.97 |
| 100 | 1388 | 0.99 | 1.10 | 1.21 | 1.07 | 0.84 |

Read, cache hot (any position): instructions, ratio

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 105 | 0.99 | 0.99 | 0.99 | 0.99 | 0.99 |
| 2 | 105 | 0.99 | 0.99 | 0.99 | 0.99 | 0.99 |
| 4 | 105 | 0.99 | 0.99 | 0.99 | 0.99 | 0.99 |
| 8 | 105 | 0.99 | 0.99 | 0.99 | 0.99 | 0.99 |
| 12 | 105 | 0.99 | 0.99 | 0.99 | 0.99 | 0.99 |
| 16 | 105 | 0.99 | 0.99 | 0.99 | 0.99 | 0.99 |
| 24 | 105 | 0.99 | 0.99 | 0.99 | 0.99 | 0.99 |
| 32 | 105 | 0.99 | 0.99 | 0.99 | 0.99 | 0.99 |
| 64 | 105 | 0.99 | 0.99 | 0.99 | 0.99 | 0.99 |
| 100 | 105 | 0.99 | 0.99 | 0.99 | 0.99 | 0.99 |

Immutable write, new attribute: instructions, ratio

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 569 | 1.05 | 0.76 | 0.76 | 0.76 | 0.76 |
| 2 | 1055 | 1.02 | 0.40 | 0.40 | 0.40 | 0.40 |
| 4 | 1288 | 1.02 | 1.44 | 0.34 | 0.34 | 0.34 |
| 8 | 1510 | 1.02 | 1.05 | 2.08 | 0.30 | 0.30 |
| 12 | 1506 | 1.02 | 1.05 | 1.16 | 0.31 | 0.31 |
| 16 | 1763 | 1.01 | 1.04 | 1.01 | 3.37 | 0.28 |
| 24 | 1295 | 1.02 | 1.01 | 1.02 | 1.20 | 0.41 |
| 32 | 1501 | 1.02 | 1.02 | 1.02 | 1.03 | 7.25 |
| 64 | 2245 | 1.01 | 0.91 | 1.01 | 1.01 | 1.01 |
| 100 | 2478 | 1.01 | 1.01 | 1.01 | 1.01 | 1.01 |

Immutable write, new attribute: cells per op (absolute)

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 3.00 | 3.00 | 2.00 | 2.00 | 2.00 | 2.00 |
| 2 | 6.00 | 6.00 | 2.00 | 2.00 | 2.00 | 2.00 |
| 4 | 7.00 | 7.00 | 6.00 | 2.00 | 2.00 | 2.00 |
| 8 | 8.00 | 8.00 | 8.00 | 10.00 | 2.00 | 2.00 |
| 12 | 8.00 | 8.00 | 8.00 | 9.00 | 2.00 | 2.00 |
| 16 | 9.00 | 9.00 | 9.00 | 9.00 | 18.00 | 2.00 |
| 24 | 6.00 | 6.00 | 6.00 | 6.00 | 7.00 | 2.00 |
| 32 | 7.00 | 7.00 | 7.00 | 7.00 | 7.00 | 34.00 |
| 64 | 11.00 | 11.00 | 10.00 | 11.00 | 11.00 | 11.00 |
| 100 | 12.00 | 12.00 | 12.00 | 12.00 | 12.00 | 12.00 |

Immutable overwrite, newest attribute (head): instructions, ratio

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 380 | 1.07 | 1.09 | 1.09 | 1.09 | 1.09 |
| 2 | 528 | 1.05 | 0.82 | 0.82 | 0.82 | 0.82 |
| 4 | 696 | 1.04 | 0.61 | 0.63 | 0.63 | 0.63 |
| 8 | 845 | 1.03 | 1.03 | 0.52 | 0.52 | 0.52 |
| 12 | 844 | 1.03 | 1.03 | 1.20 | 0.52 | 0.52 |
| 16 | 977 | 1.03 | 1.03 | 1.03 | 0.44 | 0.45 |
| 24 | 1002 | 1.03 | 1.02 | 1.03 | 1.16 | 0.43 |
| 32 | 1138 | 1.02 | 1.02 | 1.04 | 1.03 | 0.38 |
| 64 | 1302 | 1.02 | 1.02 | 1.02 | 1.02 | 0.99 |
| 100 | 1393 | 1.02 | 1.02 | 1.02 | 1.02 | 1.02 |

Immutable overwrite, oldest attribute (tail): instructions, ratio

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 380 | 1.07 | 1.10 | 1.10 | 1.10 | 1.10 |
| 2 | 388 | 1.07 | 1.14 | 1.14 | 1.14 | 1.14 |
| 4 | 530 | 1.05 | 1.21 | 1.17 | 1.17 | 1.17 |
| 8 | 677 | 1.04 | 1.04 | 1.41 | 1.36 | 1.36 |
| 12 | 852 | 1.03 | 1.00 | 1.03 | 1.47 | 1.47 |
| 16 | 856 | 1.03 | 1.03 | 1.03 | 1.83 | 1.80 |
| 24 | 1007 | 1.03 | 1.03 | 1.03 | 1.03 | 2.17 |
| 32 | 972 | 1.03 | 1.03 | 1.06 | 1.06 | 2.86 |
| 64 | 1307 | 1.02 | 0.94 | 1.02 | 1.01 | 0.88 |
| 100 | 1159 | 1.02 | 1.16 | 1.01 | 1.16 | 1.01 |

Immutable overwrite, tail: cells per op (absolute)

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 2.00 | 2.00 | 2.00 | 2.00 | 2.00 | 2.00 |
| 2 | 2.00 | 2.00 | 2.00 | 2.00 | 2.00 | 2.00 |
| 4 | 3.00 | 3.00 | 3.00 | 3.00 | 3.00 | 3.00 |
| 8 | 4.00 | 4.00 | 4.00 | 5.00 | 5.00 | 5.00 |
| 12 | 5.00 | 5.00 | 5.00 | 5.00 | 7.00 | 7.00 |
| 16 | 5.00 | 5.00 | 5.00 | 5.00 | 9.00 | 9.00 |
| 24 | 6.00 | 6.00 | 6.00 | 6.00 | 6.00 | 13.00 |
| 32 | 6.00 | 6.00 | 6.00 | 6.00 | 6.00 | 17.00 |
| 64 | 7.00 | 7.00 | 7.00 | 7.00 | 7.00 | 7.00 |
| 100 | 7.00 | 7.00 | 8.00 | 7.00 | 8.00 | 7.00 |

Mutable object, keys overwritten round-robin (the floor's pattern; always the list tail): instructions, ratio

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 640 | 1.04 | 1.04 | 1.04 | 1.04 | 1.04 |
| 2 | 714 | 1.03 | 0.96 | 0.96 | 0.96 | 0.96 |
| 4 | 788 | 1.03 | 1.12 | 1.12 | 1.12 | 1.12 |
| 8 | 907 | 1.03 | 1.05 | 1.30 | 1.33 | 1.33 |
| 12 | 979 | 1.02 | 1.02 | 1.04 | 1.51 | 1.51 |
| 16 | 1021 | 1.02 | 1.03 | 1.04 | 1.74 | 1.78 |
| 24 | 1104 | 1.02 | 1.02 | 0.99 | 1.00 | 2.15 |
| 32 | 1157 | 1.02 | 1.02 | 1.03 | 1.03 | 2.73 |
| 64 | 1453 | 1.02 | 1.02 | 1.02 | 1.02 | 0.89 |
| 100 | 1350 | 1.02 | 1.02 | 1.02 | 1.02 | 1.03 |

Mutable round-robin overwrite: cells per op (absolute)

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 3.00 | 3.00 | 3.00 | 3.00 | 3.00 | 3.00 |
| 2 | 3.50 | 3.50 | 3.00 | 3.00 | 3.00 | 3.00 |
| 4 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 |
| 8 | 4.62 | 4.62 | 4.74 | 5.99 | 5.99 | 5.99 |
| 12 | 5.07 | 5.07 | 5.07 | 5.16 | 7.98 | 7.98 |
| 16 | 5.36 | 5.36 | 5.42 | 5.55 | 9.97 | 9.97 |
| 24 | 5.89 | 5.89 | 5.89 | 5.93 | 5.93 | 13.93 |
| 32 | 6.25 | 6.25 | 6.34 | 6.31 | 6.40 | 17.87 |
| 64 | 7.18 | 7.18 | 7.21 | 7.22 | 7.19 | 7.19 |
| 100 | 7.71 | 7.71 | 7.71 | 7.74 | 7.71 | 7.82 |

Mutable object, the same key overwritten (hot field, list head): instructions, ratio

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 655 | 1.04 | 1.03 | 1.03 | 1.03 | 1.03 |
| 2 | 635 | 1.04 | 1.09 | 1.09 | 1.09 | 1.09 |
| 4 | 809 | 1.03 | 0.84 | 0.86 | 0.86 | 0.86 |
| 8 | 957 | 1.03 | 1.03 | 0.71 | 0.71 | 0.71 |
| 12 | 1079 | 1.02 | 1.05 | 1.02 | 0.65 | 0.65 |
| 16 | 1079 | 1.02 | 1.02 | 1.02 | 0.63 | 0.65 |
| 24 | 1226 | 1.02 | 1.02 | 1.02 | 1.02 | 0.56 |
| 32 | 1270 | 1.02 | 1.02 | 0.98 | 0.99 | 0.69 |
| 64 | 1563 | 1.02 | 1.02 | 1.02 | 1.02 | 0.93 |
| 100 | 1374 | 1.02 | 1.12 | 1.02 | 1.12 | 1.02 |

Mutable write of a new attribute (20,000 live mutables; table copy dominates): instructions, ratio

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 3005 | 1.01 | 0.96 | 0.96 | 0.96 | 0.96 |
| 2 | 3475 | 1.01 | 0.82 | 0.82 | 0.82 | 0.82 |
| 4 | 3704 | 1.01 | 1.15 | 0.78 | 0.78 | 0.78 |
| 8 | 3933 | 1.01 | 1.02 | 1.41 | 0.74 | 0.74 |
| 12 | 3933 | 1.01 | 1.02 | 1.06 | 0.74 | 0.74 |
| 16 | 4162 | 1.01 | 1.02 | 1.01 | 2.00 | 0.70 |
| 24 | 3710 | 1.01 | 1.00 | 1.03 | 1.07 | 0.80 |
| 32 | 3921 | 1.01 | 1.03 | 1.01 | 1.01 | 3.39 |
| 64 | 4810 | 1.00 | 0.96 | 1.00 | 1.00 | 0.98 |
| 100 | 4850 | 1.00 | 1.00 | 1.00 | 1.00 | 1.04 |

Mutable write of a new attribute: cells per op (absolute)

| n | master | br0 | br4 | br8 | br16 | br32 |
|---|---|---|---|---|---|---|
| 1 | 8.45 | 8.45 | 7.45 | 7.45 | 7.45 | 7.45 |
| 2 | 11.45 | 11.45 | 7.45 | 7.45 | 7.45 | 7.45 |
| 4 | 12.45 | 12.45 | 11.45 | 7.45 | 7.45 | 7.45 |
| 8 | 13.45 | 13.45 | 13.45 | 15.45 | 7.45 | 7.45 |
| 12 | 13.45 | 13.45 | 13.45 | 14.45 | 7.45 | 7.45 |
| 16 | 14.45 | 14.45 | 14.45 | 14.45 | 23.45 | 7.45 |
| 24 | 11.45 | 11.45 | 11.45 | 11.45 | 12.45 | 7.45 |
| 32 | 12.45 | 12.45 | 12.45 | 12.45 | 12.45 | 39.45 |
| 64 | 16.45 | 16.45 | 15.45 | 16.45 | 16.45 | 16.45 |
| 100 | 17.45 | 17.45 | 17.45 | 17.45 | 17.45 | 17.45 |
