#!/bin/bash
# The second session of 2026-10-19: the first one loaded every AFTER-side
# protoCore from one file instance whose text placement cost protoPython
# 13 % (fresh copies of the same bytes did not), and compared two separate
# runtime builds.  Here both sides run the same runtime binaries and differ
# only in the protoCore library, loaded from a fresh copy per run: probes,
# cross_run.sh (3 rounds + protoJS structures), the protoJS adaptive-heap
# evaluation, protoClojure against the JVM, and the summary table.
D=$(cd "$(dirname "$0")" && pwd)
WS=$(cd "$D/../../../../.." && pwd)
OUT=$WS/.agent_scratch/writepath-oct18/xrt2; mkdir -p $OUT/libs
export PROBE_LIBS=$OUT/libs
stamp() { echo "$(date -u +%FT%TZ) $* load=$(cut -d' ' -f1 /proc/loadavg)"; }
until awk '{exit !($1 < 3.0)}' /proc/loadavg && [ "$(pgrep -c -x rg)" = "0" ]; do sleep 30; done
stamp quiet
python3 $D/probes.py $OUT/probes.jsonl > $OUT/probes.log 2>&1; stamp probes rc=$?
OUT=$OUT ROUNDS=3 bash $D/cross_run.sh > $OUT/cross_run.log 2>&1; stamp cross_run rc=$?
python3 $D/adaptive_eval.py $OUT/adaptive.jsonl 3 > $OUT/adaptive.log 2>&1; stamp adaptive rc=$?
python3 $D/clj_jvm.py $OUT/clj_jvm.jsonl ${JARS:?set JARS to the jar directory} 3 > $OUT/clj_jvm.log 2>&1; stamp clj_jvm rc=$?
OUT=$OUT/final bash $D/final_table.sh > $OUT/final_table.log 2>&1; stamp final_table rc=$?
stamp end
