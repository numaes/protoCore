#!/bin/bash
# e2.sh <lib> <label> [env...]: clj_coll_t1 under IBS op sampling (precise IPs).
S=$SCRATCH
FM=$FM_ROOT
WL=$WS/protoCore/docs/reports/data/2026-10-04-final-remeasurement/workloads
LIB=$1; LABEL=$2; shift 2
OUT=$S/adj/e2_$LABEL.data
python3 $S/adj/quiet.py >/dev/null 2>&1
cd $WL
env LD_LIBRARY_PATH=$LIB:$FM/new/root/usr/lib/x86_64-linux-gnu PROTOCORE_HEAP_LIMIT_CELLS=2000000 "$@" \
  /usr/bin/time -f "TIME wall=%e user=%U sys=%S" \
  perf record -q -e ibs_op// -c 400000 -o $OUT -- $FM/new/root/usr/bin/protoclj coll_alloc_r1500_t1.clj > $S/adj/e2_$LABEL.out 2> $S/adj/e2_$LABEL.err
grep -q 'checksum 11250000000' $S/adj/e2_$LABEL.out && echo VERIFIED || echo '** NOT VERIFIED **'
grep TIME $S/adj/e2_$LABEL.err
