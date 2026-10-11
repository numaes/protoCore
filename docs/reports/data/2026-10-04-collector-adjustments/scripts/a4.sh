#!/bin/bash
# a4.sh: immutable_sharing_benchmark; 2.10.2 and 2.14.1 (rebuilt, byte-identical
# to the packages), each also with -falign-functions=64, and the branch.
# perf stat -r 5, interleaved, 3 rounds; then IBS profiles of 2.10.2 and 2.14.1.
S=$SCRATCH
OUT=$S/adj/a4/res; mkdir -p $OUT
export LC_ALL=C
EV1=instructions:u,cycles:u,task-clock,branch-misses:u,ic_fetch_stall.ic_stall_any,ic_fetch_stall.ic_stall_back_pressure
EV2=L1-dcache-load-misses,dTLB-load-misses,ls_refills_from_sys.ls_mabresp_lcl_dram,ic_oc_mode_switch.ic_oc_mode_switch,ic_fw32,ic_fw32_miss
for round in 1 2 3; do
  for v in b2102 b2141 b2102al b2141al adj; do
    if [ $v = adj ]; then d=$S/adj/lib_adj_rel; b=$d/immutable_sharing_benchmark; else d=$S/adj/a4/$v; b=$d/immutable_sharing_benchmark; fi
    python3 $S/adj/quiet.py > /dev/null 2>&1
    (cd $d && LD_LIBRARY_PATH=$d perf stat -r 5 -x, -e $EV1 -o $OUT/s1_${v}_r$round.csv $b > $OUT/out_${v}_r$round.txt 2>&1)
    (cd $d && LD_LIBRARY_PATH=$d perf stat -r 5 -x, -e $EV2 -o $OUT/s2_${v}_r$round.csv $b > /dev/null 2>&1)
    echo "$v round=$round $(LD_LIBRARY_PATH=$d ldd $b | grep -o 'libprotoCore.so.3 => [^ ]*') $(grep -c 'Checksum mismatch' $OUT/out_${v}_r$round.txt) mismatches"
  done
done
for v in b2102 b2141; do
  d=$S/adj/a4/$v
  python3 $S/adj/quiet.py > /dev/null 2>&1
  (cd $d && LD_LIBRARY_PATH=$d perf record -q -e ibs_op// -c 100000 -o $OUT/ibs_$v.data ./immutable_sharing_benchmark > /dev/null 2>&1)
done
echo done
