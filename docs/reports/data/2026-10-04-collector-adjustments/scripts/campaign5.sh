#!/bin/bash
D=$ADJ
R=$D/res5
stamp() { echo "$(date -u +%FT%TZ) $* load=$(cut -d' ' -f1 /proc/loadavg)"; }
stamp campaign5
FM_SIDES=ref,adj python3 $D/fm.py gc $R/gc.jsonl --reps 3 --only 'clj_coll_t1,js40_doctree_n6,js40_wordfreq_n6,js40_records_n6,clj_coll_t6,scala_tree_t6,js40_join_n6,js40_graph_n6,js40_records_n12,js40_doctree_n12,js40_graph_n12,js10_records_n12,js10_doctree_n12,js10_graph_n6,jsad_records_n12,jsad_wordfreq_n12,core_fixed640_live1M,core_adaptive_live1M,scala_tree_t1' > $R/gc.log 2>&1; stamp gc rc=$?
stamp end
