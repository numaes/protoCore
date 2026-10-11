#!/bin/bash
# Four builds of immutable_sharing_benchmark: 2.10.2 and 2.14.1, default
# Release flags and with every function aligned to 64 bytes.
set -e
for v in 2102:$SCRATCH/pc-2102 2141:$SCRATCH/pc-ct-m1_unused; do :; done
b() { # src dir flags
  cmake -S $1 -B $2 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="$3" > $2.cfg.log 2>&1
  cmake --build $2 -j4 --target immutable_sharing_benchmark > $2.build.log 2>&1
  echo "built $2"
}
REF=$ADJ/pc-ref
[ -d $REF ] || git -C $WS/protoCore worktree add --detach $REF 1365c3af > /dev/null 2>&1
b $SCRATCH/pc-2102 $ADJ/a4/b2102 ""
b $REF $ADJ/a4/b2141 ""
b $SCRATCH/pc-2102 $ADJ/a4/b2102al "-falign-functions=64"
b $REF $ADJ/a4/b2141al "-falign-functions=64"
