#!/bin/bash
# Cross-runtime re-measurement, 2026-10-18: each runtime's own comparison
# harness, run with the runtime built against protoCore 2.14.6 (BEFORE,
# */build_cmp2146) and the same commit built against 2.14.7 (AFTER,
# */build_pkg_oct18), alternating sides round by round, each run behind the
# 2026-10-04 harness's quiet gate.  Outputs in $OUT.  Same harnesses as
# 2026-10-12, plus protoST benchmarks/run_benchmarks.py (CPython 3.14).
# Every run loads its protoCore from a fresh copy (fresh(), below).
#   protoPython  benchmarks/run_4way_interleaved.py (CPython 3.14, 3.14t; the protopyc
#                column is required: without it the script fails writing its report)
#   protoJS      tests/benchmarks/run_standard_comparison.js (Node 22)
#   protoScala   benchmarks/run_benchmarks.py (CPython, Scala 3.9.0 on the JVM)
#   protoClojure benchmarks/bench.sh (Babashka 1.4.192)
#   protoST      benchmarks/run_benchmarks.py (CPython 3.14; comparable workloads and actors)
#   protoJS      benchmarks/structures/run.py, once per side (median of 3 in-process)
set -u
D=$(cd "$(dirname "$0")" && pwd)
WS=${PROTO_WORKSPACE:-$(cd "$D/../../../../.." && pwd)}
OUT=${OUT:-$WS/.agent_scratch/writepath-oct18/xrt}; mkdir -p $OUT
ROUNDS=${ROUNDS:-3}
Q="python3 $WS/protoCore/docs/reports/data/2026-10-12-mark-stack-int-compare/quiet.py"
# Both sides run the SAME runtime binaries (built against 2.14.6, */build_cmp2146)
# and differ only in the protoCore library they load: 2.14.7 changes no public
# header and adds one out-of-line constructor to proto_internal.h, so the
# 2.14.6-built binaries run unchanged on it.  Two separate runtime builds per
# side would add their own per-file placement difference (2026-10-19).
tree() { echo build_cmp2146; }
# Each run loads its side's protoCore from a FRESH copy of the staged library.
# The same library bytes loaded from one file instance ran 13 % slower than
# from fresh copies of it, stably across runs (2026-10-19): the placement of
# a file's text pages is a per-file lottery, so every run draws a new one.
STAGE=$WS/.agent_scratch/installers-oct2026
fresh() {   # side tag -> exports LD_LIBRARY_PATH to a fresh copy
  local v; [ $1 = before ] && v=2146 || v=2147
  local d=$OUT/libs/$1_$2_$(date +%s%N); mkdir -p $d
  cp -L $STAGE/stage$v/usr/lib/x86_64-linux-gnu/libprotoCore.so.3 $d/libprotoCore.so.3
  export LD_LIBRARY_PATH=$d
  echo "$1 $2 $d" >> $OUT/libs.log
}
stamp() { echo "$(date -u +%FT%TZ) $*"; }
NODE=$(command -v node || ls -d $HOME/.nvm/versions/node/*/bin/node | tail -1)
export PATH=$(dirname $NODE):$PATH
for r in $(seq 1 $ROUNDS); do
  for side in before after; do
    t=$(tree $side)
    $Q > /dev/null; fresh $side py_r$r; stamp "py $side r$r"
    ( cd $WS/protoPython && PROTOPY_BIN=$WS/protoPython/$t/src/runtime/protopy \
        PROTOPYC_BIN=$WS/protoPython/$t/src/compiler/protopyc RUN_MODULE_BIN=$WS/protoPython/$t/test/compiler/run_module \
        LD_LIBRARY_PATH_BENCH=$WS/protoPython/$t/src/library \
        CPYTHON_BIN=/usr/local/bin/python3.14 CPYTHONT_BIN=$HOME/.local/bin/python3.14t \
        python3 benchmarks/run_4way_interleaved.py -o $OUT/py_${side}_r$r.md > $OUT/py_${side}_r$r.log 2>&1 )
    $Q > /dev/null; fresh $side js_r$r; stamp "js $side r$r"
    ( cd $WS/protoJS && PROTOJS_BIN=$WS/protoJS/$t/protojs node tests/benchmarks/run_standard_comparison.js \
        > $OUT/js_${side}_r$r.txt 2>&1; cp tests/benchmarks/results/standard_comparison.json $OUT/js_${side}_r$r.json 2>/dev/null )
    $Q > /dev/null; fresh $side scala_r$r; stamp "scala $side r$r"
    ( cd $WS/protoScala && PROTOSCALA_BIN=$WS/protoScala/$t/protoscala PROTOSCALA_RELEASE_BIN=/nonexistent \
        PROTOPY_BIN=/nonexistent PROTOST_BIN=/nonexistent PROTOCLJ_BIN=/nonexistent \
        CPYTHON_BIN=/usr/local/bin/python3.14 SCALA_HOME=$WS/tools/scala3-dl/scala3-3.9.0-x86_64-pc-linux \
        python3 benchmarks/run_benchmarks.py --output $OUT/scala_${side}_r$r.md > $OUT/scala_${side}_r$r.log 2>&1 )
    $Q > /dev/null; fresh $side clj_r$r; stamp "clj $side r$r"
    ( cd $WS/protoClojure && bash benchmarks/bench.sh $WS/protoClojure/$t/protoclj $WS/tools/bb-1.4.192/bb \
        > $OUT/clj_${side}_r$r.txt 2>&1 )
    $Q > /dev/null; fresh $side st_r$r; stamp "st $side r$r"
    ( cd $WS/protoST && PROTOST_BIN=$WS/protoST/$t/protost PROTOPY_BIN=/nonexistent \
        CPYTHON_BIN=/usr/local/bin/python3.14 \
        python3 benchmarks/run_benchmarks.py --output $OUT/st_${side}_r$r.md > $OUT/st_${side}_r$r.log 2>&1 )
  done
done
for wl in records join doctree wordfreq graph; do
  for side in before after; do
    t=$(tree $side); $Q > /dev/null; fresh $side struct_$wl; stamp "struct $wl $side"
    ( cd $WS/protoJS/benchmarks/structures && python3 run.py --protojs $WS/protoJS/$t/protojs --reps 3 \
        --ns 1,2,4,6,12 --scale 1 --heap-cells 40000000 --memory-max 4G --only $wl \
        --out $OUT/structures_$side.jsonl >> $OUT/structures_$side.log 2>&1; echo "rc=$?" >> $OUT/structures_$side.log )
  done
done
stamp end
