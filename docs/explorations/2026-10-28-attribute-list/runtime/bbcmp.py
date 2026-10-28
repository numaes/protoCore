#!/usr/bin/env python3
"""protoScala vs protoClojure (each on master and on explore/attr-list) vs
babashka, on the workloads they share.

For each workload, ROUNDS interleaved rounds (arm order reversed on even
rounds); each run is one process under the build lock with perf stat
(user instructions and cycles) and wall-clock time (perf's duration_time, so lock waits are excluded).  protoCore arms load a
fresh copy of their libprotoCore.so.4 (checked with ldd).  Every run's output
is verified: protoScala against its file's `// EXPECT:` line, protoClojure
and bb against each other.  Prints medians per arm.

usage: bbcmp.py <out.jsonl>"""
import json, os, re, shutil, statistics, subprocess, sys, time

W = "/home/gamarino/Documentos/proyectos/.agent_scratch/pc-attr-list-work"
LOCK = "/home/gamarino/Documentos/proyectos/.agent_scratch/build.lock"
BB = "/home/gamarino/Documentos/proyectos/.agent_scratch/list-cost/bb-dl/bb"
SC = W + "/rt/protoScala/benchmarks/comparable/"
CL = W + "/rt/protoClojure/benchmarks/"
WORK = [("tak", "tak.scala", "tak.clj"), ("fib30", "fib30.scala", "fib.clj"),
        ("sum_loop", "sum_loop.scala", "sum-loop.clj"), ("factorial_100", "factorial_100.scala", "factorial-100.clj")]
ROUNDS = int(os.environ.get("ROUNDS", "5"))


def lib(side):
    d = "%s/libcopies/bb-%s-%d" % (W, side, time.time_ns())
    os.makedirs(d)
    shutil.copy("%s/prefix-%s/lib/libprotoCore.so.4" % (W, side), d + "/libprotoCore.so.4")
    return d


def run(arm, wl):
    name, scala, clj = wl
    env = dict(os.environ)
    if arm == "bb":
        cmd = [BB, CL + clj]
    else:
        rt, side = arm.split("-")
        b = W + ("/rt/protoScala/build-%s/protoscala" if rt == "scala" else "/rt/protoClojure/build-%s/protoclj") % side
        d = lib(side)
        env["LD_LIBRARY_PATH"] = d
        ldd = subprocess.run(["ldd", b], capture_output=True, text=True, env=env).stdout
        got = [l for l in ldd.splitlines() if "libprotoCore" in l][0].split("=>")[1].split()[0]
        assert got.startswith(d), "wrong libprotoCore: " + got
        cmd = [b, (SC + scala) if rt == "scala" else (CL + clj)]
    t0 = time.perf_counter()
    p = subprocess.run(["flock", LOCK, "perf", "stat", "-x", ",", "-e", "instructions:u,cycles:u,duration_time"] + cmd,
                       capture_output=True, text=True, env=env)
    st = {}
    for line in p.stderr.splitlines():
        f = line.split(",")
        if len(f) > 3 and f[0] and f[0][0].isdigit():
            st[f[2].split(":")[0]] = float(f[0])
    wall = st.get("duration_time", 0) / 1e6   # ns -> ms, measured inside the lock
    return {"rc": p.returncode, "instructions": st.get("instructions", 0), "cycles": st.get("cycles", 0),
            "wall_ms": wall, "out": p.stdout.strip().splitlines()[-1:] if p.stdout.strip() else []}


def main():
    fo = open(sys.argv[1], "a")
    arms = ["scala-base", "scala-branch", "clj-base", "clj-branch", "bb"]
    for wl in WORK:
        want = open(SC + wl[1]).readline()[len("// EXPECT:"):].strip()
        res = {a: [] for a in arms}
        bad = []
        for r in range(ROUNDS):
            order = arms if r % 2 == 0 else arms[::-1]
            for a in order:
                x = run(a, wl)
                fo.write(json.dumps({"workload": wl[0], "arm": a, "round": r, **x}) + "\n"); fo.flush()
                res[a].append(x)
                if x["rc"] != 0 or (a.startswith("scala") and x["out"] != [want]):
                    bad.append((a, r, x["rc"], x["out"]))
        clj_outs = {tuple(x["out"]) for a in ("clj-base", "clj-branch", "bb") for x in res[a]}
        if len(clj_outs) != 1:
            bad.append(("clj/bb outputs differ", sorted(clj_outs)))
        print("\n**%s** (median of %d interleaved runs; whole process incl. start-up)  check: %s\n"
              % (wl[0], ROUNDS, "OK (%s; clj/bb %s)" % (want, list(clj_outs)[0][0] if len(clj_outs) == 1 else "?") if not bad else "FAILED %s" % bad))
        print("| arm | instructions | cycles | wall ms (median) | wall ms (min-max) |")
        print("|---|---|---|---|---|")
        for a in arms:
            v = res[a]
            print("| %s | %.0f M | %.0f M | %.1f | %.1f-%.1f |" % (
                a, statistics.median(x["instructions"] for x in v) / 1e6, statistics.median(x["cycles"] for x in v) / 1e6,
                statistics.median(x["wall_ms"] for x in v), min(x["wall_ms"] for x in v), max(x["wall_ms"] for x in v)))
        sys.stdout.flush()


main()
