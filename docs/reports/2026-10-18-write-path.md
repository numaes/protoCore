# The attribute path, the write path and the price of dynamism (2.14.7)

Date: 2026-10-18/19.  Author: Gustavo Marino, with Claude.

Questions: where does protoCore's attribute access stand against
implementations with compile-time offsets, against the JVM's persistent
structures, and in real programs; what does an immutable update cost, and
why; and what changes in 2.14.7 do to the five runtimes' own comparisons.

**Everything here is synthetic and notebook-class**: DEV12 (AMD Ryzen 5
5500U, 6 cores / 12 threads, two DDR4 channels, ZFS root, a desktop with an
editor and a browser).  Power profile as in the 2026-10-12 reports (TLP in
battery mode, governor `powersave`, EPP `balance_power`): interleaved
comparisons are valid; absolute times should not be compared with reports
measured in TLP's AC mode.  GCC 15.2, Release; Java 25.0.4.1; Node 22.17.0;
CPython 3.14.0 and 3.14t; Babashka 1.4.192; Scala 3.9.0; Clojure 1.12.0 and
Scala 2.13.15 library jars from Maven Central (sha1-checked).

## Verdict

- **The attribute cache answers repeated reads; the AVL is a small part of
  real programs.**  A cache hit costs about 5 ns (a native field read 0.8
  ns).  In 25 workloads of five runtimes, 70-99.9 % of protoPython's and
  74-87 % of protoJS's `getAttribute` calls avoid every AVL probe.  The
  whole attribute path is 10-25 % of process time in the heavy workloads
  (protoST about 2 %), of which the AVL walk is 1-6 %; the rest is the body
  of `getAttribute` (chain steps, negative lookups: 60-67 % of protoJS's
  calls end not found, mutable snapshot resolution on 80-100 % of steps).
  A shapes/hidden-class redesign aimed at the AVL would have a 1-6 %
  ceiling; it was not pursued.
- **Against its peers, protoCore is within a small factor; the large factor
  is dynamism itself** (section 2, summary table).  Reads: 1.3-3.1x the
  best JVM persistent map (Clojure or Scala), faster than Clojure at 16
  attributes over 4,096 objects.  Copy-updates: 1.6-2.8x on one hot object
  (adaptive heap), 1.2-2.1x over many objects (no recycling).  Going from
  a record with compile-time offsets to a map with dynamic keys costs
  4.6-32x on reads and 1.9-5.3x on writes on the JVM too; immutability
  with a fixed shape costs nothing on reads and 5-30x on writes.
- **The hot copy-update looked 5-9x behind Clojure; the cause was heap
  policy, not the data structure.**  One copy allocates exactly 3 cells.
  With the default configuration nothing is collected in that pattern:
  every cell is fresh memory the kernel zeroes (471 k page faults, 70 % of
  the time in the kernel, the heap growing to 30 M cells).  Under the
  adaptive heap controller the same copy takes 44-47 ns instead of 130 ns.
- **2.14.7, two changes on the write path.**  A new value for an existing
  key copies the root-to-key path without recomputing sizes and heights
  and without rebalancing (copy-update -18 to -29 % over 4 k-64 k
  objects).  The per-cell spinlock on the young chain is gone; the
  collector reads the chain head only inside a stop-the-world after its
  quorum (hot copy -9 % user cycles; ThreadSanitizer clean).
- **A measurement artefact to remember: the same library bytes can run
  13 % slower from one file instance.**  The first cross-runtime session
  read protoPython as 7-12 % slower on 2.14.7 with identical
  instructions.  The staged 2.14.7 library was byte-identical to fresh
  copies of itself that ran normally; only that file instance was slow,
  run after run (section 6).  The session was repeated with both sides on
  the same runtime binaries and a fresh library copy per run.
- **Writing an attribute inside a runtime costs 950-2,900 cycles, against
  about 800 for protoCore's own mutable write** (section 7).  What the
  runtimes add is mostly systemic: a `ProtoContext` per call is expensive
  (heap slot array, a `ReturnReference` cell per return, two atomic
  operations and a spinlock to submit the young chain), and every mutable
  write invalidates the attribute cache for the whole object.  Plus
  runtime-specific dead work (protoPython walks the class MRO on every
  `STORE_ATTR` and discards the result).

- **2.14.7 is neutral for the five runtimes' suites** (section 5): the
  geomean of each suite's ratio to its competitor moves -2.1 % to +1.3 %;
  the few rows outside their A/A spread go both ways, except protopyc
  `memory_pressure` (-11.7 %, informational).  Write probes: protoPython
  -5 %, protoST -9 % (A/A 5 %), the rest within noise.
- **The adaptive heap would now be a sound default for protoJS** (section
  5.3): on the five structures workloads it is -10 % to +4 % in time and
  uses 4-7.5x less resident memory (2.2-2.5 GB -> 0.15-0.5 GB).  The
  2026-10-04 measurement that kept it off (13-36 % slower) is no longer
  reproduced.  Not changed here; a decision for protoJS.

## 1. The attribute path

### 1.1 Micro-benchmark

`performance/attribute_lookup_benchmark` (new): random access over N
objects, best of 5, checksum verified; native C++ reads a field of a
variable-length array at a known offset.  ns per read (2.14.6):

| Objects read | protoCore, 4 attrs | protoCore, 16 attrs | native 4 / 16 | inherited, depth 2 |
|---|---|---|---|---|
| 256 | 9.8 | 9.9 | 0.8 / 0.8 | 13.6 |
| 1,024 | 9.9 | 19.2 | 1.2 / 1.4 | 22.9 |
| 4,096 | 17.7 | 31.0 | 1.5 / 2.3 | 31.8 |
| 16,384 | 52.6 | 119.9 | 1.9 / 3.4 | 120.6 |
| 65,536 | 82.1 | 178.4 | 6.5 / 10.6 | 217.2 |

Inherited reads with a hot cache cost about 2 ns per chain level (the
cache records one fact per step, not per chain).  About 4 ns of the 9.8 ns
hot read is the benchmark's own unboxing (`asLong` across the shared
library).  Beyond the 1,024-entry cache the cost is the AVL walk: one
64-byte cell per attribute, dependent loads into DRAM.

### 1.2 Real workloads: cache statistics

`PROTO_CACHE_STATS` counted cache probes per chain step only, which cannot
tell how many lookups avoid the AVL (at depth 4: 80 % of steps hit, 3 % of
lookups avoid every probe).  It now also reports per call: steps,
calls without an AVL probe, depth found, mutable steps, and
`getOwnAttributeDirect` hits.  Each runtime's installed binary ran against
the statistics build (`LD_LIBRARY_PATH`, checked with `ldd`).  Calls
without any AVL probe:

| Runtime (workloads) | Calls without AVL | Not found | Mutable steps |
|---|---|---|---|
| protoPython (richards, binary_trees, nqueens, fannkuch, fib, mandelbrot) | 70.4-99.98 % | 0-11.7 % | 87.5-100 % |
| protoJS (structures: records, join, doctree, wordfreq, graph) | 73.6-87.0 % | 59.6-67.3 % | 80.7-93.5 % |
| protoST (example programs) | 34.3-72.0 % | 53-66 % | ~100 % |
| protoScala (object_tree, map_build, list_ops) | 35.7-99.99 % | 0-35.7 % | 50-100 % |
| protoClojure (tak, reduce-list, fib) | 50-100 % | 0 % | 50-100 % |

Collisions whose evicted entry had the same name (a newer snapshot of the
same mutable): `py:fannkuch` 99.98 % (every write publishes a new snapshot
and the next read misses), protoJS 8-9 % (the rest are capacity misses).

### 1.3 Share of process time (`perf`, default build)

| Workloads | Attribute path | of which the AVL |
|---|---|---|
| protoJS structures | 17-25 % | 3-6 % |
| protoScala object_tree | 25 % | 6.4 % |
| protoPython | 10-15 % | 1-2.4 % |
| protoST programs | ~2 % | < 1 % |

### 1.4 The cache index (not changed)

All six sites index the 1,024-entry direct-mapped cache with
`((obj >> 6) ^ (name >> 4)) % 1024`.  Interned names are 64-byte-aligned
cells whose tag bits 4-5 are constant, so two names on one object collide
when their addresses agree modulo 16 KB, and the pattern is deterministic
(`clj:tak` 50.00 % hits, `scala:attr_lookup` 33.33 %).  `>> 6` was
committed in `982db143` and lost in the revert `84974040` of the
attribute-cache rework chain.  Measured in isolated copies, outputs
identical in 25 workloads:

| Index | clj:tak | scala:attr_lookup | st:attr_lookup | realistic workloads | time |
|---|---|---|---|---|---|
| current | 50 % | 33 % | 0.3 % | | |
| `name >> 6` | 100 % | 100 % | 0.4 % | unchanged | within spread, 0.6-3.8 % fewer instructions |
| Fibonacci hash | 100 % | 100 % | 50 % | unchanged | within spread, +1.5 % instructions on protoJS |

Neither changes time measurably in these workloads; the cache matters
where lookups repeat, and there the conflicts were rare.  Left as found.

## 2. Against C++, the JVM, static and dynamic immutable structures

`final_table.sh`: protoCore 2.14.7 (`attribute_lookup_benchmark`,
`hotcopy_batched`), native C++, and `ImmutableBench.java` on Java 25 with
a mutable Java class, a record (the bytecode of a Scala case class and its
`copy`), Clojure's persistent map (array map up to 8 keys, hash map above)
and Scala's `immutable.HashMap`.  Same shapes (4 / 16 attributes), objects,
random order, checksums; JVM numbers after 3 warm-up repetitions; protoCore
numbers the median over three fresh library copies.

Reads, ns per read (median of three fresh library copies for protoCore):

| Objects | Attrs | C++ in place | JVM mutable class | Java record | Clojure map | Scala HashMap | protoCore |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 4 | 0.8 | 0.7 | 0.7 | 3.2 | 3.6 | 9.9 |
| 1 | 16 | 0.8 | 0.7 | 0.7 | 6.4 | 5.8 | 9.9 |
| 4,096 | 4 | 1.6 | 1.3 | 1.3 | 7.5 | 7.8 | 17.9 |
| 4,096 | 16 | 2.3 | 1.6 | 1.7 | 36.6 | 23.8 | 30.6 |
| 16,384 | 4 | 1.9 | 1.6 | 1.6 | 22.5 | 38.3 | 48.1 |
| 16,384 | 16 | 3.3 | 2.6 | 2.9 | 91.9 | 93.4 | 116.0 |
| 65,536 | 4 | 6.5 | 4.3 | 2.6 | 39.3 | 46.9 | 80.8 |
| 65,536 | 16 | 10.6 | 7.5 | 7.5 | 122.1 | 120.1 | 164.7 |

Updates, ns per update (in place for C++ and the mutable class, a new
version for the others):

| Objects | Attrs | C++ in place | JVM mutable class | Java record copy | Clojure `assoc` | Scala `updated` | protoCore, default heap | protoCore, adaptive heap |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 4 | 0.8 | 1.0 | 5.0 | 15.8 | 73.3 * | 131.8 | 44.2 |
| 1 | 16 | 0.8 | 2.0 | 15.6 | 29.8 | 29.6 | 131.5 | 46.6 |
| 4,096 | 4 | 1.6 | 1.4 | 16.1 | 82.2 | 98.6 | 164.8 | |
| 4,096 | 16 | 2.3 | 1.8 | 54.6 | 122.6 | 134.5 | 170.4 | |
| 16,384 | 4 | 2.5 | 1.8 | 22.1 | 116.4 | 163.0 | 232.1 | |
| 16,384 | 16 | 2.9 | 2.1 | 63.6 | 222.6 | 214.4 | 258.9 | |
| 65,536 | 4 | 11.7 | 4.7 | 27.3 | 139.7 | 220.4 | 292.3 | |
| 65,536 | 16 | 17.7 | 11.9 | 73.9 | 227.5 | 283.9 | 335.6 | |

\* The first cell the JVM runs; 25 ns in the first session.  The adaptive
column is the batched hot copy (`hotcopy_batched`, a child context per
1,000 copies, repetitions 1-4 of each run); the many-object protoCore
copies keep every version alive, so nothing is recycled.  The hot protoCore
read includes about 4 ns of the benchmark's own unboxing across the shared
library (1.1).

Reading the columns left to right, each step adds one property:

| Step | Reads | Writes |
|---|---|---|
| mutable -> immutable, fixed shape (JVM class -> record) | 1x | 5-30x |
| fixed shape -> dynamic keys (record -> best JVM persistent map) | 4.6-32x | 1.9-5.3x |
| JVM persistent map -> protoCore | 1.3-3.1x | 1.6-2.8x hot, 1.2-2.1x many objects |

protoCore additionally gives prototypes, mutable objects shared between
threads without locks or a GIL, and no deoptimisation machinery; the JVM
maps give none of that in this benchmark.  The JVM pays its collector
inside the timed loop; protoCore's copy over many objects is measured
without recycling (all versions stay alive; see 3.2), which disadvantages
it.

## 3. Why the hot copy-update looked slow

- One copy-update allocates exactly 3 cells (4 or 16 attributes): the
  path copy and the new object cell.
- 130-150 ns per copy with the default configuration: 1.05 s system
  against 0.52 s user time, 471 k page faults, heap grown to 30 M cells, 0
  collection cycles.  `getFreeCells` refills from the operating system
  when a freelist runs dry and does not start a cycle (2026-05-23 policy).
- A child context per 1,000 copies (as a runtime's calls are) with
  `PROTOCORE_ADAPTIVE_HEAP=1`: 60 ns (44-47 ns in the second session's
  summary run), 0.08 s system time, 4-6 cycles per repetition, heap 2.1 M
  cells.  A fixed 2 M-cell limit: 75 ns.
- `madvise(MADV_POPULATE_WRITE)` on fresh OS memory cuts the faults
  1,000-fold but not the system time (the cost is fresh pages, not
  traps); a freelist prefetch is slower; transparent huge pages through
  glibc regress after the first repetition.  None adopted.
- In user space the allocation path is about half of a copy:
  `implAllocCell` (the freelist pop reads the free cell), the young-chain
  spinlock (gone in 2.14.7), `allocCell`'s memset.

Real workloads, default against adaptive heap (checksums identical):
js:graph 6.95 -> 5.6 s, js:records 6.04 -> 5.77 s, resident memory
2.2-2.5 GB -> 0.4-0.8 GB, system time 1.3 -> 0.3 s; the small protoScala
and protoPython workloads unchanged.  protoJS keeps the controller off
for sequential programs by a measured decision of 2026-10-04 (13-36 %
slower); section 5 re-measures it.

## 4. The 2.14.7 changes

### 4.1 Value replacement without rebalancing

`sparse_avl::replaceValue`: when the key is present, copy the root-to-key
path keeping each node's size and height, read no sibling, rebalance
nothing; inserts, removals and keys holding no value take the general
`setAt`.  Exactly one cell per node on the path (tested).  Interleaved,
outputs identical:

| | 1 object | 4,096 | 16,384 | 65,536 |
|---|---|---|---|---|
| copy-update, 4 attrs | -9 % | -6 % | -19 % | -21 % |
| copy-update, 16 attrs | -3 % | -27 % | -26 % | -29 % |

Mutable write+read -3 to -8 %.  `SparseListValueReplaceTests` (5 tests)
check AVL invariants against a `std::map` model after random replacements
mixed with inserts and removals; injecting a stale height makes two fail.

### 4.2 No spinlock per allocated cell

`addCell2Context` took the context's spinlock for every cell.  Only the
owning thread pushes onto or detaches its young chain; the collector reads
the head only in phase 2, after the stop-the-world quorum (a thread inside
a critical section does not park, so the collector waits for it; an
unmanaged thread does not allocate; at `SPACE_STATE_ENDING` the collector
leaves before phase 2), and parking orders the accesses.  Hot copy probe
72-76 -> 65-68 ns (-9 % user cycles).  673/673 tests (Release); 667/668
under ThreadSanitizer (one test skips itself under TSan) with no report.

## 5. The five runtimes on 2.14.7

`rerun2.sh` (second session, 2026-10-19 04:38-05:55 UTC, load 1.3-2.0
outside the parallel structures runs): every runtime's own harness, three
rounds interleaving 2.14.6 and 2.14.7, both sides on the **same runtime
binaries** (`build_cmp2146`), differing only in the protoCore library,
loaded from a fresh copy per run (`LD_LIBRARY_PATH`, checked with `ldd` in
`libs.log`).  Every harness verifies its results; every run passed.  The
primary figure is the ratio to the runtime's competitor within the same
run (it cancels load drift); full tables in
[results/tables.md](data/2026-10-18-write-path/results/tables.md).

### 5.1 Suites

| Suite | Workloads | Competitor | Geomean change of the ratio | Rows outside their A/A spread |
|---|---:|---|---:|---|
| protoPython, protopy | 14 | CPython 3.14t | -0.2 % | 4, both signs, by 0.4-0.8 points |
| protoPython, protopyc | 8 | CPython 3.14t | -0.8 % | `memory_pressure` -11.7 % (informational) |
| protoJS standard suite | 11 | Node 22 | -2.1 % | `function_calls` -5.0 %, `string_concat` -5.9 %, `array_literal` +1.6 % |
| protoScala | 12 | CPython 3.14 | -0.6 % | `tak` -5.3 % |
| protoScala | 11 | Scala 3.9.0 on OpenJDK 25 | +1.3 % | 3, by 0.2-1.4 points |
| protoClojure | 5 | Babashka 1.4.192 | 0.0 % | none |
| protoST | 7 | CPython 3.14 | -1.3 % | none |
| protoJS structures (raw ms, one run per side) | 50 | | +0.1 % (seq -0.5 %, par +0.7 %) | single runs: -8 % to +17 % |

2.14.7 is neutral for the runtimes' suites.  The write-path changes help
copy-heavy and allocation-heavy code (the micro-benchmarks of section 4,
`memory_pressure`), which these suites barely contain.

Where each runtime stands (2.14.7, ratio to its competitor, > 1: the
competitor is faster):

- **protoPython** against free-threaded CPython 3.14t: protopy 0.65
  (`multithread_cpu`) to 14.5 (`binary_trees`); protopyc 0.28
  (`multithread_cpu`) to 8.8 (`str_concat_loop`).  `attr_lookup` 6.25 /
  3.26.
- **protoJS** against Node 22: 15.6x on `object_property`, 69-171x on the
  JIT's numeric and call loops, 126x on `object_write_only`; 0.03-0.07 on
  `parallel_cpu` and `string_repeated_doubling` (ropes, threads).  The
  structures workloads: 53-293x sequential, 7-70x with workers (Node pays
  structured clone).
- **protoScala**: 0.87-1.87x CPython on most of its suite, 4.4x on
  `map_build`, 0.26 on `list_ops`; against the JVM's whole process 0.14-0.45
  on the short programs (start-up), 2.8x on `fib30`, 1.8x on `object_tree`,
  0.95 on `map_build`.
- **protoClojure** against Babashka: 1.2-1.7x slower on four workloads,
  2.4x faster on `sum-loop` (5.2).
- **protoST** against CPython: 0.95-2.2x on four, 4.2x `attr_lookup`,
  8.9x `fib`, 20.7x `exception_latency` (C++ unwinding).

`list_ops` on the JVM failed in every run, both sides: the twin folds into
an `Int` and Scala prints `1409965408` (overflow), while protoScala's
integers never overflow (deviation D1) and the expected `9999900000` was
taken from protoScala.  The benchmark is wrong, not the runtime; the fix
(fold into a `Long`) is proposed on a protoScala branch.

### 5.2 protoClojure against Clojure on the JVM

`clj_jvm.py`, three rounds, median; the JVM column once as a whole process
(`java -cp clojure.jar clojure.main`) and once in steady state (best of
five iterations inside one process).  Every result agrees across the three
implementations.

| Workload | protoClojure ms | Babashka ms | Clojure JVM, process | Clojure JVM, steady |
|---|---:|---:|---:|---:|
| fib | 472 | 383 | 853 | 29.8 |
| tak | 41 | 20 | 813 | 9.3 |
| sum-loop | 55 | 140 | 864 | 16.6 |
| reduce-list | 25 | 12 | 852 | 6.2 |
| sum-squares | 18 | 10 | 836 | 6.0 |

As a process protoClojure is 1.8-46x faster than the JVM (start-up of
about 0.8 s); in steady state the JVM's JIT is 3-16x faster than
protoClojure.  Babashka (GraalVM native image, an interpreter) is the
like-for-like peer: within 2.1x either way.

### 5.3 The adaptive heap in protoJS

`adaptive_eval.py`: the five structures workloads (sequential, N = 1),
default heap against `PROTOCORE_ADAPTIVE_HEAP=1`, three interleaved rounds
in alternating order, checksums identical:

| Workload | Default ms | Adaptive ms | Change | Default RSS MB | Adaptive RSS MB |
|---|---:|---:|---:|---:|---:|
| records | 1432 | 1356 | -5.3 % | 2204 | 511 |
| join | 1369 | 1317 | -3.8 % | 1475 | 484 |
| doctree | 1113 | 1160 | +4.2 % | 1064 | 251 |
| wordfreq | 908 | 947 | +4.3 % | 1109 | 147 |
| graph | 1814 | 1634 | -9.9 % | 2492 | 456 |

The 2026-10-04 decision to keep the controller off for sequential protoJS
programs measured it 13-36 % slower; on 2.14.7 it is within -10 % / +4 %
and uses 4-7.5x less memory.  The recommendation is to turn it on by
default in protoJS; the change is protoJS's to make and was not made.

### 5.4 Write probes

`probes.py`: the same loop with and without one attribute write per
iteration (2,000,000), `perf stat` cycles, interleaved before / after /
before again (A/A):

| Runtime | Write | Cycles per write, 2.14.6 | 2.14.7 | A/A | Change |
|---|---|---:|---:|---:|---:|
| protoPython | `o.x = v` | 2,720 | 2,578 | 2,708 | -5.2 % |
| protoJS | `o.x = v` | 2,919 | 2,849 | 3,075 | -2.4 % (A/A 5 %) |
| protoScala | `var` setter | 2,513 | 2,500 | 2,487 | -0.5 % |
| protoScala | case class `copy` | 9,657 | 9,637 | 9,688 | -0.2 % |
| protoClojure | `assoc` | 1,231 | 1,225 | 1,208 | -0.4 % |
| protoST | instance variable | 935 | 852 | 890 | -9.0 % (A/A 5 %) |

protoScala's `copy` costs about 9,600 cycles (2.5 us), against 44-170 ns for
protoCore's own copy-update: the cost is protoScala's call path (four
contexts per `copy`, section 7), not the data structure.

## 6. A measurement artefact: the library file instance

The first session (`results-session1/`) compared a 2.14.6 build tree with
a 2.14.7 build tree and read protoPython as 7-12 % slower on 2.14.7, with
identical instruction counts.  Bisecting the difference:

- the staged 2.14.7 `libprotoCore.so` and fresh `cp` copies of it were
  byte-identical (md5);
- loading the staged file was 13 % slower in cycles, run after run; any
  fresh copy ran at the 2.14.6 speed;
- so the effect follows the file instance, not the bytes.  The likely
  mechanism (not proven here) is the physical placement of the file's
  page-cache pages, which decides cache-set and branch-predictor aliasing
  of the library's text.

The second session therefore loads a fresh copy of each side's library
for every run and runs both sides on the same runtime binaries, so the
only difference between the arms is the library's bytes.  The A/A arms of
section 5.4 (before, after, before again) bound what remains: up to 5 %.
Any before/after comparison of protoCore builds through embedders should
do the same; the 2.14.7 numbers of section 5 replace those of the first
session.


## 7. What a write costs inside each runtime

Write probes (`probes/`, 2,000,000 iterations; base reads an attribute,
write also rewrites one): cycles per write after subtracting the base loop:
protoJS ~2,900, protoPython ~2,750, protoScala `var` ~2,500, protoClojure
`assoc` ~1,260, protoST ~950.  protoCore's own mutable `setAttribute`:
about 800 (attribute path copy, a new object cell, a path copy in the
global mutable table's shard, one CAS).  protoScala, the runtime with the
least compatibility logic, pays about as much as Python and JavaScript:
the cost is not language semantics.  From reading each runtime's write
path (code, not yet profiled):

Systemic, in protoCore:

- **A `ProtoContext` per call is expensive**: a heap slot array
  (`new[]`, filled with `PROTO_NONE`, `delete[]`), registration as the
  thread's current context, a `ReturnReference` cell for every returned
  cell (even singletons), and on destruction `submitYoungGeneration` (a
  spinlock and two CAS on space-wide state, one dirty segment even for one
  cell).  protoScala's `var` write is an interpreted setter call (one
  context), its `copy` four; protoClojure's `assoc` two.
- **A mutable write invalidates the attribute cache for the whole
  object**: the cache keys on the snapshot and every write publishes a new
  one, so every later lookup on that object (the setter, protoJS's eight
  flag attributes, methods) walks the tree.
- The shard path copy in the global mutable table grows with the number
  of live mutable objects (256 shards).
- `hasAttribute` has no cache.

Runtime-specific dead work: protoPython walks the class MRO on every
`STORE_ATTR` and discards the result (left from the removal of `__keys__`
tracking in `6368ec16`); protoJS converts the key to UTF-8 and compares it
with `"length"` on every property store (the same handler already compares
by pointer); protoClojure builds the global's symbol with `createSymbol` on
every `PUSH_VAR`.

The cost of a mutable write is the price of protoCore's model (lock-free
sharing, collector without barriers) and is not treated as a defect; the
per-call context cost is not part of the model and affects immutable-style
programs as much.

## What this does not prove

- One machine, one power profile; synthetic workloads.
- The runtime write-path analysis of section 7 is from reading code;
  profiles are pending.
- The protoJS structures rows of 5.1 are one run per side; their raw
  changes (-8 % to +17 %) are within what single runs of those workloads
  vary.
- The JVM comparison is not JMH; warm-up is manual.

Data and scripts: [data/2026-10-18-write-path/](data/2026-10-18-write-path/).
