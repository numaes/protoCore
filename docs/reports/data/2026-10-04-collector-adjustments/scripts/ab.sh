#!/bin/bash
cd $WS/protoJS/benchmarks/structures
for rep in 1 2 3; do for L in lib_base_inst lib_adj_inst lib_hoist_inst; do
  python3 $ADJ/quiet.py > /dev/null 2>&1
  out=$(env LD_LIBRARY_PATH=$ADJ/$L:$FM_ROOT/new/root/usr/lib/x86_64-linux-gnu WORKLOAD=records MODE=par N=12 REPS=3 SCALE=1 PROTOCORE_HEAP_LIMIT_CELLS=40000000 /usr/bin/time -f 'TIME wall=%e' $FM_ROOT/new/root/usr/bin/protojs protojs_bench.js 2>&1)
  echo "$L rep=$rep ok=$(echo "$out" | grep -c '"ok":true') $(echo "$out" | grep TIME)"
done; done
