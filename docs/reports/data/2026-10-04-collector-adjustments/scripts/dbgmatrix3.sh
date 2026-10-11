#!/bin/bash
# Quick diagnosis: one run per variant, gated.  DBG_WIDE: 1 always wide, 2 waiters only, unset = about-to-wait rule.
S=$SCRATCH
FM=$FM_ROOT
WL=$WS/protoCore/docs/reports/data/2026-10-04-final-remeasurement/workloads
JS=$WS/protoJS/benchmarks/structures
run() { # label lib env... -- cmd
  local label=$1 lib=$2; shift 2
  python3 $S/adj/quiet.py > /dev/null 2>&1
  local out=$(env LD_LIBRARY_PATH=$lib:$FM/new/root/usr/lib/x86_64-linux-gnu PROTOCORE_HEAP_TRACE=1 /usr/bin/time -f 'TIME wall=%e user=%U' env "$@" 2>&1)
  local cyc=$(echo "$out" | grep -c 'protoCore heap')
  local ok=$(echo "$out" | grep -cE 'checksum 11250000000|"ok":true')
  echo "$label ok=$ok cycles=$cyc $(echo "$out" | grep TIME)"
}
for rep in 1 2 3; do
for v in ref p-old p-zero p-lead; do
  case $v in
    ref) L=$S/adj/lib_base_rel; E="";;
    p-old) L=$S/adj/lib_dbg; E="DBG_PACING_OLD=1";;
    p-zero) L=$S/adj/lib_dbg; E="DBG_PACING_OLD=2";;
    p-lead) L=$S/adj/lib_dbg; E="";;
  esac
  cd $WL; run "clj_t1 $v" $L $E PROTOCORE_HEAP_LIMIT_CELLS=2000000 $FM/new/root/usr/bin/protoclj coll_alloc_r1500_t1.clj
  cd $WL; run "clj_t6 $v" $L $E PROTOCORE_HEAP_LIMIT_CELLS=2000000 $FM/new/root/usr/bin/protoclj coll_alloc_r1500_t6.clj
  cd $JS; run "js40_records_n6 $v" $L $E WORKLOAD=records MODE=par N=6 REPS=3 SCALE=1 PROTOCORE_HEAP_LIMIT_CELLS=40000000 $FM/new/root/usr/bin/protojs protojs_bench.js
  cd $JS; run "js40_doctree_n6 $v" $L $E WORKLOAD=doctree MODE=par N=6 REPS=3 SCALE=1 PROTOCORE_HEAP_LIMIT_CELLS=40000000 $FM/new/root/usr/bin/protojs protojs_bench.js
  cd $JS; run "js40_wordfreq_n6 $v" $L $E WORKLOAD=wordfreq MODE=par N=6 REPS=3 SCALE=1 PROTOCORE_HEAP_LIMIT_CELLS=40000000 $FM/new/root/usr/bin/protojs protojs_bench.js
done
done
