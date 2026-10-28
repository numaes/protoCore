#!/bin/bash
S=/home/gamarino/Documentos/proyectos/.agent_scratch; W=$S/pc-attr-list-work; L=$S/build.lock
B=$W/rt/protoJS/tests/benchmarks/standard/string_insert_middle.js
for i in $(seq 1 12); do
  for arm in base branch branchT0; do
    side=${arm%T0}; env=""; [ $arm = branchT0 ] && env="PROTOCORE_ATTR_LIST_MAX=0"
    d=$W/libcopies/sim-$arm-$i; mkdir -p $d; cp $W/prefix-$side/lib/libprotoCore.so.4 $d/
    v=$(env $env LD_LIBRARY_PATH=$d flock $L perf stat -x, -e instructions:u,cycles:u $W/rt/protoJS/build-$side/protojs $B 2>&1 >/dev/null | cut -d, -f1 | tr '\n' ' ')
    echo "$arm $i $v"
  done
done
echo SIM DONE
