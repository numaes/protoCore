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
| 2026-10-03 | [Collector throughput: pacing, early wake and a faster sweep](2026-10-03-collector-throughput.md) — data: [data/2026-10-03-collector-throughput/](data/2026-10-03-collector-throughput/) | 2.12.0.  The sweep's per-cell cost under concurrent allocation was memory latency (about one serialized DRAM miss per cell), not lock contention; a multi-cursor sweep cuts it 2.7–3× at 6–12 threads (wall −26 to −63 %).  Waits now end when cells arrive (they ended on the 50 ms watchdog), and fixed limits pace their cycles (regime-1 waits 20 % → 3 %).  A controller regression found and fixed before release.  2.13.0 (M3): the control law minimises waits within the budget with no fitted constant; waits fall to a third, wall time unchanged (the time goes to collector CPU); a memory-only experiment (10–200 M cells) classifies the workloads and shows the controller at the fixed limits' knee with a fraction of their memory, slower only where a large fixed heap never collects.  2.14.0 (M4): helper threads cut the sweep's cost per cell 2.3–2.6× where it is the bottleneck (wall −6 to −18 % on protoJS N = 12), little on the t6 workloads; aged heaps are almost entirely recycled memory and the gains hold there; a hardware-class section classifies each change as hardware-robust or hardware-sensitive (the latter configurable). |
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
