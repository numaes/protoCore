#!/bin/bash
# The 2.14.2 adjustments against 2.14.1 (both rebuilt in this session; the
# runtime binaries are the re-measurement's after side), one interleaved
# session; the 2.10.2 before side of the re-measurement for the rows the
# adjustments target.
D=$ADJ
R=$D/res; mkdir -p $R
stamp() { echo "$(date -u +%FT%TZ) $* load=$(cut -d' ' -f1 /proc/loadavg)"; }
stamp start
FM_SIDES=before,ref,adj python3 $D/fm.py gc $R/gc3.jsonl --reps 3 --only 'clj_coll_t1,js40_doctree_n6,js40_wordfreq_n6,js40_records_n6' > $R/gc3.log 2>&1; stamp gc3 rc=$?
FM_SIDES=ref,adj python3 $D/fm.py gc $R/gc.jsonl --reps 3 --only 'clj_coll_t6,scala_tree_t1,scala_tree_t6,js40_join_n6,js40_graph_n6,js40_records_n12,js40_doctree_n12,js40_graph_n12,js10_records_n12,js10_doctree_n12,js10_graph_n6,core_fixed640_live1M,core_adaptive_live1M,jsad_records_n12,jsad_wordfreq_n12' > $R/gc.log 2>&1; stamp gc rc=$?
python3 $D/fm.py cal $R/cal.jsonl --reps 3 --variants ref-default,adj-default,ref-adaptive,adj-adaptive --only 'st/*,core/*,js/*,py/list_append_loop,py/sieve,py/binary_trees,scala/list_ops,scala/map_build,scala/object_tree' > $R/cal.log 2>&1; stamp cal rc=$?
FM_POLICY=ref-adaptive,adj-adaptive python3 $D/fm.py policy $R/policy.jsonl --reps 3 > $R/policy.log 2>&1; stamp policy rc=$?
FM_SIDES=ref,adj python3 $D/fm.py gc $R/cad.jsonl --reps 3 --only 'cad_20k' > $R/cad.log 2>&1; stamp cad rc=$?
stamp end
