#!/bin/bash
# The re-run of 2026-10-12/13: the first session's harness round 2 and its
# perf stat checks overlapped an editor's file listing (~9 CPUs, load ~40,
# from about 21:50 UTC).  Waits for the 1-minute load below 3 with no such
# process, then: probes, cross_run.sh (3 rounds + protoJS structures),
# perfcheck.sh.  Every run is behind the per-run quiet gate as before.
D=$(cd "$(dirname "$0")" && pwd)
WS=$(cd "$D/../../../../.." && pwd)
OUT=$WS/.agent_scratch/markfix-oct12/xrt2; mkdir -p $OUT
stamp() { echo "$(date -u +%FT%TZ) $* load=$(cut -d' ' -f1 /proc/loadavg)"; }
until awk '{exit !($1 < 3.0)}' /proc/loadavg && [ "$(pgrep -c -x rg)" = "0" ]; do sleep 30; done
stamp quiet
python3 $D/probes.py $OUT/probes.jsonl > $OUT/probes.log 2>&1; stamp probes rc=$?
OUT=$OUT ROUNDS=3 bash $D/cross_run.sh > $OUT/cross_run.log 2>&1; stamp cross_run rc=$?
bash $D/perfcheck.sh $OUT/perfcheck.txt > $OUT/perfcheck.log 2>&1; stamp perfcheck rc=$?
stamp end
