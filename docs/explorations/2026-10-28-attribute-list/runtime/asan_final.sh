#!/bin/bash
S=/home/gamarino/Documentos/proyectos/.agent_scratch; W=$S/pc-attr-list-work; L=$S/build.lock
flock $L cmake --build $W/build-asan -j8 > $W/logs/asan-build.log 2>&1; echo "asan build rc=$?"
cd $W/build-asan
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0
timeout 10800 flock $L ctest -j6 --timeout 1800 > $W/logs/asan-full-T8.log 2>&1; echo "asan full T=8 rc=$? $(grep 'tests passed' $W/logs/asan-full-T8.log)"
for t in 2 64; do PROTOCORE_ATTR_LIST_MAX=$t timeout 3600 flock $L ctest -j6 --timeout 1800 -R "AttributeList|SetAttributes|Object|Mutable" > $W/logs/asan-T$t.log 2>&1; echo "asan T=$t rc=$? $(grep 'tests passed' $W/logs/asan-T$t.log)"; done
grep -l "ERROR: AddressSanitizer\|ERROR: LeakSanitizer" $W/build-asan/Testing/Temporary/LastTest*.log 2>/dev/null; grep -c "ERROR: AddressSanitizer\|ERROR: LeakSanitizer" $W/logs/asan-*.log
echo ASAN DONE
