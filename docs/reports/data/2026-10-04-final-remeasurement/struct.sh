#!/bin/bash
# struct.sh <outdir>: protoJS's structure benchmarks (benchmarks/structures/run.py,
# the configuration of protoJS's 2026-10-03 report: scale 1, N = 1,2,4,6,12,
# REPS = 3 in each process, a 40 M-cell heap limit, a 4 GB cap) for the BEFORE
# binary (2026-10-03 package on protoCore 2.10.2) and the AFTER binary
# (2026-10-04 package on protoCore 2.14.1), workload by workload, interleaved.
# run.py verifies every checksum against Node's sequential run and stops on a
# mismatch.  Release libraries (not instrumented).
OUT=${1:?outdir}; mkdir -p $OUT
WS=${PROTO_WORKSPACE:-$(cd "$(dirname "$0")/../../../../.." && pwd)}
ROOT=${FM_ROOT:-$WS/.agent_scratch/final-oct04}
cd $WS/protoJS/benchmarks/structures
for wl in records join doctree wordfreq graph; do
  for side in before after; do
    if [ $side = before ]; then bin=$ROOT/base/root/usr/bin/protojs; lib=$ROOT/base/lib210;
    else bin=$ROOT/new/root/usr/bin/protojs; lib=$ROOT/new/lib2141; fi
    q=$(python3 $(dirname "$0")/quiet.py)   # waits for a quiet machine (fm.quiet_gate)
    echo "== $wl $side quiet=$q lib=$(LD_LIBRARY_PATH=$lib ldd $bin | grep -o 'libprotoCore.so.3 => [^ ]*')"
    LD_LIBRARY_PATH=$lib python3 run.py --protojs $bin --reps 3 --ns 1,2,4,6,12 --scale 1 \
        --heap-cells 40000000 --memory-max 4G --only $wl --out $OUT/structures_$side.jsonl
    echo "rc=$?"
  done
done
