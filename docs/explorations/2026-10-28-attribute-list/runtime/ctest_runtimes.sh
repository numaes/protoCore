#!/bin/bash
S=/home/gamarino/Documentos/proyectos/.agent_scratch; W=$S/pc-attr-list-work; L=$S/build.lock
for rt in protoPython protoClojure protoJS; do
  j=4; [ $rt = protoJS ] && j=2
  for side in base branch; do
    cd $W/rt/$rt/build-$side
    timeout 5400 flock $L ctest -j$j --timeout 900 > $W/logs/ctest-$rt-$side.log 2>&1
    echo "$rt $side ctest rc=$? $(grep 'tests passed' $W/logs/ctest-$rt-$side.log)"
  done
done
