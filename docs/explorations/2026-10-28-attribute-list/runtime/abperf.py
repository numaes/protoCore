#!/usr/bin/env python3
"""A/B/A perf stat driver for runtime probes.

Arms: base (runtime built against master 3.3.1), branch (the same runtime
commit built against explore/attr-list), base again (A/A floor).  Each run
loads a FRESH copy of its arm's libprotoCore.so.4 through LD_LIBRARY_PATH
(checked with ldd).  perf stat -r R, user instructions and cycles plus
task-clock.  Scripts printing "CHECK <n>" must agree across arms.

usage:
  abperf.py probes <out.jsonl> <rt> <script>...        # whole-program counts
  abperf.py perop  <out.jsonl> js <name>...            # (400k - 200k) / 200k
"""
import json, os, re, shutil, subprocess, sys, time

W = "/home/gamarino/Documentos/proyectos/.agent_scratch/pc-attr-list-work"
LOCK = "/home/gamarino/Documentos/proyectos/.agent_scratch/build.lock"
LIBS = {"base": W + "/prefix-base/lib/libprotoCore.so.4", "branch": W + "/prefix-branch/lib/libprotoCore.so.4"}
BIN = {"py": "rt/protoPython/build-%s/src/runtime/protopy", "js": "rt/protoJS/build-%s/protojs",
       "clj": "rt/protoClojure/build-%s/protoclj", "scala": "rt/protoScala/build-%s/protoscala"}
ARMS = ["base", "branch", "base"]
R = int(os.environ.get("ABPERF_R", "5"))


def fresh(side):
    d = "%s/libcopies/ab-%s-%d" % (W, side, time.time_ns())
    os.makedirs(d)
    shutil.copy(LIBS[side], d + "/libprotoCore.so.4")
    return d


def run(rt, side, script, cwd):
    b = W + "/" + BIN[rt] % side
    d = fresh(side)
    env = dict(os.environ, LD_LIBRARY_PATH=d)
    ldd = subprocess.run(["ldd", b], capture_output=True, text=True, env=env).stdout
    lib = [l for l in ldd.splitlines() if "libprotoCore" in l][0].split("=>")[1].split()[0]
    assert lib.startswith(d), "wrong libprotoCore: " + lib
    p = subprocess.run(["flock", LOCK, "perf", "stat", "-r", str(R), "-x", ",", "-e",
                        "instructions:u,cycles:u,task-clock", b, script],
                       capture_output=True, text=True, cwd=cwd, env=env)
    st = {}
    for line in p.stderr.splitlines():
        f = line.split(",")
        if len(f) > 3 and f[0] and f[0][0].isdigit():
            st[f[2].split(":")[0]] = float(f[0])
    checks = sorted(set(re.findall(r"CHECK\s+(-?\d+)", p.stdout)))
    first = open(script).readline() if os.path.exists(script) else ""
    if first.startswith("// EXPECT:"):
        # protoScala comparable files: the last printed line must be the
        # EXPECT value (each perf stat repetition prints it once).
        want = first[len("// EXPECT:"):].strip()
        lines = [l for l in p.stdout.splitlines() if l.strip()]
        checks = ["expect-ok"] if lines and all(l.strip() == want for l in lines[-1:]) and p.stdout.count(want) == R else ["expect-FAIL %r" % lines[-1:]]
    if not checks:
        # No CHECK line: compare the output with timings masked (any number
        # with a decimal point, and the protoJS harness's time_ms field).
        norm = re.sub(r"-?\d+\.\d+(e-?\d+)?", "#", p.stdout)
        norm = re.sub(r'"time_ms":\s*\d+', '"time_ms":#', norm)
        norm = re.sub(r"\d+ ?(ms|us|ns|s)\b", "# ms", norm)
        checks = ["out:%x" % (hash(norm) & 0xffffffff)]
    return {"rc": p.returncode, "stats": st, "checks": checks, "lib": lib, "bin": b, "stdout": p.stdout[-400:]}


def main():
    mode, out, rt = sys.argv[1], sys.argv[2], sys.argv[3]
    items = sys.argv[4:]
    fo = open(out, "a")
    here = os.path.dirname(os.path.abspath(__file__))
    for it in items:
        rows = []
        for i, side in enumerate(ARMS):
            if mode == "probes":
                r = run(rt, side, it, here)
                rows.append(r)
                rec = {"mode": mode, "rt": rt, "script": it, "arm": i, "side": side, **r}
            else:
                a = run(rt, side, "%s/jsops/%s_200000.js" % (here, it), here + "/jsops")
                c = run(rt, side, "%s/jsops/%s_400000.js" % (here, it), here + "/jsops")
                per = {k: (c["stats"].get(k, 0) - a["stats"].get(k, 0)) / 2e5 for k in ("instructions", "cycles", "task-clock")}
                r = {"rc": a["rc"] | c["rc"], "stats": per, "checks": [], "lib": a["lib"]}
                rows.append(r)
                rec = {"mode": mode, "rt": rt, "script": it, "arm": i, "side": side, **r}
            fo.write(json.dumps(rec) + "\n"); fo.flush()
        ok = all(r["rc"] == 0 for r in rows) and len({tuple(r["checks"]) for r in rows}) == 1
        s = rows
        print("%-4s %-16s base %10.0f i %10.0f c | branch %10.0f i %10.0f c (%+5.1f%% i %+5.1f%% c) | A/A %+5.1f%% c %s" % (
            rt, it.split("/")[-1],
            s[0]["stats"].get("instructions", 0), s[0]["stats"].get("cycles", 0),
            s[1]["stats"].get("instructions", 0), s[1]["stats"].get("cycles", 0),
            100 * (s[1]["stats"].get("instructions", 0) / max(1, s[0]["stats"].get("instructions", 1)) - 1),
            100 * (s[1]["stats"].get("cycles", 0) / max(1, (s[0]["stats"].get("cycles", 1) + s[2]["stats"].get("cycles", 1)) / 2) - 1),
            100 * (s[2]["stats"].get("cycles", 0) / max(1, s[0]["stats"].get("cycles", 1)) - 1),
            "OK" if ok else "MISMATCH/FAIL %s" % [(r["rc"], r["checks"]) for r in rows]), flush=True)


main()
