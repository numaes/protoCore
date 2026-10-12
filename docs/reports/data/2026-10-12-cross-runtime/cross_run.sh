#!/bin/bash
# Cross-runtime re-measurement, 2026-10-12: each runtime's own comparison
# harness, run with the runtime built against protoCore 2.14.5 (BEFORE,
# */build_cmp2145) and the same commit built against 2.14.6 (AFTER,
# */build_pkg_oct12), alternating sides round by round, each run behind the
# 2026-10-04 harness's quiet gate.  Outputs in $OUT.
#   protoPython  benchmarks/run_4way_interleaved.py (CPython 3.14, 3.14t; the protopyc
#                column is required: without it the script fails writing its report)
#   protoJS      tests/benchmarks/run_standard_comparison.js (Node 22)
#   protoScala   benchmarks/run_benchmarks.py (CPython, Scala 3.9.0 on the JVM)
#   protoClojure benchmarks/bench.sh (Babashka 1.4.192)
#   protoJS      benchmarks/structures/run.py, once per side (median of 3 in-process)
set -u
D=$(cd "$(dirname "$0")" && pwd)
WS=${PROTO_WORKSPACE:-$(cd "$D/../../../../.." && pwd)}
OUT=${OUT:-$WS/.agent_scratch/markfix-oct12/xrt}; mkdir -p $OUT
ROUNDS=${ROUNDS:-3}
Q="python3 $WS/protoCore/docs/reports/data/2026-10-12-mark-stack-int-compare/quiet.py"
tree() { [ $1 = before ] && echo build_cmp2145 || echo build_pkg_oct12; }
stamp() { echo "$(date -u +%FT%TZ) $*"; }
NODE=$(command -v node || ls -d $HOME/.nvm/versions/node/*/bin/node | tail -1)
export PATH=$(dirname $NODE):$PATH
for r in $(seq 1 $ROUNDS); do
  for side in before after; do
    t=$(tree $side)
    $Q > /dev/null; stamp "py $side r$r"
    ( cd $WS/protoPython && PROTOPY_BIN=$WS/protoPython/$t/src/runtime/protopy \
        PROTOPYC_BIN=$WS/protoPython/$t/src/compiler/protopyc RUN_MODULE_BIN=$WS/protoPython/$t/test/compiler/run_module \
        LD_LIBRARY_PATH_BENCH=$WS/protoPython/$t/src/library \
        CPYTHON_BIN=/usr/local/bin/python3.14 CPYTHONT_BIN=$HOME/.local/bin/python3.14t \
        python3 benchmarks/run_4way_interleaved.py -o $OUT/py_${side}_r$r.md > $OUT/py_${side}_r$r.log 2>&1 )
    $Q > /dev/null; stamp "js $side r$r"
    ( cd $WS/protoJS && PROTOJS_BIN=$WS/protoJS/$t/protojs node tests/benchmarks/run_standard_comparison.js \
        > $OUT/js_${side}_r$r.txt 2>&1; cp tests/benchmarks/results/standard_comparison.json $OUT/js_${side}_r$r.json 2>/dev/null )
    $Q > /dev/null; stamp "scala $side r$r"
    ( cd $WS/protoScala && PROTOSCALA_BIN=$WS/protoScala/$t/protoscala PROTOSCALA_RELEASE_BIN=/nonexistent \
        PROTOPY_BIN=/nonexistent PROTOST_BIN=/nonexistent PROTOCLJ_BIN=/nonexistent \
        CPYTHON_BIN=/usr/local/bin/python3.14 SCALA_HOME=$WS/tools/scala3-dl/scala3-3.9.0-x86_64-pc-linux \
        python3 benchmarks/run_benchmarks.py --output $OUT/scala_${side}_r$r.md > $OUT/scala_${side}_r$r.log 2>&1 )
    $Q > /dev/null; stamp "clj $side r$r"
    ( cd $WS/protoClojure && bash benchmarks/bench.sh $WS/protoClojure/$t/protoclj $WS/tools/bb-1.4.192/bb \
        > $OUT/clj_${side}_r$r.txt 2>&1 )
  done
done
for wl in records join doctree wordfreq graph; do
  for side in before after; do
    t=$(tree $side); $Q > /dev/null; stamp "struct $wl $side"
    ( cd $WS/protoJS/benchmarks/structures && python3 run.py --protojs $WS/protoJS/$t/protojs --reps 3 \
        --ns 1,2,4,6,12 --scale 1 --heap-cells 40000000 --memory-max 4G --only $wl \
        --out $OUT/structures_$side.jsonl >> $OUT/structures_$side.log 2>&1; echo "rc=$?" >> $OUT/structures_$side.log )
  done
done
stamp end
