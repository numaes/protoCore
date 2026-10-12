#!/bin/bash
# perf stat -r 5 (cycles, instructions) of single workloads, arms interleaved:
# before (built against 2.14.5), after (2.14.6), before again (A/A).
# usage: perfcheck.sh <out.txt>
WS=$(cd "$(dirname "$0")/../../../../.." && pwd); OUT=${1:?out}
tree() { [ $1 = after ] && echo build_pkg_oct12 || echo build_cmp2145; }
run() {  # label cwd binary-relative-path args...
  local label=$1 cwd=$2 rel=$3; shift 3
  for arm in before after aa; do
    t=$(tree $arm)
    s=$(cd $cwd && perf stat -r 5 -x, -e cycles,instructions $WS/${rel/TREE/$t} "$@" 2>&1 >/tmp/perfcheck.$$ | awk -F, '/cycles|instructions/{printf "%s=%s ", $3, $1}')
    echo "$label $arm $s out=$(tail -1 /tmp/perfcheck.$$ | cut -c1-60)" | tee -a $OUT
  done
}
P=$WS/protoPython/benchmarks
run py/list_append_loop $P protoPython/TREE/src/runtime/protopy list_append_loop.py
run py/str_concat_loop $P protoPython/TREE/src/runtime/protopy str_concat_loop.py
run py/richards_lite $P protoPython/TREE/src/runtime/protopy pyperf/bench_richards_lite.py
run py/call_recursion $P protoPython/TREE/src/runtime/protopy call_recursion.py
S=$WS/protoScala/benchmarks/comparable
run sc/object_tree $S protoScala/TREE/protoscala object_tree.scala
run sc/list_ops $S protoScala/TREE/protoscala list_ops.scala
run sc/fib $S protoScala/TREE/protoscala fib.scala
J=$WS/protoJS/tests/benchmarks/standard
run js/control_flow $J protoJS/TREE/protojs control_flow.js
run js/object_property $J protoJS/TREE/protojs object_property.js
