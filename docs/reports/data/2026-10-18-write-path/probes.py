#!/usr/bin/env python3
"""Write-path probes: does a runtime's attribute write reach protoCore's
changed write path (2.14.7)?  Each probe (probes/<rt>_<op>.<ext>) loops
2,000,000 times; <rt>_base reads an attribute, <rt>_write also rewrites an
existing one (sc_copy: a case-class copy), and prints a checksum, which must
match across arms.  perf stat -r 5 (cycles, instructions) per arm, arms
interleaved: before (runtime built against protoCore 2.14.6), after (the
same commit built against 2.14.7), and before again (the A/A floor).

usage: probes.py <results.jsonl>
"""
import json, os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "../2026-10-12-mark-stack-int-compare"))  # fm.quiet_gate
import fm  # quiet_gate

WS = os.environ.get("PROTO_WORKSPACE", os.path.abspath(os.path.join(HERE, "../../../../..")))
# One binary per runtime for both arms; the arms differ only in the library
# (fresh_lib).  See cross_run.sh.
TREES = {"before": "build_cmp2146", "after": "build_cmp2146"}
RT = {"py": ("protoPython", "src/runtime/protopy", "py"), "js": ("protoJS", "protojs", "js"),
      "sc": ("protoScala", "protoscala", "scala"), "clj": ("protoClojure", "protoclj", "clj"),
      "st": ("protoST", "protost", "st")}
OPS = ["base", "write", "copy"]


STAGE = os.path.join(WS, ".agent_scratch", "installers-oct2026")
LIBDIR = os.environ.get("PROBE_LIBS", "/tmp/probe-libs")


def fresh_lib(side):
    """A fresh copy of the side's staged protoCore (see cross_run.sh, fresh())."""
    import shutil, time
    v = "2146" if side == "before" else "2147"
    d = os.path.join(LIBDIR, "%s_%d" % (side, time.time_ns()))
    os.makedirs(d)
    shutil.copy(os.path.join(STAGE, "stage" + v, "usr/lib/x86_64-linux-gnu/libprotoCore.so.3"),
                os.path.join(d, "libprotoCore.so.3"))
    return d


def perf(cmd, libdir=None):
    env = dict(os.environ, LD_LIBRARY_PATH=libdir) if libdir else None
    p = subprocess.run(["perf", "stat", "-r", "5", "-x", ",", "-e", "cycles,instructions,task-clock"] + cmd,
                       capture_output=True, text=True, cwd=os.path.join(HERE, "probes"), env=env)
    st = {}
    for line in p.stderr.splitlines():
        f = line.split(",")
        if len(f) > 3 and f[0] and f[0][0].isdigit():
            st[f[2]] = float(f[0])
    checks = set(re.findall(r"CHECK\s+(-?\d+)", p.stdout))
    return p.returncode, st, checks, p.stdout.count("CHECK")


def main():
    out = open(sys.argv[1], "a")
    for rt, (repo, rel, ext) in RT.items():
        for op in OPS:
            script = "%s_%s.%s" % (rt, op, ext)
            if not os.path.exists(os.path.join(HERE, "probes", script)):
                continue
            for arm, side in (("before", "before"), ("after", "after"), ("aa", "before")):
                load, top = fm.quiet_gate()
                exe = os.path.join(WS, repo, TREES[side], rel)
                rc, st, checks, n = perf([exe, script], fresh_lib(side))
                rec = dict(rt=rt, op=op, arm=arm, ok=(rc == 0 and len(checks) == 1 and n == 5),
                           check=sorted(checks), perf=st, load=load)
                out.write(json.dumps(rec) + "\n"); out.flush()
                print(json.dumps(rec), flush=True)


if __name__ == "__main__":
    main()
