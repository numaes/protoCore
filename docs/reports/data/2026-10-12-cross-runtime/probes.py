#!/usr/bin/env python3
"""Per-operation probes: does a runtime's integer operation reach protoCore?
Each probe (probes/<rt>_<op>.<ext>) loops 2,000,000 times over one operation
and prints a checksum, which must match across arms.  perf stat -r 5 (cycles,
instructions) per arm, arms interleaved: before (runtime built against
protoCore 2.14.5), after (the same commit built against 2.14.6), and before
again (the A/A floor).  An operation that bypasses protoCore shows no change
beyond the A/A difference.

usage: probes.py <results.jsonl>
"""
import json, os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "../2026-10-12-mark-stack-int-compare"))
import fm  # quiet_gate

WS = os.environ.get("PROTO_WORKSPACE", os.path.abspath(os.path.join(HERE, "../../../../..")))
TREES = {"before": "build_cmp2145", "after": "build_pkg_oct12"}
RT = {"py": ("protoPython", "src/runtime/protopy", "py"), "js": ("protoJS", "protojs", "js"),
      "sc": ("protoScala", "protoscala", "scala"), "clj": ("protoClojure", "protoclj", "clj")}
OPS = ["base", "cmp", "mod", "div", "neg"]


def perf(cmd):
    p = subprocess.run(["perf", "stat", "-r", "5", "-x", ",", "-e", "cycles,instructions,task-clock"] + cmd,
                       capture_output=True, text=True, cwd=os.path.join(HERE, "probes"))
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
                rc, st, checks, n = perf([exe, script])
                rec = dict(rt=rt, op=op, arm=arm, ok=(rc == 0 and len(checks) == 1 and n == 5),
                           check=sorted(checks), perf=st, load=load)
                out.write(json.dumps(rec) + "\n"); out.flush()
                print(json.dumps(rec), flush=True)


if __name__ == "__main__":
    main()
