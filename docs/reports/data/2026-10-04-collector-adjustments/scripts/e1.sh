#!/bin/bash
# e1.sh <lib> <label> [env...]: clj_coll_t1 under perf record, per-thread counts.
S=$SCRATCH
FM=$FM_ROOT
WL=$WS/protoCore/docs/reports/data/2026-10-04-final-remeasurement/workloads
LIB=$1; LABEL=$2; shift 2
OUT=$S/adj/e1_$LABEL.data
python3 $ADJ/quiet.py >/dev/null 2>&1
EV="cycles:u,instructions:u,ls_refills_from_sys.ls_mabresp_lcl_cache,ls_refills_from_sys.ls_mabresp_lcl_dram,ls_refills_from_sys.ls_mabresp_lcl_l2,task-clock"
cd $WL
env LD_LIBRARY_PATH=$LIB:$FM/new/root/usr/lib/x86_64-linux-gnu PROTOCORE_HEAP_LIMIT_CELLS=2000000 "$@" \
  /usr/bin/time -f "TIME wall=%e user=%U sys=%S rss_kb=%M" \
  perf record -q -c 200003 -o $OUT -e "$EV" -- $FM/new/root/usr/bin/protoclj coll_alloc_r1500_t1.clj > $S/adj/e1_$LABEL.out 2> $S/adj/e1_$LABEL.err
grep -q 'checksum 11250000000' $S/adj/e1_$LABEL.out && echo VERIFIED || echo '** NOT VERIFIED **'
tail -1 $S/adj/e1_$LABEL.out; grep TIME $S/adj/e1_$LABEL.err
perf script -i $OUT -F comm,event,period 2>/dev/null | python3 -c '
import sys,collections
d=collections.defaultdict(lambda: collections.Counter())
for l in sys.stdin:
    p=l.split()
    if len(p)<3: continue
    comm=p[0]; period=int(p[1]); ev=p[2].rstrip(":")
    d[comm][ev]+=period
for c,cnt in d.items():
    cyc=cnt.get("cycles:u",0); ins=cnt.get("instructions:u",0); tc=cnt.get("task-clock",0)
    print("%-16s task=%.2fs cyc=%.2fG ins=%.2fG ipc=%.2f GHz=%.2f c2c=%.1fM dram=%.1fM l2=%.1fM"%(c,tc/1e9,cyc/1e9,ins/1e9,ins/max(cyc,1),cyc/max(tc,1),
      cnt.get("ls_refills_from_sys.ls_mabresp_lcl_cache",0)/1e6,cnt.get("ls_refills_from_sys.ls_mabresp_lcl_dram",0)/1e6,cnt.get("ls_refills_from_sys.ls_mabresp_lcl_l2",0)/1e6))
'
