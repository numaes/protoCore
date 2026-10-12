#!/usr/bin/env python3
"""The measurements specific to the 2026-10-12 report, before (master 5257b37d,
2.14.5) and after (the branch), interleaved run by run, each behind fm.py's
quiet gate.  Same layout as fm.py (FM_ROOT, default .agent_scratch/markfix-oct12):
{base,new}/{rel,inst} are the protoCore build trees, primes_bench the program
built from primes_bench.cpp.  Every run verifies its output.

usage: spec.py <results.jsonl> [--reps N] [--only k5,demo2,primes,markcost]

  k5        protoST `(1 to: 500000) asArray` with PROTOCORE_ADAPTIVE_HEAP=0
            (the fixed 10 M-cell limit; expected: out-of-memory exit 3), and
            under the adaptive controller (protoST 0.6.4's default; expected:
            prints 500000): wall, peak RSS; plus one instrumented run per side
            for the [GC-PHASES] totals (mark time, pauses, mark stack peak).
  demo2     protoST's talk demo 2 (installed with protoST 0.6.4): its own
            "time one thread" and "time 12 actors" lines; must print 4946
            primes twice and true.
  primes    perf stat -r 5 (cycles, instructions, task-clock) of primes_bench
            48000 10 (VERIFIED 4946).
  markcost  adaptive_heap_benchmark fixed 10485760 1000000 with the
            instrumented libraries: mark ns per marked cell, unmark, pauses.
"""
import json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fm  # noqa: E402  (quiet_gate, ROOT)

ROOT = fm.ROOT
SIDES = {"before": ROOT + "/base", "after": ROOT + "/new"}
DEMO2 = "/usr/share/protoST/talks/2026-10-15-fas/demos/02-actors-parallelism.st"
K5 = "| a | a := (1 to: 500000) asArray. Transcript showCr: a size printString.\n"


def clean_env(lib, extra=None):
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("PROTOCORE_") or k.startswith("PROTOST_"):
            del env[k]
    env["LD_LIBRARY_PATH"] = lib
    env.update(extra or {})
    return env


def timed(cmd, env, cwd, mem="4G"):
    full = ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=" + mem, "-p", "MemorySwapMax=0",
            "/usr/bin/time", "-f", "TIME wall=%e user=%U sys=%S rss_kb=%M"] + cmd
    p = subprocess.run(full, env=env, capture_output=True, text=True, cwd=cwd, errors="replace")
    m = re.search(r"TIME wall=([\d.]+) user=([\d.]+) sys=([\d.]+) rss_kb=(\d+)", p.stderr)
    r = {"exit": p.returncode}
    if m:
        r.update(wall_s=float(m.group(1)), user_s=float(m.group(2)), sys_s=float(m.group(3)),
                 rss_kb=int(m.group(4)))
    return r, p


def emit(out, rec):
    out.write(json.dumps(rec) + "\n")
    out.flush()
    print(json.dumps(rec)[:240], flush=True)


def run_k5(out, reps):
    script = os.path.join(ROOT, "k5.st")
    open(script, "w").write(K5)
    for rep in range(reps):
        for mode, extra, want_exit, pat in (("fixed", {"PROTOCORE_ADAPTIVE_HEAP": "0"}, 3, r"out of memory"),
                                            ("adaptive", {}, 0, r"(?m)^500000$")):
            for side, d in SIDES.items():
                load, top = fm.quiet_gate()
                r, p = timed(["/usr/bin/protost", script], clean_env(d + "/rel", extra), ROOT)
                ok = r["exit"] == want_exit and re.search(pat, p.stdout + p.stderr) is not None
                emit(out, dict(group="k5", mode=mode, side=side, rep=rep, ok=ok, load=load, **r))
    # One instrumented run per side and mode: the collector's own totals.
    for mode, extra in (("fixed", {"PROTOCORE_ADAPTIVE_HEAP": "0"}), ("adaptive", {})):
        for side, d in SIDES.items():
            load, top = fm.quiet_gate()
            e = dict(extra)
            e["PROTOCORE_GC_PROFILE"] = "1"
            r, p = timed(["/usr/bin/protost", script], clean_env(d + "/inst", e), ROOT)
            emit(out, dict(group="k5-inst", mode=mode, side=side, ok="[GC-PHASES]" in p.stderr,
                           gc=fm.parse_phases(p.stderr), load=load, **r))


def run_demo2(out, reps):
    for rep in range(reps):
        for side, d in SIDES.items():
            load, top = fm.quiet_gate()
            r, p = timed(["/usr/bin/protost", DEMO2], clean_env(d + "/rel"), ROOT)
            o = p.stdout
            one = re.search(r"time one thread: (\d+) ms", o)
            par = re.search(r"time 12 actors: (\d+) ms", o)
            ok = (r["exit"] == 0 and o.count("4946 primes") == 2 and re.search(r"(?m)^true$", o) is not None
                  and one is not None and par is not None)
            emit(out, dict(group="demo2", side=side, rep=rep, ok=ok, load=load,
                           one_ms=int(one.group(1)) if one else None, par_ms=int(par.group(1)) if par else None,
                           **r))


def perf_stat(cmd, env, cwd):
    full = ["perf", "stat", "-r", "5", "-x", ",", "-e", "cycles,instructions,task-clock"] + cmd
    p = subprocess.run(full, env=env, capture_output=True, text=True, cwd=cwd, errors="replace")
    stats = {}
    for line in p.stderr.splitlines():
        f = line.split(",")
        if len(f) > 3 and f[0] and f[0][0].isdigit():
            stats[f[2]] = float(f[0])
    return p, stats


def run_primes(out, reps):
    for rep in range(reps):
        for side, d in SIDES.items():
            load, top = fm.quiet_gate()
            p, st = perf_stat([ROOT + "/primes_bench", "48000", "10"], clean_env(d + "/rel"), ROOT)
            ok = p.returncode == 0 and p.stdout.count("VERIFIED 4946") == 5
            emit(out, dict(group="primes", side=side, rep=rep, ok=ok, load=load, perf=st))


def run_markcost(out, reps):
    for rep in range(reps):
        for side, d in SIDES.items():
            load, top = fm.quiet_gate()
            r, p = timed([d + "/inst/adaptive_heap_benchmark", "fixed", "10485760", "1000000"],
                         clean_env(d + "/inst", {"PROTOCORE_GC_PROFILE": "1"}), ROOT, mem="4G")
            ok = r["exit"] == 0 and "verified=yes" in p.stdout
            emit(out, dict(group="markcost", side=side, rep=rep, ok=ok, load=load, gc=fm.parse_phases(p.stderr),
                           **r))


def main():
    res = sys.argv[1]
    reps = 5
    only = ["k5", "demo2", "primes", "markcost"]
    a = sys.argv[2:]
    i = 0
    while i < len(a):
        if a[i] == "--reps":
            reps = int(a[i + 1]); i += 2
        elif a[i] == "--only":
            only = a[i + 1].split(","); i += 2
        else:
            raise SystemExit("unknown argument " + a[i])
    out = open(res, "a")
    if "k5" in only:
        run_k5(out, reps)
    if "demo2" in only:
        run_demo2(out, 2 * reps)
    if "primes" in only:
        run_primes(out, reps)
    if "markcost" in only:
        run_markcost(out, reps)


if __name__ == "__main__":
    main()
