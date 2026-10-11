#!/bin/bash
# place.sh <lib> <reps>: clj_coll_t1 pinned to two CPUs (same core / same CCX / other CCX), 8 vs 1 chains.
S=$SCRATCH
FM=$FM_ROOT
WL=$WS/protoCore/docs/reports/data/2026-10-04-final-remeasurement/workloads
LIB=$1; REPS=${2:-1}; shift 2
cd $WL
for rep in $(seq $REPS); do
 for cpus in 0,1 2,4 2,8 any; do
  for c in 8 1; do
   python3 $S/adj/quiet.py > /dev/null 2>&1
   if [ $cpus = any ]; then T=""; else T="taskset -c $cpus"; fi
   out=$(env LD_LIBRARY_PATH=$LIB:$FM/new/root/usr/lib/x86_64-linux-gnu PROTOCORE_HEAP_LIMIT_CELLS=2000000 PROTOCORE_GC_SWEEP_CURSORS=$c "$@" \
     /usr/bin/time -f "TIME wall=%e user=%U sys=%S" $T $FM/new/root/usr/bin/protoclj coll_alloc_r1500_t1.clj 2>&1)
   ok=NO; echo "$out" | grep -q 'checksum 11250000000' && ok=yes
   echo "rep=$rep cpus=$cpus cursors=$c ok=$ok $(echo "$out" | grep TIME)"
  done
 done
done
