#!/bin/bash
# A/B sweep of attrbench: master vs branch at several list ceilings.
# Each run loads a FRESH copy of its libprotoCore (LD_LIBRARY_PATH, checked
# with ldd); configurations alternate inside each round; three rounds for
# time and cells (counted per operation).  Heap: adaptive controller with a
# 60 M-cell hard limit (the default heap never collects in these loops).
S=/home/gamarino/Documentos/proyectos/.agent_scratch; W=$S/pc-attr-list-work; L=$S/build.lock
OUT=$W/bench/sweep; mkdir -p $OUT $W/libcopies
CONFIGS="master br0 br4 br8 br12 br16 br24 br32 br8np br16np"
run() { # config round mode
  local cfg=$1 r=$2 mode=$3 lib env=""
  if [ $cfg = master ]; then lib=$W/base/build/libprotoCore.so.4; else lib=$W/build-branch/libprotoCore.so.4; fi
  case $cfg in br*np) t=${cfg#br}; t=${t%np}; env="PROTOCORE_ATTR_LIST_MAX=$t PROTOCORE_ATTR_LIST_PACK=0";; br*) env="PROTOCORE_ATTR_LIST_MAX=${cfg#br}";; esac
  local d=$W/libcopies/v2-$cfg-$r-$mode-$(date +%s%N); mkdir -p $d; cp $lib $d/libprotoCore.so.4
 
  LD_LIBRARY_PATH=$d ldd $W/bench/attrbench | grep protoCore > $OUT/$cfg-$r-$mode.ldd
  : > $OUT/$cfg-$r-$mode.txt
  for n in 1 2 4 8 12 16 24 32 64 100; do   # one process per n: a clean mutable table
    env $env PROTOCORE_ADAPTIVE_HEAP=1 PROTOCORE_HEAP_LIMIT_CELLS=60000000 LD_LIBRARY_PATH=$d flock $L $W/bench/attrbench $n >> $OUT/$cfg-$r-$mode.txt 2>&1
    echo "$cfg $r $mode n=$n rc=$?" >> $OUT/rc.log
  done
  echo "$cfg $r $mode rc=done $(grep -c 'RESULT OK' $OUT/$cfg-$r-$mode.txt)/10 OK"
}
for r in 1 2 3; do
  if [ $r = 2 ]; then order=$(echo $CONFIGS | tr ' ' '\n' | tac | tr '\n' ' '); else order=$CONFIGS; fi
  for c in $order; do run $c $r time; done
done
