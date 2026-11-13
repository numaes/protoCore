# ProtoList as an RRB tree — design (November 2026)

Status: **implemented** on branch `perf/list-trie` (`core/ProtoListRrb.h`,
`core/ProtoListRrb.cpp`), unreleased. Sections 1–7 are the design study that
chose the structure and its parameters; section 8 records what the
implementation changed and section 9 what it measured in the kernel.
Prototype: `protoCpp` branch `perf/costmodel`, `costmodel/rrb_lab.cpp`.
Author: Gustavo Marino, with Claude.

## 1. Why this study exists

ProtoList today has two forms: the inline `SmallList` (up to 5 elements in one
cell) and a persistent AVL tree with one element per node. The AVL form makes
every operation O(log n), but with large constants: on DEV12, with 100,000
elements, `getAt` at random costs 588 ns and an append 20 cells (protoCore
v3.5.2, `costmodel_list_ops_kernel`), 18–27 times CPython's C API.

Branch `perf/list-trie` (commit `1fa21724`, phase 1) added a third form: a
persistent radix trie with six slots per 64-byte cell and an open tail, as in
Clojure's vector. It delivered what it promised for the operations it does
natively (`getAt` 47.7 → 11.4 ns, append 390 → 30 ns and 13 → 2 cells, at
N = 1000), and it was **wrong** for everything else: a trie cannot insert,
remove, concatenate or slice in O(log n), so phase 1 converted the whole list to
the AVL form for each of those operations. Deriving many versions from one list
— the normal style of a persistent structure — then costs O(n) per version:

| operation, same base each call | AVL v3.5.2 | trie phase 1 |
|---|---|---|
| `appendFirst`, N = 1,000 | 143 ns / 11 cells | 39,800 ns / 1,011 cells |
| `insertAt` middle, N = 100,000 | 344 ns / 18 cells | 4.4 ms / 100,017 cells |
| `getSlice` half, N = 100,000 | 513 ns / 23 cells | 4.2 ms / 100,017 cells |

The cost model's kernel (20,000 derivations from one list per batch) reached
26.5 GB and was killed by earlyoom. Phase 1 must not be merged. The lesson,
recorded in `tasks/lessons.md`: a representation with a conversion path must be
measured by repeating each converting operation on the same immutable version.

An RRB tree (relaxed radix balanced tree; Bagwell & Rompf 2011, L'orange 2014)
is a radix trie whose inner nodes may be *relaxed*: they carry a table of
cumulative subtree sizes, which lets the tree hold leaves and subtrees that are
not full. With it, concatenation, slicing, insertion and removal anywhere are
O(log n) on the same structure, and a list built by appends is still a plain
radix trie. It is what Scala's `Vector` and Clojure's `core.rrb-vector` use.
This study asks whether it fits protoCore's fixed 64-byte cells, which
parameters keep it balanced, and what it costs per operation.

## 2. Layout in 64-byte cells

A cell has 48 useful bytes: six pointers.

| node | cells | content |
|---|---|---|
| leaf | 1 | up to 6 elements, as stored (nullptr allowed); its length is known from the parent |
| strict inner | 1 | 6 children; *dense*: every child full except the last, every leaf below it full; indexed by base-6 digits |
| relaxed inner | 2 | `R` = { 6 cumulative sizes of 48 bits (36 B), child count, pointer to `C` }; `C` = { 6 children } |
| root (list handle) | 1 | size, tree, height, tail (1..6 elements), tail length |

Decisions and the alternatives they beat:

- **Six cumulative sizes plus six children do not fit in 48 bytes**, so a
  relaxed node takes two cells. Rejected: relaxed nodes of width 4 with 32-bit
  sizes in one cell (a 4-billion-element ceiling per subtree, and two branching
  factors in the rebalancing code); relaxed nodes that store only their own
  total and make the parent read each child (one cache line per child scanned).
  48-bit sizes allow 2.8 × 10^14 elements, beyond any memory the structure
  could occupy (at least 10.7 bytes per element).
- **Strict nodes stay one cell.** A list that only ever grew by appends has no
  relaxed node at all and indexes exactly as the phase-1 trie does.
- **A relaxed child is marked by bit 0 of the pointer in its parent**, so a
  lookup never calls `getType()` (a virtual call; it is what made phase 1's
  `getSize` 1.5 ns slower). In protoCore this means distinct cell types for
  leaves (slots are elements, reported with `asCellPointer`) and inner nodes
  (slots are node pointers, reported with the mark bit stripped), instead of
  phase 1's single `ListTrieNode`.
- A leaf does not store its length: under a strict parent it is full, under a
  relaxed one the size table gives it. This keeps nullptr a legal element.
- A relaxed lookup starts at the radix guess `i / 6^h`, a lower bound of the
  right child because no child holds more than `6^h` elements, and steps right
  through the size table.

## 3. Operations

| operation | how |
|---|---|
| `getAt`, `setAt` | radix walk in strict subtrees, size-table walk in relaxed nodes; `setAt` copies one path |
| `appendLast`, `removeLast` | open tail; a full tail is pushed down the right edge (relaxed nodes take it too); the last leaf comes back as the tail |
| `extend` (concat) | Bagwell–Rompf merge along the seam of the two trees, rebalancing the nodes of the seam at each level |
| `getSlice`, `splitFirst/Last`, `removeFirst` | native `take` / `drop`: one path copied, the cut leaf copied; nodes on the cut become relaxed |
| `insertAt`, `removeAt`, `appendFirst` | **native**, B-tree style (section 5) |
| `removeSlice` | `take` + `drop` + concat |

## 4. Balance: the seam slack E must be 0 with six slots

Bagwell's rebalancing allows a seam `E` nodes more than the optimum
`ceil(slots / 6)`. With 32 slots per node (Clojure, Scala), `E = 2` is a small
slack. With 6 it is not: nodes can stay half full and the height grows like
log3 n instead of log6 n. Measured on lists of 100,000 elements built by
100,000 split-and-concat insertions in the middle (median of 3 runs):

| E | height (optimum 6) | `getAt` random |
|---|---|---|
| 2 | 10 | 418 ns |
| 1 | 9 | 269 ns |
| 0 | 7 | 111 ns |

With `E = 0` the seam is always repacked to the optimum. L'orange's
redistribution plan cannot always reach it (it pours a node only into the nodes
after it), so the prototype falls back to repacking from the first non-full
node, which reaches `ceil(slots / 6)` exactly. The price is about 15% more on
concatenation (`extend` 700 → 800 ns, 30 → 31 cells). **Recommendation: E = 0.**

## 5. Native insertion and removal

Built from split and concat, `insertAt` copies two paths and a seam: 51–56
cells at N = 100,000, three times the AVL's 18. Size tables allow the B-tree
way instead: copy one path, split a full leaf in two, and let the split climb
only as far as it has to. Two refinements keep the tree full:

- **The split leans away from the insertion at either end**, so a run of
  prepends leaves full leaves behind it.
- **B\* split sharing**: a split child first shares its slots with its
  emptiest neighbour (three nodes become two) when they fit. Without it a list
  built by 100,000 insertions in the middle reached height 9 and 304 ns per
  `getAt`; with it, height 6 and 73 ns.

Removal copies one path and merges a child that falls to half capacity or less
with a neighbour when the two fit in one node. Random removals from 100,000
down to 50,000 elements keep height 6 (89 ns per `getAt`).

## 6. Measurements

Prototype on the lab's simulated cell heap (no collector, no
`ProtoContext`), E = 0 with B\*, median of 3 runs, DEV12 at < 10% load.
AVL: protoCore v3.5.2 through `costmodel_list_ops_kernel`, median of 3 rounds
of the interleaved A/B run of 2026-11-12. **Nanoseconds from the two columns
are not comparable** (the kernel pays for real allocation and the collector;
the lab's trie ran about 1.4× faster than the same trie in the kernel);
**cells per operation are**, and they are what the collector pays for.
All operations derive from one base list of 100,000 elements, built by appends
(strict) or by concatenating pieces of 1..40 elements (relaxed).

| operation | AVL kernel ns / cells | RRB lab, strict base | RRB lab, relaxed base |
|---|---|---|---|
| `getAt` random | 586 / 0 | 33 / 0 | 75 / 0 |
| `getAt` sequential, per element | 76 | 17 | 43 |
| iteration, per element | 81 | 0.7 | 2.5 |
| `setAt` random | 897 / 15.7 | 158 / 8 | 273 / 13.8 |
| `appendLast` | 794 / 20 | 10 / 2 | 10 / 2 |
| `removeLast` | 324 / 17 | 8 / 2 | 8 / 2 |
| `appendFirst` | 279 / 18 | 331 / 20 | 354 / 25 |
| `insertAt` middle | 256 / 18 | 395 / 23 | 298 / 17 |
| `removeAt` middle | 269 / 17 | 206 / 14 | 199 / 14 |
| `removeFirst` | 248 / 17 | 204 / 14 | 200 / 14 |
| `getSlice` middle half | 397 / 23 | 309 / 21 | 319 / 27 |
| `extend`, two halves | 300 / 16 | 802 / 31 | — |

Whole-list builds, per element, N = 100,000: by appends 30.5 ns / 2.9 cells; by
prepends 216 ns / 12.8 cells; by insertions in the middle 268 ns / 13.7 cells.
`getAt` afterwards: 33, 58 and 73 ns.

Reading the table:

- Reads, appends and `setAt` gain one to two orders of magnitude in time and
  allocate 2–8 times fewer cells.
- Insertion, removal, prepend and slicing allocate about what the AVL does
  (14–27 cells against 17–23).
- **Concatenation is the one operation that gets more expensive**: about twice
  the AVL's cells (31 against 16), because an RRB seam copies two nodes per
  level and relaxed nodes are two cells.
- A heavily relaxed list reads 2–2.5 times slower than a strict one (75 against
  33 ns), still about 8 times faster than the AVL.

## 7. Correctness evidence so far

`rrb_lab check` runs every operation (both the concat-based and the native
insert/remove) against `std::vector` models with nullptr elements, checking
after every step: sizes in every size table, strict-node density, leaf lengths,
no single-child root, contents through `getAt` and through iteration, and 128
old versions per run intact at the end. 4 seeds × 1,500 steps × 8 rounds × 3
values of E pass. The checker was mutation-tested: an off-by-one in the relaxed
lookup, an oversized merged leaf and an off-by-one in `drop` were each caught.

What the lab does **not** show: behaviour under protoCore's collector and
`ProtoContext` (rooting of half-built paths, concurrent readers), real
allocation cost, and the cost of the cell-type changes in the runtimes. Those
are the implementation's job, with the same test discipline as phase 1 plus the
repeated-derivation benchmark.

## 8. The implementation

The proposal of the study was followed; what differs from the lab is listed
here.

- **Forms.** A list of up to 5 elements is a `SmallList`; every larger list
  produced by a list operation is an RRB tree. The AVL form remains readable
  and accepted by every operation (code may still build AVL cells directly);
  an operation whose operands are all non-AVL never produces it, and one with
  an AVL operand runs the AVL algorithms after converting the other operand.
- **Telling the forms apart.** The RRB root shares `POINTER_TAG_LIST` with the
  AVL root (protoClojure and protoScala recognise lists by tag). A layout
  contract replaces the virtual `getType()` call: the RRB root keeps zero bytes
  where the AVL root keeps `height` and `isEmpty` (a non-empty AVL node has
  height ≥ 1, an empty one has `isEmpty` set) and its size where the AVL root
  keeps `size`, so `getSize` reads either form at one offset. `static_assert`s
  in `ProtoListRrb.cpp` pin the offsets; the bytes are read through the object
  representation (`memcpy`), not through a pointer of the other type.
- **Cells.** `ListRrb` (root: tree, height, tree size, tail, tail length),
  `ListRrbLeaf`, `ListRrbInner`, `ListRrbSizes`. New `CellType` values are
  appended, so existing values keep their numbers.
- **Concatenation merges one seam.** The left operand's tail joins the seam of
  the two trees as a middle leaf, instead of being pushed into the left tree
  first (a second seam merge); up to 5 + 3 + 5 nodes meet at a level and are
  repacked to the optimum. This cut `extend` of two halves by 38 %. A right
  operand without a tree (at most 6 elements) joins the left tail, and a full
  leaf is pushed as an append would, so short extends do not relax the tree.
- **Size limit.** Size tables hold 48 bits; an `extend` or `multiply` whose
  result would pass 2^48 − 1 elements raises `ProtoErrorKind::Overflow`.
- **Rooting.** Every operation runs inside the caller's critical section;
  half-built paths live in C++ locals until the root is returned, as for the
  AVL form.
- **Iterator.** An iterator over an RRB list carries the leaf holding its
  position and the range it covers; advancing inside a leaf reads no node.

Tests (`test/ListRrbTests.cpp`, structural checker `test/ListRrbCheck.h`, also
applied by `ListStructuralOpsTests` to every list it sees): every operation
against `std::vector` models with nullptr elements, every change of form and
height (5→6→7, 36→37, 216→217, 1296→1297), versions kept and re-checked,
bounded cells per derivation from one base list (the phase-1 regression),
height within one level of the optimum after prepends, middle inserts,
random inserts, random removals and concatenation of small pieces, AVL
operands alone and mixed, the size limit, collections between operations, and
four ProtoThreads deriving from one shared list while a kicker requests
collections. Mutation-tested: an off-by-one in the relaxed lookup, a merge
that overfills a node and a seam left unbalanced were each caught. The full
suite passes (852 tests); the list and collector suites are clean under
AddressSanitizer and ThreadSanitizer.

## 9. Measured in the kernel

Interleaved A/B on DEV12 (AMD Ryzen 5 5500U), 2026-11-13: blocks base, RRB,
RRB, base, each `costmodel/run.sh` with 3 rounds (`protoCpp` `perf/costmodel`,
`costmodel_list_ops_kernel`, adaptive heap, a fresh copy of the library per
round, machine busy 4–8 %); base is protoCore v3.5.2 (AVL), RRB is commit
`13f5da3f`; `ldd` checked per block. Median of the 6 per-round medians per
side; every operation derives from one base list built by appends.
Results in `worktrees/costmodel/results/ab-20261113-0027`.

| operation | N | AVL ns / cells | RRB ns / cells | AVL / RRB |
|---|---:|---:|---:|---:|
| `getSize` | 1000 | 2.0 / 0.00 | 2.0 / 0.00 | 1.00× |
| `getLast` | 1000 | 15.4 / 0.00 | 6.1 / 0.00 | 2.53× |
| `getAt_random` | 1000 | 47.5 / 0.00 | 10.6 / 0.00 | 4.48× |
| `setAt_random` | 1000 | 232.9 / 8.99 | 181.2 / 4.99 | 1.29× |
| `appendLast_on_N` | 1000 | 399.6 / 13.00 | 89.8 / 2.00 | 4.45× |
| `appendFirst` | 1000 | 156.6 / 11.00 | 406.3 / 11.00 | 0.39× |
| `insertAt_middle` | 1000 | 439.8 / 11.00 | 282.5 / 12.00 | 1.56× |
| `removeAt_middle` | 1000 | 147.4 / 10.00 | 376.9 / 8.00 | 0.39× |
| `removeFirst` | 1000 | 142.0 / 10.00 | 372.8 / 8.00 | 0.38× |
| `removeLast` | 1000 | 401.5 / 10.00 | 36.8 / 2.00 | 10.90× |
| `extend_two_halves` | 1000 | 111.6 / 9.00 | 502.5 / 11.00 | 0.22× |
| `getSlice_middle_half` | 1000 | 873.5 / 14.00 | 327.6 / 13.00 | 2.67× |
| `splitFirst_half` | 1000 | 94.1 / 7.00 | 165.4 / 5.00 | 0.57× |
| `iterate_all` | 1000 | 28.6 / 1.001 | 21.9 / 1.001 | 1.31× |
| `getAt_sequential_all` | 1000 | 27.1 / 0.000 | 10.1 / 0.000 | 2.69× |
| `has_missing_scan` | 1000 | 8.4 / 0.000 | 7.7 / 0.000 | 1.09× |
| `getSize` | 100000 | 2.2 / 0.00 | 2.1 / 0.00 | 1.06× |
| `getLast` | 100000 | 25.6 / 0.00 | 6.3 / 0.00 | 4.08× |
| `getAt_random` | 100000 | 586.0 / 0.00 | 36.4 / 0.00 | 16.12× |
| `setAt_random` | 100000 | 891.5 / 15.69 | 355.5 / 8.00 | 2.51× |
| `appendLast_on_N` | 100000 | 777.3 / 20.00 | 30.3 / 2.00 | 25.65× |
| `appendFirst` | 100000 | 275.7 / 18.00 | 470.9 / 20.00 | 0.59× |
| `insertAt_middle` | 100000 | 255.7 / 18.00 | 583.5 / 23.00 | 0.44× |
| `removeAt_middle` | 100000 | 266.4 / 17.00 | 313.3 / 14.00 | 0.85× |
| `removeFirst` | 100000 | 243.3 / 17.00 | 300.9 / 14.00 | 0.81× |
| `removeLast` | 100000 | 296.7 / 17.00 | 83.1 / 2.00 | 3.57× |
| `extend_two_halves` | 100000 | 283.2 / 16.00 | 753.9 / 19.00 | 0.38× |
| `getSlice_middle_half` | 100000 | 402.6 / 23.00 | 547.2 / 22.00 | 0.74× |
| `splitFirst_half` | 100000 | 228.7 / 12.00 | 267.5 / 8.00 | 0.86× |
| `iterate_all` | 100000 | 80.3 / 1.000 | 40.9 / 1.000 | 1.97× |
| `getAt_sequential_all` | 100000 | 76.2 / 0.000 | 23.2 / 0.000 | 3.29× |
| `has_missing_scan` | 100000 | 75.3 / 0.000 | 8.1 / 0.000 | 9.35× |

Whole-list builds in the general kernel table (`costmodel_kernel`, same run):
appending to 100,000 elements 296 → 62 ns and 18.7 → 2.9 cells per element;
`getAt` random over 100,000 elements 606 → 40 ns, sequential 75 → 22 ns.
Against CPython's C API, list append goes from 16.9× to 2.0× and random index
from 26.7× to 1.8×.

Reading the table:

- **Reads, appends, `removeLast` and `setAt` win by 2.5× to 26×** at 100,000
  elements, and allocate 2–9 times fewer cells. Iteration wins 2× only: the
  immutable iterator allocates a cell per `advance`, which the RRB cannot
  remove; a scan through `has` (no iterator) wins 9.4×.
- **Prepend, removal, slicing and concatenation are slower in time.** At
  N = 100,000: `removeAt` 1.2×, `splitFirst` 1.2×, `removeFirst` 1.2×,
  `getSlice` 1.4×, `appendFirst` 1.7×, `insertAt` 2.3×, `extend` 2.7×. At
  N = 1,000: `splitFirst` 1.8×, `appendFirst`, `removeAt` and `removeFirst`
  2.6×, `extend` 4.5× (there `insertAt` and `getSlice` are faster, 1.6× and
  2.7×). The cell counts are similar (5–23 against 7–18). The extra time is CPU spent building nodes (decoding children, size
  tables, the B\* attempt, relaxed nodes taking two cells); a profile of
  `insertAt` is flat across the node builders, with no single hot spot. An
  insertion into a dense tree splits a full node at every level, which is the
  B-tree price of keeping nodes full (and reads fast).
- **Cells: every operation allocates at most 1.3× the AVL's cells**
  (`insertAt` middle 23 against 18); concatenation fell from 31 cells in the
  study to 19 with the single-seam merge (AVL 16). The acceptance threshold of
  the study (≤ 2×) holds everywhere.
- **The study's speed thresholds hold partly.** ≥ 5× was asked for `getAt`
  (16×), `appendLast` (26×), `removeLast` (3.6×), `setAt` (2.5×) and
  iteration (2×); the last three miss it.
- The base list in this table is strict; a heavily relaxed list reads about
  2× slower (study, section 6).
- One artefact to know when reading the general kernel table: its string
  appends, which use no list code, read 300 → 550 ns (2,000 characters, wall)
  and 370 → 690 ns (10,000, CPU) in the RRB blocks. Run alone, against each
  library, they are equal or faster with the RRB (2,000: 311 → 275 ns;
  10,000: 381 → 346 ns, median of 3). Each kernel measurement inherits the
  heap and collector state that the previous ones left in the same process,
  and the list benchmarks before them now leave six times less garbage.

### In the runtimes

Built against this branch (`ldd` checked) and against v3.5.2, interleaved,
every run's output identical on both sides, median of 5 rounds:
protoST `list_append` −18 %, `parallel_speedup` (pool) −21 %,
`saturation_big` w=6 −14 %, `range_iterate` +8 %, the rest within ±2 %
(its own `compare_builds.py`); protoScala `list_ops` −29 %, the rest within
±4 %; protoClojure neutral (its lists are `ProtoLinkedList`, its vectors
`ProtoTuple`). Their suites pass against the branch: protoST 1,291,
protoClojure 886, protoScala 2,524 tests.

### Open: the adaptive-heap storm gate

`AdaptiveHeapStorm.SoftLimitRisesUnderStall` fails in CI (Linux release,
macOS, Windows x64) and on DEV12 under `taskset -c 0,1`; v3.5.2 passes under
the same restriction. The controller's trace shows the collector's throughput
never measured (`T = 0` every cycle): the controller takes it only from wide
sweeps, and every sweep of this single-mutator storm is narrow. Under the AVL
list, building the live set allocated beyond the initial soft limit and the
storm's sweeps were wide, so `T` was measured and S grew. Without `T` the law
keeps S although the mutator waits 20–40 % of the time. Why every storm sweep
is narrow with the RRB live set is not established yet. The fix belongs to the
controller, so the branch is not merged until that is decided.

## References

- P. Bagwell, T. Rompf, *RRB-Trees: Efficient Immutable Vectors*, EPFL 2011.
- J. N. L'orange, *Improving RRB-Tree Performance through Transience*, 2014.
- N. Stucki et al., *RRB Vector: A Practical General Purpose Immutable Sequence*, ICFP 2015.
