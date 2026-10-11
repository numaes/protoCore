# Data: collector adjustments (2026-10-04)

- `fm.py`, `quiet.py`, `tab.py`: the final re-measurement's harness with two
  more sides (`ref` = 2.14.1 rebuilt, `adj` = the branch) and a stricter quiet
  gate (no runtime process, no test262 driver).  `tab.py gc <jsonl>` and
  `tab.py cal2 <jsonl>` print the report's tables (`TAB_A`/`TAB_B` pick the sides).
- `results/release/`: the release library (`2618fc0f`) against 2.14.1 --
  `gc.jsonl` (A1 table), `cal.jsonl` (A3 table), `ab_records_n12.log`.
- `results/gc3.jsonl`: the three-way 2.10.2 / 2.14.1 / `d5fca7b0` session (A2).
  Other files in `results/` are the `d5fca7b0` session's rows, superseded by
  `results/release/` and kept as the record.
- `results/pacing_matrix.log`, `width_matrix_v2.log`: the exploratory
  matrices (A2 pacing variants, A1 width rules).
- `a1/`: per-thread perf, IBS runs and the placement experiment.
- `a4/`: `perf stat -r 5` CSVs of the five builds (`a4.sh`, `a4_build.sh`).
- `scripts/`: the drivers.  Paths are scrubbed: `$WS` the workspace,
  `$FM_ROOT` the re-measurement's staging tree, `$ADJ`/`$SCRATCH` the
  session's scratch directory, `$PROTOCORE` the branch checkout.
