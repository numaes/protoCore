#!/usr/bin/env python3
"""protoJS sequential programs: protoJS's default heap policy (no environment)
against protoCore's adaptive heap controller (PROTOCORE_ADAPTIVE_HEAP=1), on
the structure workloads (benchmarks/structures, MODE=seq N=1, median of 3
in-process repetitions per run).  Rounds alternate the two modes; every run
is behind the quiet gate; checksums must agree between modes.

usage: adaptive_eval.py <results.jsonl> [rounds]
"""
import json, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "../2026-10-12-mark-stack-int-compare"))
import fm  # quiet_gate

WS = os.environ.get("PROTO_WORKSPACE", os.path.abspath(os.path.join(HERE, "../../../../..")))
PROTOJS = os.path.join(WS, "protoJS", "build_cmp2146", "protojs")   # + a fresh copy of the 2.14.7 library
STAGE = os.path.join(WS, ".agent_scratch", "installers-oct2026")


def fresh_libdir():
    import shutil, time
    d = os.path.join(os.environ.get("PROBE_LIBS", "/tmp/probe-libs"), "adaptive_%d" % time.time_ns())
    os.makedirs(d)
    shutil.copy(os.path.join(STAGE, "stage2147/usr/lib/x86_64-linux-gnu/libprotoCore.so.3"), d)
    return d
CWD = os.path.join(WS, "protoJS", "benchmarks", "structures")
MODES = {"default": {}, "adaptive": {"PROTOCORE_ADAPTIVE_HEAP": "1"}}


def run(workload, mode):
    env = dict(os.environ, WORKLOAD=workload, MODE="seq", N="1", REPS="3",
               LD_LIBRARY_PATH=fresh_libdir(), **MODES[mode])
    env.pop("PROTOCORE_HEAP_LIMIT_CELLS", None)
    p = subprocess.run(["/usr/bin/time", "-f", "TIME %e %U %S %M", PROTOJS, "protojs_bench.js"],
                       cwd=CWD, env=env, capture_output=True, text=True, timeout=1800)
    rec = {"workload": workload, "mode": mode, "rc": p.returncode}
    for line in p.stdout.splitlines():
        if line.startswith("{"):
            rec.update(json.loads(line))
    t = [l for l in p.stderr.splitlines() if l.startswith("TIME ")]
    if t:
        e, u, s, m = t[-1].split()[1:]
        rec.update(wall_s=float(e), user_s=float(u), sys_s=float(s), maxrss_mb=int(m) // 1024)
    return rec


def main():
    out = open(sys.argv[1], "a")
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    for r in range(rounds):
        for wl in ("records", "join", "doctree", "wordfreq", "graph"):
            order = ("default", "adaptive") if r % 2 == 0 else ("adaptive", "default")
            for mode in order:
                load, _ = fm.quiet_gate()
                rec = run(wl, mode)
                rec.update(round=r, load=load)
                out.write(json.dumps(rec) + "\n"); out.flush()
                print(wl, mode, r, rec.get("median_ms"), rec.get("maxrss_mb"), rec.get("checksums"), flush=True)


if __name__ == "__main__":
    main()
