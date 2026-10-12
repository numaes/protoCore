#!/bin/bash
# The 2026-10-12 session, sequential.  Results in $FM_ROOT/res.
D=$(cd "$(dirname "$0")" && pwd)
ROOT=${FM_ROOT:-$(cd "$D/../../../../.." && pwd)/.agent_scratch/markfix-oct12}
R=$ROOT/res; mkdir -p $R
stamp() { echo "$(date -u +%FT%TZ) $* load=$(cut -d' ' -f1 /proc/loadavg)"; }
# The collector workloads of the phase report, without the three protoJS
# runs at 10 M cells that end in out-of-memory on both sides (join N = 6, 12)
# and join N = 1 with them.
GC_ONLY="core_*,js40_*,js10_records_*,js10_doctree_*,js10_wordfreq_*,js10_graph_*,jsad_*,scala_*,clj_*,cad_20k"
stamp start
python3 $D/spec.py $R/spec.jsonl --reps 5 > $R/spec.log 2>&1; stamp spec rc=$?
python3 $D/fm.py gc $R/gc.jsonl --reps 3 --only "$GC_ONLY" > $R/gc.log 2>&1; stamp gc rc=$?
python3 $D/fm.py cal $R/cal.jsonl --reps 3 > $R/cal.log 2>&1; stamp cal rc=$?
stamp end
