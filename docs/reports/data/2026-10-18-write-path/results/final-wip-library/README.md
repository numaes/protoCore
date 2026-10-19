# Summary run on the wrong library (superseded)

`final_table.sh` rebuilt `attribute_lookup_benchmark` and libprotoCore in
`build_pkg_oct18`, a build tree configured on the protoCore working copy,
which at that moment held the uncommitted changes of the
`perf/context-slot-stack` branch.  These numbers are therefore 2.14.7 plus
those changes, not 2.14.7.  The report's summary table comes from `../final/`,
run on a build of a clean checkout of master (`f2d8eb5c`, `ldd.txt`).
