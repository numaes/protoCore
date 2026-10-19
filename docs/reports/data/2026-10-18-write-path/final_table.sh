#!/bin/bash
# The summary table of 2026-10-18: protoCore 2.14.7 against native C++, the
# JVM's mutable objects, immutable static records and immutable dynamic maps.
# Same shapes (4 and 16 attributes), objects, random order and checksums.
#   protoCore + C++   performance/attribute_lookup_benchmark (sweep: reads; upd:
#                     copy-update without recycling, and C++ in-place update)
#   protoCore hot copy with recycling: attr-study/hotcopy_batched.cpp
#                     (a child context per 1,000 copies, PROTOCORE_ADAPTIVE_HEAP=1)
#   JVM               attr-study/jvm/ImmutableBench.java (Java 25; Clojure 1.12.0,
#                     Scala 2.13.15 library jars from Maven Central, sha1-checked)
set -u
D=$(cd "$(dirname "$0")" && pwd)
WS=$(cd "$D/../../../../.." && pwd)
OUT=${OUT:-$WS/.agent_scratch/writepath-oct18/final}; mkdir -p $OUT
CORE=$WS/protoCore/build_pkg_oct18
JARS=${JARS:?set JARS to the directory holding the four jars}
Q="python3 $WS/protoCore/docs/reports/data/2026-10-12-mark-stack-int-compare/quiet.py"
stamp() { echo "$(date -u +%FT%TZ) $* load=$(cut -d' ' -f1 /proc/loadavg)"; }
cmake --build $CORE --target attribute_lookup_benchmark -j4 > $OUT/build.log 2>&1
g++ -std=c++20 -O2 -I$WS/protoCore/headers $D/attr-study/hotcopy_batched.cpp -L$CORE -lprotoCore \
    -Wl,-rpath,$CORE -o $OUT/hotcopy_batched
ldd $CORE/attribute_lookup_benchmark $OUT/hotcopy_batched | grep libprotoCore > $OUT/ldd.txt
CP=$(ls $JARS/*.jar | tr '\n' ':')
javac -cp "$CP" -d $OUT/jvm $D/attr-study/jvm/ImmutableBench.java 2> $OUT/javac.log
# Three fresh copies of the library, one per repetition (see cross_run.sh,
# fresh(): the placement of a library file's pages is a per-file lottery).
for k in 1 2 3; do
  L=$OUT/libs/k$k; mkdir -p $L; cp -L $CORE/libprotoCore.so.3 $L/
  export LD_LIBRARY_PATH=$L
  $Q > /dev/null; stamp sweep$k; $CORE/attribute_lookup_benchmark sweep > $OUT/proto_sweep_$k.txt 2>&1
  $Q > /dev/null; stamp own$k;   $CORE/attribute_lookup_benchmark own > $OUT/proto_own_$k.txt 2>&1
  $Q > /dev/null; stamp upd$k;   $CORE/attribute_lookup_benchmark upd > $OUT/proto_upd_$k.txt 2>&1
  for n in 4 16; do
    $Q > /dev/null; stamp hot$n.$k
    PROTOCORE_ADAPTIVE_HEAP=1 $OUT/hotcopy_batched $n 2000000 1000 > $OUT/proto_hotcopy_${n}_$k.txt 2>&1
  done
  unset LD_LIBRARY_PATH
done
$Q > /dev/null; stamp jvm
java -Xms2g -Xmx2g -cp "$OUT/jvm:$CP" ImmutableBench > $OUT/jvm.txt 2>&1
stamp end
