# Demonstrator: a versioned CAD model under concurrent editing (design)

Date: 2026-10-19.  Status: proposal, not implemented.  Author: Gustavo
Marino, with Claude.

## 1. Purpose

protoCore's benchmarks so far compare raw operations: attribute reads,
copy-updates, collector throughput.  On those, the 2026-10-18 measurements
place protoCore within 1.2-3.2x of the JVM's persistent maps and 9-150x
behind mutable objects with compile-time offsets
(`docs/reports/2026-10-18-write-path.md`).  None of them measures what the
model is for: **many readers that each need a consistent version of a large
object graph while an editor keeps producing new versions**, with older
versions kept for undo and comparison.  On the JVM, Node or C++ that
guarantee is the application's job (locks, copies, or a hand-made version
protocol); in protoCore it is the object model.

The demonstrator measures what each platform's *honest* solution to that
problem costs: reader latency under editing, editor throughput, memory per
retained version, and the code needed to be correct.  It is written in the
paradigm's style (immutable values, versions as values), not in mutable
style, following the 2026-10-03 lesson that benchmarks must not measure
protoCore on mutable-style ground.

## 2. Claims under test

1. A reader on protoCore sees a consistent version without locks, and an
   editor never waits for readers.
2. Retaining the last K versions costs memory proportional to what changed
   between them (structural sharing), not K times the model.
3. The equivalent guarantee on the JVM, Node or C++ costs either reader
   latency (locks), memory and time (copies), or a persistent-structure
   library that the program must be written around (Clojure).
4. Without any guarantee (unsynchronised mutable model) readers observe
   torn versions; the demonstrator counts them, so the guarantee is shown
   to be needed, not assumed.

## 3. Workload

**Model.** The CAD model of protoJS `benchmarks/structures/cad/model.js`:
an assembly tree (depth 3, branching 8), parts as box solids with
topology (8 vertices, 12 edges, 6 faces), materials, layers, metadata.
Scale: 10 k parts (development) and 200 k parts (the existing multi-GB
run).  Integer arithmetic only, so every implementation gets identical
checksums.

**Editor (one thread).**  A deterministic stream of edit transactions
(seeded): move a part (8 vertex updates), change a part's material, move a
sub-assembly (transform update), add a part, delete a part.  Each
transaction keeps two **cross-object invariants** that a reader can check:

- *Mass invariant:* every assembly stores the total mass of its subtree;
  an edit that changes a part's material or adds/removes a part updates
  the part and every ancestor's total in the same transaction.
- *Topology invariant:* every edge's endpoints are vertices of its own
  part; moving a part updates all 8 vertices in one transaction.

A reader that sees half a transaction finds an ancestor total that does
not match its subtree, or a box that is not a box.  The editor publishes
version v+1 when a transaction completes.

**Readers (R threads, R = 1, 2, 4, 6, 11).**  Each repeatedly takes the
current version and runs one analysis over it: bill of materials (mass by
material), clash count on a spatial grid, layer visibility export, the
integrity analysis of the existing benchmark.  Every reader verifies both
invariants on the version it read and records the version number,
latency and checksum.

**Versions.**  The last K = 16 versions stay reachable (undo stack); a
reader may also be asked to diff version v against v-8.

**Rates.**  Editor: unthrottled (throughput) and fixed rates (100, 1 k,
10 k transactions/s) to measure reader latency under a known load.

## 4. Implementations

| Platform | Strategy | Notes |
|---|---|---|
| protoJS on protoCore | Immutable-style edits (new objects along the edited path, spread), a version is one root value; readers are Deferreds on the shared model | No locks, no copies |
| protoScala on protoCore | Case classes and `copy`; same structure | The paradigm's leading case |
| Java 25, mutable + `ReentrantReadWriteLock` | Readers hold the read lock for a whole analysis; the editor takes the write lock per transaction | The common enterprise answer; editor starves or readers wait |
| Java 25, mutable + copy per version | The editor deep-copies the model on publish (or copy-on-write per subtree by hand) | Correct without reader locks; measures copy time and memory |
| Clojure 1.12 on the JVM | Persistent maps/vectors, versions in an atom | Same paradigm on the JVM: the strongest competitor, reported as such |
| Node 22 | Workers; each version reaches workers by structured clone (or SharedArrayBuffer columns as in `cad/soa.js`) | Node has no shared object graphs between threads |
| Java 25, mutable, unsynchronised | No guarantee | Only to count torn reads and exceptions; never reported as a time |

The same edit stream, analyses, invariants and checksums in every
implementation; each prints its checksums and invariant-violation count
and the runner verifies them (benchmarks must self-report).

## 5. Metrics

- Reader latency per analysis: p50, p99, max, at each editor rate and R.
- Editor throughput (transactions/s) at each R; editor wait time.
- Versions observed per reader per second (staleness).
- Memory: RSS and live heap with K = 1 and K = 16 retained versions; bytes
  per retained version.
- Correctness: invariant violations and exceptions (expected 0 except the
  unsynchronised baseline).
- Code: lines of the synchronisation/versioning layer in each
  implementation (the part the platform does not give for free).

## 6. Method

- The 2026-10-04 harness rules: quiet gate per run, interleaved
  implementations, three rounds, `perf stat` on single runs, results with
  their spread.
- Each process reports its own work (transactions applied, analyses run,
  checksums); a run that does not complete is reported as such.
- The whole reader curve (R = 1..11), not a single point.
- Heap policy stated for every protoCore run (the 2026-10-18 finding: the
  default configuration never collects; the adaptive controller is the
  realistic setting for a long-running editor).
- JVM: `-Xms`/`-Xmx` fixed and reported; GC logs kept.

## 7. What it will not show

- Raw single-thread speed: the JVM will be faster per operation; that is
  measured elsewhere and is not the question.
- Distribution or replication (Croquet's replicated computation): out of
  scope; the demonstrator is one process.

## 8. Plan

1. Edit stream, invariants and analyses in JS on top of `model.js`; the
   unsynchronised Java baseline first, to prove torn reads occur.
2. protoJS and Node implementations; then Java (RW lock, copy) and
   Clojure; protoScala last (needs the model ported to case classes).
3. Runner with self-verification; development scale, then 200 k parts.
4. Report in protoCore `docs/reports/`, with the code-size table.

Open questions for the maintainer: editor rates of interest; whether a
protoST implementation (the digital-twin base) should replace or join
protoScala; K.
