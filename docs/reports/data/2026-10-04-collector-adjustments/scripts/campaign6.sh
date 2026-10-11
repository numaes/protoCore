#!/bin/bash
D=$ADJ
R=$D/res5
stamp() { echo "$(date -u +%FT%TZ) $* load=$(cut -d' ' -f1 /proc/loadavg)"; }
while ps -eo args | grep -v grep | grep -q 'adj/campaign5.sh'; do sleep 20; done
stamp campaign6
python3 $D/fm.py cal $R/cal.jsonl --reps 3 --variants ref-adaptive,adj-adaptive --only 'st/*,core/*,py/list_append_loop,py/sieve,py/binary_trees,scala/list_ops,scala/map_build,scala/object_tree,js/probe-100k' > $R/cal.log 2>&1; stamp cal rc=$?
python3 $D/fm.py cal $R/cal.jsonl --reps 3 --variants ref-default,adj-default --only 'core/*,st/fib,st/saturation_big' >> $R/cal.log 2>&1; stamp cal-default rc=$?
stamp end
