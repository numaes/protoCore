#!/usr/bin/env python3
"""A/B of the runtime-side fixes: each runtime's main build (A,
build_pkg_oct18) against its fix branch's build (B, build_fix), both built
against protoCore 2.14.7 and run on a fresh copy of the same 2.14.7 library
per run.  Arms A, B, A again; perf stat -r 5; outputs checked.

usage: ab_runtime.py <libprotoCore.so> <results.jsonl> [rounds]
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ab  # fresh(), fm, WS, PROBES

CASES = [
    ("py:write", ab.PROBES, "protoPython/%s/src/runtime/protopy", "py_write.py", "CHECK 3999999"),
    ("py:attr_lookup", "protoPython/benchmarks", "protoPython/%s/src/runtime/protopy", "attr_lookup.py", "result=30000000"),
    ("js:write", ab.PROBES, "protoJS/%s/protojs", "js_write.js", "CHECK 3999999"),
    ("js:object_write_only", "protoJS/tests/benchmarks/standard", "protoJS/%s/protojs", "object_write_only.js", '"ops":1000000'),
    ("clj:fib", "protoClojure/benchmarks", "protoClojure/%s/protoclj", "fib.clj", "832040"),
    ("clj:tak", "protoClojure/benchmarks", "protoClojure/%s/protoclj", "tak.clj", "7"),
    ("clj:assoc", ab.PROBES, "protoClojure/%s/protoclj", "clj_write.clj", "CHECK 3999999"),
]


def main():
    lib, out = sys.argv[1], open(sys.argv[2], "a")
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    for r in range(rounds):
        for name, cwd, exe, script, expect in CASES:
            for arm, tree in (("A", "build_pkg_oct18"), ("B", "build_fix"), ("AA", "build_pkg_oct18")):
                load, _ = ab.fm.quiet_gate()
                ok, st, exp = ab.run((name, cwd, exe % tree, script, expect), lib)
                rec = dict(case=name, round=r, arm=arm, ok=ok, expect=exp, perf=st, load=load)
                out.write(json.dumps(rec) + "\n"); out.flush()
                print(json.dumps(rec), flush=True)


if __name__ == "__main__":
    main()
