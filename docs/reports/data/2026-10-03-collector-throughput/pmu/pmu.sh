#!/bin/bash
# pmu.sh <lib> <label> -- per-thread PMU counters of clj_coll_t6 (R=500); the
# collector is the thread with the most cycles besides the main thread.
cd "${SCRATCH:-$(dirname "$0")}"
LIB=$PWD/$1; LABEL=$2
while ps -eo comm | grep -qE '^(cc1plus|ctest|proto_tests|ld|make)$'; do sleep 20; done
EV=cycles,instructions,ls_refills_from_sys.ls_mabresp_lcl_l2,ls_refills_from_sys.ls_mabresp_lcl_cache,ls_refills_from_sys.ls_mabresp_lcl_dram,ls_refills_from_sys.ls_mabresp_rmt_cache
LD_LIBRARY_PATH=$LIB PROTOCORE_HEAP_LIMIT_CELLS=2000000 PROTOCORE_GC_PROFILE=1 ../bin/protoclj wl/coll_alloc_r500_t6.clj > pmu_$LABEL.out 2> pmu_$LABEL.err &
PID=$!
sleep 0.3
perf stat -p $PID --per-thread -x, -e $EV -o pmu_$LABEL.csv 2>/dev/null
wait $PID
echo "exit=$? $(tail -1 pmu_$LABEL.out)"
