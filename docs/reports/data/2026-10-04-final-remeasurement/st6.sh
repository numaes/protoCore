#!/bin/bash
# st6.sh <outdir> <dirA> <dirB>: perf stat -r 3 of protoCore's six single-threaded
# benchmarks, A (2.10.2, build_pkg_oct03) and B (2.14.1, build_pkg_oct04)
# interleaved, three rounds.  Each round waits for a quiet machine (no compiler
# or test suite, 1-minute load below MAX_LOAD) and records the load.
OUT=${1:?outdir}; A=${2:?dirA}; B=${3:?dirB}; mkdir -p $OUT
MAX_LOAD=${MAX_LOAD:-4.0}
export LC_ALL=C
for round in 1 2 3; do
  for b in microbenchmark_final mutable_access_benchmark cache_timing_benchmark hash_quality_benchmark object_access_benchmark immutable_sharing_benchmark; do
    for side in A B; do
      if [ $side = A ]; then d=$A; else d=$B; fi
      while ps -eo comm | grep -qE '^(cc1plus|ctest|proto_tests|ld|make|ninja|cmake|cpack)$' || \
            awk -v m=$MAX_LOAD '{exit !($1 > m)}' /proc/loadavg; do sleep 20; done
      load=$(cut -d' ' -f1 /proc/loadavg)
      (cd $d && LD_LIBRARY_PATH=$d perf stat -r 3 -e instructions:u,cycles:u,task-clock -x, \
          -o $OUT/perf_${b}_${side}_r$round.csv ./$b > $OUT/out_${b}_${side}_r$round.txt 2>&1)
      rc=$?
      echo "$b $side round=$round rc=$rc load=$load lib=$(LD_LIBRARY_PATH=$d ldd $d/$b | grep -o 'libprotoCore.so.3 => [^ ]*')"
    done
  done
done
