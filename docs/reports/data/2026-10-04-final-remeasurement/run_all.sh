#!/bin/bash
# The whole 2026-10-04 session, sequential.  Results in $FM_ROOT/res.
WS=${PROTO_WORKSPACE:-$(cd "$(dirname "$0")/../../../../.." && pwd)}
ROOT=${FM_ROOT:-$WS/.agent_scratch/final-oct04}
D=$(cd "$(dirname "$0")" && pwd)
R=$ROOT/res; mkdir -p $R
export MAX_LOAD=${MAX_LOAD:-4.5}
stamp() { echo "$(date -u +%FT%TZ) $* load=$(cut -d' ' -f1 /proc/loadavg)"; }
stamp start
python3 $D/fm.py cells $R/cells.jsonl --reps 3 > $R/cells.log 2>&1; stamp cells rc=$?
python3 $D/fm.py rt6 $R/rt6.jsonl --reps 3 --only "js/*" >> $R/rt6.log 2>&1; stamp rt6-js rc=$?
python3 $D/fm.py cal $R/cal.jsonl --reps 3 > $R/cal.log 2>&1; stamp cal rc=$?
python3 $D/fm.py gc $R/gc.jsonl --reps 3 > $R/gc.log 2>&1; stamp gc rc=$?
bash $D/struct.sh $R/struct > $R/struct.log 2>&1; stamp struct rc=$?
stamp end
