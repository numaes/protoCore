#!/bin/bash
S=/home/gamarino/Documentos/proyectos/.agent_scratch; W=$S/pc-attr-list-work; L=$S/build.lock; O=$W/results; P=$W/probes
until grep -q "SCALA DONE" $W/logs/build_scala.out && grep -q "SIM DONE" $O/sim_dist.txt; do sleep 20; done
for s in base branch; do echo "protoScala $s $(ldd $W/rt/protoScala/build-$s/protoscala | grep protoCore)"; done >> $O/ldd.txt
SC=$W/rt/protoScala/benchmarks/comparable
cd $P && python3 abperf.py probes $O/scala.jsonl scala $SC/object_tree.scala $SC/case_copy.scala $SC/map_build.scala $SC/list_ops.scala $SC/attr_lookup.scala $SC/tak.scala $SC/fib30.scala $SC/sum_loop.scala $SC/factorial_100.scala $SC/int_sum_loop.scala $SC/fib.scala $SC/str_concat.scala $SC/range_iterate.scala > $O/scala-comparable.txt 2>&1
echo "scala comparable done"
cd $P && python3 bbcmp.py $O/bbcmp.jsonl > $O/bbcmp.txt 2>&1; echo "bbcmp rc=$?"
for s in base branch base branch; do
  d=$W/libcopies/benchsh-$s-$(date +%s%N); mkdir -p $d; cp $W/prefix-$s/lib/libprotoCore.so.4 $d/
  echo "== protoclj $s ($(LD_LIBRARY_PATH=$d ldd $W/rt/protoClojure/build-$s/protoclj | grep protoCore | awk '{print $3}'))" >> $O/benchsh.txt
  LD_LIBRARY_PATH=$d flock $L $W/rt/protoClojure/benchmarks/bench.sh $W/rt/protoClojure/build-$s/protoclj $S/list-cost/bb-dl/bb >> $O/benchsh.txt 2>&1; echo "benchsh $s rc=$?"
done
echo "SCALA BB DONE"
