#!/bin/bash
# st6.sh <label> <dirA> <dirB>: perf stat -r 3 of the six single-threaded benchmarks, A and B interleaved.
SP=${SP:?set SP to a scratch directory}
OUT=$SP/ct/st6_$1; mkdir -p $OUT
for b in microbenchmark_final mutable_access_benchmark cache_timing_benchmark hash_quality_benchmark object_access_benchmark immutable_sharing_benchmark; do
  for side in A B; do
    if [ $side = A ]; then d=$2; else d=$3; fi
    while ps -eo comm | grep -qE '^(cc1plus|ctest|proto_tests|ld|make)$'; do sleep 20; done
    (cd $d && perf stat -r 3 -e instructions:u,cycles:u,task-clock -x, -o $OUT/perf_${b}_$side.csv ./$b > $OUT/out_${b}_$side.txt 2>&1)
    echo "$b $side rc=$? load=$(cut -d' ' -f1 /proc/loadavg)"
  done
done
