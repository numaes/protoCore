# Measurement and incident reports

Dated reports of measurements and investigations.  Each one states its
method, the versions measured and what it does not prove.  The conclusions
that affect the design are also kept in the design documents
(`GarbageCollector.md`, "Measured behaviour"), so they survive even if a
report is not read.

Raw data and the scripts that produced it are in `data/<report name>/`.
The scripts record exactly what was run on the measuring machine (staged
binaries, library paths); set `PROTO_WORKSPACE` to the directory holding
the protoCore and runtime checkouts, and adapt the staged-binary paths,
to run them elsewhere.

| Date | Report | Key findings |
|---|---|---|
| 2026-10-03 | [Collector time, phase by phase](2026-10-03-gc-phase-breakdown.md) — data: [data/2026-10-03-gc-phase-breakdown/](data/2026-10-03-gc-phase-breakdown/) | The sweep dominates collector time (41–99 %); stop-the-world ≤ 0.23 ms per cycle.  With 12 allocating threads the single collector is busy 56–95 % of wall time and mutators wait 17–63 %.  Sweep cost per cell rises 3–4× under concurrent allocation (suspected cache coherence, unmeasured).  Fixed limits need pacing.  Amdahl: a 1.2–3.9× faster sweep reaches the zero-wait bound; parallel mark pays only near a full heap. |
| 2026-10-03 | [Adaptive heap controller: calibration](2026-10-03-adaptive-heap-calibration.md) — data: [data/2026-10-03-adaptive-heap-calibration/](data/2026-10-03-adaptive-heap-calibration/) | A pressure-only soft limit runs away for fast allocators (20× live); capped at 8× live in 2.10.1.  Memory at or below the previous policy on 40/44 workloads, time within +5 % on 28/44; the remaining cost is collector throughput.  Rejected alternatives are listed with their numbers. |
| 2026-10-03 | [GCRootScope failure on macOS arm64](2026-10-03-gcrootscope-macos-flake.md) | A test bug (an object reachable only from a C++ local across an allocation, against EMBEDDER-CONFORMANCE rule 3), not a collector bug: 9/1,000 before, 0/3,600 after.  Side finding: a destroyed `ProtoSpace` does not return its memory (issue #2). |

Related reports in the runtimes:

- protoJS `benchmarks/reports/2026-10-02-memory-per-object.md` — most of
  protoJS's 5–6 KB per object was garbage between collections; live data
  is 4–11 cells per flat object; a mutable write costs about 12 cells (half
  object path copy, half mutable-table path copy).
- protoJS `benchmarks/reports/2026-10-03-structure-benchmarks.md` —
  mutable-style structure workloads and a CAD model against Node.js:
  protoJS 60–140× slower per task, sharing without copies where Node's
  workers clone; the single collector limits scaling.
- protoJS `benchmarks/reports/2026-10-02-parallel-deferred.md` — Deferreds on
  the CPU pool: 4.2× on 6 cores; ahead of Node's `postMessage` clone on a
  500 k-object graph.
