#!/bin/bash
# The session of 2026-10-18/19 (protoCore 2.14.6 -> 2.14.7).  Waits for the
# 1-minute load below 3, then: write-path probes, cross_run.sh (3 rounds of
# every runtime's own harness + protoJS structures), the protoJS
# adaptive-heap evaluation, and protoClojure against Clojure on the JVM.  Every run is behind the per-run quiet gate.
D=$(cd "$(dirname "$0")" && pwd)
WS=$(cd "$D/../../../../.." && pwd)
OUT=$WS/.agent_scratch/writepath-oct18/xrt; mkdir -p $OUT
stamp() { echo "$(date -u +%FT%TZ) $* load=$(cut -d' ' -f1 /proc/loadavg)"; }
until awk '{exit !($1 < 3.0)}' /proc/loadavg && [ "$(pgrep -c -x rg)" = "0" ]; do sleep 30; done
stamp quiet
python3 $D/probes.py $OUT/probes.jsonl > $OUT/probes.log 2>&1; stamp probes rc=$?
OUT=$OUT ROUNDS=3 bash $D/cross_run.sh > $OUT/cross_run.log 2>&1; stamp cross_run rc=$?
python3 $D/adaptive_eval.py $OUT/adaptive.jsonl 3 > $OUT/adaptive.log 2>&1; stamp adaptive rc=$?
python3 $D/clj_jvm.py $OUT/clj_jvm.jsonl ${JARS:?set JARS to the jar directory} 3 > $OUT/clj_jvm.log 2>&1; stamp clj_jvm rc=$?
stamp end
