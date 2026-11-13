# ProtoList as an RRB tree — design study (November 2026)

Status: **proposal**, not implemented in protoCore. Prototype and measurements:
`protoCpp` branch `perf/costmodel`, `costmodel/rrb_lab.cpp` (commit `db675a3`).
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

## 8. Proposal for the implementation

1. Replace the phase-1 trie on `perf/list-trie` with the RRB tree; the AVL
   cell type stays as a readable input form (lists built by older code paths,
   tests), and every mutator outputs `SmallList` (≤ 5) or RRB.
2. Keep `POINTER_TAG_LIST` for the RRB root (protoClojure and protoScala
   recognise lists by tag), told apart from the AVL by cell type; make that
   check cheap (no virtual call on the hot path).
3. Cell types: `ListRrb` (root), `ListRrbLeaf`, `ListRrbInner`, `ListRrbSizes`
   (the `R` cell of a relaxed node).
4. E = 0 with the optimal-repack fallback; native insert/remove with B\* and
   merge on underflow; native take/drop.
5. Acceptance, all against v3.5.2 in the interleaved A/B harness, deriving from
   one base list: no operation allocates more than 2× the AVL's cells (concat
   is the expected worst case); `getAt`, `setAt`, `appendLast`, `removeLast` and
   iteration at least 5× faster; the costmodel kernel completes with bounded
   memory; the full protoCore suite, ASan and TSan clean; the runtimes' suites
   linked against the branch (verified with `ldd`).

## References

- P. Bagwell, T. Rompf, *RRB-Trees: Efficient Immutable Vectors*, EPFL 2011.
- J. N. L'orange, *Improving RRB-Tree Performance through Transience*, 2014.
- N. Stucki et al., *RRB Vector: A Practical General Purpose Immutable Sequence*, ICFP 2015.
