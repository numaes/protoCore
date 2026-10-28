#!/bin/bash
# Runtime A/B/A after the microbenchmark sweep and the branch rebuilds.
S=/home/gamarino/Documentos/proyectos/.agent_scratch; W=$S/pc-attr-list-work; L=$S/build.lock
P=$W/probes; O=$W/results; mkdir -p $O
while pgrep -f "bench/sweep.sh|rebuild_branch_rt.sh" > /dev/null; do sleep 30; done
for rt in protoJS protoPython protoClojure; do for s in base branch; do
  echo "$rt $s $(ldd $W/rt/$rt/build-$s/$( [ $rt = protoJS ] && echo protojs || ([ $rt = protoPython ] && echo src/runtime/protopy || echo protoclj)) | grep protoCore)"; done; done > $O/ldd.txt
# 1. Order sensitivity: branch-side ctest with other list ceilings.
for t in 64 2; do for rt in protoPython protoClojure protoJS; do
  j=4; [ $rt = protoJS ] && j=2
  cd $W/rt/$rt/build-branch && PROTOCORE_ATTR_LIST_MAX=$t timeout 5400 flock $L ctest -j$j --timeout 900 > $O/ctest-$rt-branch-T$t.log 2>&1
  echo "ctest $rt T=$t rc=$? $(grep 'tests passed' $O/ctest-$rt-branch-T$t.log)"
done; done
# Also the default ceiling again on the rebuilt binaries.
for rt in protoPython protoClojure protoJS; do
  j=4; [ $rt = protoJS ] && j=2
  cd $W/rt/$rt/build-branch && timeout 5400 flock $L ctest -j$j --timeout 900 > $O/ctest-$rt-branch-T8-rebuilt.log 2>&1
  echo "ctest $rt T=8 rebuilt rc=$? $(grep 'tests passed' $O/ctest-$rt-branch-T8-rebuilt.log)"
done
cd $P
# 2. protoJS per-op harness.
python3 abperf.py perop $O/js-perop.jsonl js closurecall concat fieldread fieldreadloc fieldwrite globalcall globalread globalwrite idxread keyread keyreadloc localcall loop loopacc method push > $O/js-perop.txt 2>&1
# 3. Write/read probes.
python3 abperf.py probes $O/probes.jsonl py py_base.py py_write.py py_write8.py py_new.py > $O/probes.txt 2>&1
python3 abperf.py probes $O/probes.jsonl js js_base.js js_write.js js_write8.js js_new.js >> $O/probes.txt 2>&1
python3 abperf.py probes $O/probes.jsonl clj clj_base.clj clj_write.clj >> $O/probes.txt 2>&1
# 4-6. Standard benchmarks (whole program, perf stat -r 3).
JS=$W/rt/protoJS/tests/benchmarks/standard
ABPERF_R=3 python3 abperf.py probes $O/std.jsonl js $(ls $JS/*.js | grep -v parallel) > $O/js-std.txt 2>&1
PY=$W/rt/protoPython/benchmarks
ABPERF_R=3 python3 abperf.py probes $O/std.jsonl py $PY/int_sum_loop.py $PY/list_append_loop.py $PY/str_concat_loop.py $PY/range_iterate.py $PY/attr_lookup.py $PY/call_recursion.py $PY/memory_pressure.py $PY/pyperf/bench_fib.py $PY/pyperf/bench_binary_trees.py $PY/pyperf/bench_nqueens.py $PY/pyperf/bench_richards_lite.py $PY/pyperf/bench_sieve.py > $O/py-std.txt 2>&1
CL=$W/rt/protoClojure/benchmarks
ABPERF_R=3 python3 abperf.py probes $O/std.jsonl clj $CL/fib.clj $CL/tak.clj $CL/sum-loop.clj $CL/reduce-list.clj $CL/sum-squares.clj $CL/factorial-100.clj > $O/clj-std.txt 2>&1
# 7. protoJS structure benchmarks, sequential and parallel, N = 1.
cd $W/rt/protoJS
for b in base branch base; do
  timeout 7200 flock $L python3 benchmarks/structures/run.py --protojs $W/rt/protoJS/build-$b/protojs --ns 1 --reps 3 --out $O/struct-$b-$RANDOM.json > $O/struct-$b-$(date +%s).log 2>&1; echo "struct $b rc=$?"
done
echo ALL DONE
