#!/usr/bin/env python3
"""A/B of the context slot stack: the same runtime binaries (built against
protoCore 2.14.6, build_cmp2146) load either a clean master build of
libprotoCore (A) or the perf/context-slot-stack build (B), each from a fresh
copy per run (the library file-instance artefact of 2026-10-18).  Arms
interleaved A, B, A again (the A/A floor); perf stat -r 5 cycles and
instructions; every run's printed result is checked against the expected one.

usage: ab.py <libA.so> <libB.so> <results.jsonl> [rounds]
"""
import json, os, re, shutil, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
WS = os.environ.get("PROTO_WORKSPACE", os.path.abspath(os.path.join(HERE, "../../../../..")))
sys.path.insert(0, os.environ.get("FM_DIR", os.path.join(HERE, "../2026-10-12-mark-stack-int-compare")))
import fm  # quiet_gate

PROBES = os.environ.get("PROBES", os.path.join(HERE, "../2026-10-18-write-path/probes"))
# name, cwd, binary (relative to WS), script, expected output fragment
CASES = [
    ("clj:fib", "protoClojure/benchmarks", "protoClojure/build_cmp2146/protoclj", "fib.clj", "832040"),
    ("clj:tak", "protoClojure/benchmarks", "protoClojure/build_cmp2146/protoclj", "tak.clj", "7"),
    ("clj:assoc", PROBES, "protoClojure/build_cmp2146/protoclj", "clj_write.clj", "CHECK 3999999"),
    ("sc:fib30", "protoScala/benchmarks/comparable", "protoScala/build_cmp2146/protoscala", "fib30.scala", None),
    ("sc:object_tree", "protoScala/benchmarks/comparable", "protoScala/build_cmp2146/protoscala", "object_tree.scala", None),
    ("sc:copy", PROBES, "protoScala/build_cmp2146/protoscala", "sc_copy.scala", "CHECK 3999999"),
    ("py:fib", "protoPython/benchmarks/pyperf", "protoPython/build_cmp2146/src/runtime/protopy", "bench_fib.py", "result=75025"),
    ("js:function_calls", "protoJS/tests/benchmarks/standard", "protoJS/build_cmp2146/protojs", "function_calls.js", "\"state\":1000001"),
]
LIBDIR = os.environ.get("AB_LIBS", "/tmp/ab-libs")


def fresh(lib):
    d = os.path.join(LIBDIR, str(time.time_ns()))
    os.makedirs(d)
    shutil.copy(lib, os.path.join(d, "libprotoCore.so.3"))
    return d


def expected_of(cwd, script):
    m = re.search(r"EXPECT:\s*(.+)", open(os.path.join(cwd, script)).read())
    return m.group(1).strip() if m else None


def run(case, lib):
    name, cwd, exe, script, expect = case
    cwd = cwd if os.path.isabs(cwd) else os.path.join(WS, cwd)
    expect = expect or expected_of(cwd, script)
    env = dict(os.environ, LD_LIBRARY_PATH=fresh(lib))
    p = subprocess.run(["perf", "stat", "-r", "5", "-x", ",", "-e", "cycles,instructions",
                        os.path.join(WS, exe), script], cwd=cwd, env=env, capture_output=True, text=True)
    st = {}
    for line in p.stderr.splitlines():
        f = line.split(",")
        if len(f) > 3 and f[0] and f[0][0].isdigit():
            st[f[2]] = float(f[0])
    ok = p.returncode == 0 and (expect is None or p.stdout.count(expect) == 5)
    return ok, st, expect


def main():
    libs = {"A": sys.argv[1], "B": sys.argv[2]}
    out = open(sys.argv[3], "a")
    rounds = int(sys.argv[4]) if len(sys.argv) > 4 else 3
    for r in range(rounds):
        for case in CASES:
            for arm, lib in (("A", "A"), ("B", "B"), ("AA", "A")):
                load, _ = fm.quiet_gate()
                ok, st, expect = run(case, libs[lib])
                rec = dict(case=case[0], round=r, arm=arm, ok=ok, expect=expect, perf=st, load=load)
                out.write(json.dumps(rec) + "\n"); out.flush()
                print(json.dumps(rec), flush=True)


if __name__ == "__main__":
    main()
