#!/bin/bash
D=$ADJ
R=$D/res
stamp() { echo "$(date -u +%FT%TZ) $* load=$(cut -d' ' -f1 /proc/loadavg)"; }
stamp campaign4
python3 $D/fm.py cal $R/cal.jsonl --reps 3 --variants ref-adaptive,adj-adaptive --only 'st/*,core/*' > $R/cal.log 2>&1; stamp cal-a3 rc=$?
python3 $D/fm.py cal $R/cal.jsonl --reps 3 --variants ref-default,adj-default --only 'core/*,st/fib,st/saturation_big' >> $R/cal.log 2>&1; stamp cal-default rc=$?
python3 $D/fm.py cal $R/cal.jsonl --reps 3 --variants ref-adaptive,adj-adaptive --only 'py/list_append_loop,py/sieve,py/binary_trees,scala/list_ops,scala/map_build,scala/object_tree,js/probe-100k' >> $R/cal.log 2>&1; stamp cal-rest rc=$?
FM_SIDES=ref,adj python3 $D/fm.py gc $R/gc_rep3.jsonl --reps 1 --only 'scala_tree_t1,scala_tree_t6,clj_coll_t6,jsad_wordfreq_n12,core_fixed640_live1M,core_adaptive_live1M,js40_records_n12,js40_join_n6,js40_graph_n6,js40_doctree_n12,js40_graph_n12,js10_records_n12,js10_doctree_n12,js10_graph_n6,jsad_records_n12' > $R/gc_rep3.log 2>&1; stamp gc-rep3 rc=$?
stamp end
