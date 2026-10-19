#!/usr/bin/env bash
# Runs real workloads of each runtime against the PROTO_CACHE_STATS build of
# protoCore and prints the attribute-cache statistics of each run.
set -u
S=/tmp/claude-1000/-home-gamarino-Documentos-proyectos/9cb98613-5533-4787-ae0f-b301a4c38554/scratchpad
P=/home/gamarino/Documentos/proyectos
export LD_LIBRARY_PATH=$S/bld_stats
OUT=$S/hitrate_results.txt
: > "$OUT"

run() {   # name cwd cmd...
    local name=$1 cwd=$2; shift 2
    local lib
    lib=$(ldd "$(command -v "$1")" | awk '/libprotoCore/{print $3}')
    [[ "$lib" == "$S/bld_stats/"* ]] || { echo "$name: WRONG LIB $lib" | tee -a "$OUT"; return; }
    local log=$S/log_${name//[^A-Za-z0-9_]/_}.txt
    ( cd "$cwd" && timeout 600 "$@" ) > "$log" 2>&1
    local rc=$?
    {
        echo "=== $name (exit $rc)"
        grep -v "proto-cache-stats" "$log" | tail -3 | sed 's/^/    out: /'
        grep "proto-cache-stats" "$log"
    } | tee -a "$OUT"
}

# protoPython: pyperformance-style subset
for b in "bench_richards_lite.py 2000" "bench_binary_trees.py" "bench_nqueens.py 10" \
         "bench_fannkuch.py" "bench_fib.py" "bench_mandelbrot.py"; do
    set -- $b
    run "py:$1" $P/protoPython protopy --path benchmarks --script benchmarks/pyperf/$1 "${@:2}"
done

# protoJS: structure workloads (ordinary JS objects, sequential, one task)
for w in records join doctree wordfreq graph; do
    WORKLOAD=$w MODE=seq N=1 REPS=1 PROTOCORE_HEAP_LIMIT_CELLS=40000000 \
        run "js:$w" $P/protoJS/benchmarks/structures protojs protojs_bench.js
done

# protoST: example programs and comparable workloads
for f in examples/programs/json_transform.st examples/programs/traffic_intersection.st \
         examples/programs/rpn_interpreter.st examples/programs/calculator.st \
         benchmarks/comparable/attr_lookup.st benchmarks/comparable/list_append.st; do
    run "st:$(basename $f)" $P/protoST protost $f
done

# protoScala: comparable workloads
for f in object_tree map_build list_ops attr_lookup tak; do
    run "scala:$f" $P/protoScala protoscala benchmarks/comparable/$f.scala
done

# protoClojure
for f in tak reduce-list fib; do
    run "clj:$f" $P/protoClojure protoclj benchmarks/$f.clj
done
