#!/usr/bin/env python3
"""Sequential GC phase-breakdown runs against the instrumented libprotoCore."""
import json, os, re, subprocess, sys, time
SP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = SP + "/pc-measure/build_gcprof"
BIN = SP + "/bin"
OUT = SP + "/gcphase/out"
JS = os.path.join(os.environ.get("PROTO_WORKSPACE", os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../../.."))), "protoJS/benchmarks/structures")
RES = SP + "/gcphase/results.jsonl"
os.makedirs(OUT, exist_ok=True)

def parse_phases(err):
    last = None
    for line in err.splitlines():
        if line.startswith("[GC-PHASES]"):
            last = line
    d = {}
    if last:
        for k, v in re.findall(r"(\w+)=(\d+)", last):
            d[k] = int(v)
    return d

def run(name, cmd, env_extra, mem, cwd, check):
    env = dict(os.environ)
    env.update({"LD_LIBRARY_PATH": LIB, "PROTOCORE_GC_PROFILE": "1"})
    env.update({k: str(v) for k, v in env_extra.items()})
    full = ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=" + mem, "-p", "MemorySwapMax=0",
            "/usr/bin/time", "-f", "TIME wall=%e user=%U sys=%S rss_kb=%M"] + cmd
    t0 = time.time()
    p = subprocess.run(full, env=env, capture_output=True, text=True, cwd=cwd, timeout=3600)
    open(OUT + "/" + name + ".out", "w").write(p.stdout)
    open(OUT + "/" + name + ".err", "w").write(p.stderr)
    m = re.search(r"TIME wall=([\d.]+) user=([\d.]+) sys=([\d.]+) rss_kb=(\d+)", p.stderr)
    rec = {"name": name, "exit": p.returncode, "env": env_extra, "mem": mem}
    if m:
        rec.update(wall_s=float(m.group(1)), user_s=float(m.group(2)), sys_s=float(m.group(3)), rss_kb=int(m.group(4)))
    rec["ok"] = bool(p.returncode == 0 and check(p.stdout, p.stderr))
    rec["gc"] = parse_phases(p.stderr)
    rec["lib_loaded"] = "[GC-PHASES]" in p.stderr or "[GC-PROFILE]" in p.stderr
    rec["stdout_tail"] = p.stdout[-600:]
    with open(RES, "a") as f:
        f.write(json.dumps(rec) + "\n")
    print("%-28s exit=%s ok=%s wall=%s cycles=%s" % (name, p.returncode, rec["ok"], rec.get("wall_s"), rec["gc"].get("cycles")), flush=True)
    return rec

def js_ok(out, err):
    for line in out.splitlines():
        if line.startswith("{"):
            j = json.loads(line)
            return j.get("ok") is True and len(set(j.get("checksums", [0]))) == 1
    return False

def main(groups):
    if "core" in groups:
        b = SP + "/pc-measure/build_gcprof/adaptive_heap_benchmark"
        ok = lambda o, e: "verified=yes" in o
        run("core_fixed640_live1M", [b, "fixed", "10485760", "1000000"], {}, "10G", SP, ok)
        run("core_adaptive_live1M", [b, "adaptive", "1000000"], {}, "10G", SP, ok)
    if "js40" in groups:
        for wl in ["records", "join", "doctree", "wordfreq", "graph"]:
            for n in [1, 6, 12]:
                run("js40_%s_n%d" % (wl, n), [BIN + "/protojs", "protojs_bench.js"],
                    {"WORKLOAD": wl, "MODE": "par", "N": n, "REPS": 3, "SCALE": 1,
                     "PROTOCORE_HEAP_LIMIT_CELLS": 40000000}, "6G", JS, js_ok)
    if "js10" in groups:
        for wl in ["records", "join", "doctree", "wordfreq", "graph"]:
            for n in [1, 6, 12]:
                run("js10_%s_n%d" % (wl, n), [BIN + "/protojs", "protojs_bench.js"],
                    {"WORKLOAD": wl, "MODE": "par", "N": n, "REPS": 1, "SCALE": 1,
                     "PROTOCORE_HEAP_LIMIT_CELLS": 10000000}, "4G", JS, js_ok)
    if "jsad" in groups:
        for wl in ["records", "join", "doctree", "wordfreq", "graph"]:
            run("jsad_%s_n12" % wl, [BIN + "/protojs", "protojs_bench.js"],
                {"WORKLOAD": wl, "MODE": "par", "N": 12, "REPS": 3, "SCALE": 1,
                 "PROTOCORE_HEAP_LIMIT_CELLS": 40000000, "PROTOCORE_ADAPTIVE_HEAP": 1}, "6G", JS, js_ok)
    if "scala" in groups:
        ok = lambda o, e: "checksum=540451801" in o and o.strip().endswith("ok")
        for t in [1, 6]:
            run("scala_tree_t%d" % t, [BIN + "/protoscala", SP + "/wl/tree_alloc.scala"],
                {"T": t, "BATCHES": 100, "PROTOCORE_HEAP_LIMIT_CELLS": 2000000}, "4G", SP + "/wl", ok)
    if "clj" in groups:
        R = 1500
        expect = 6001000 * R + 1000 * R * (R - 1)
        ok = lambda o, e: ("checksum %d" % expect) in o and o.strip().endswith("ok")
        for t in [1, 6]:
            src = open(SP + "/wl/coll_alloc.clj.in").read().replace("@ROUNDS@", str(R)).replace(
                "@TASKS@", "[" + " ".join(str(i) for i in range(t)) + "]")
            path = SP + "/wl/coll_alloc_t%d.clj" % t
            open(path, "w").write(src)
            run("clj_coll_t%d" % t, [BIN + "/protoclj", path],
                {"PROTOCORE_HEAP_LIMIT_CELLS": 2000000}, "4G", SP + "/wl", ok)
    if "cad" in groups:
        def cad_ok(o, e):
            for line in o.splitlines():
                if line.startswith("{"):
                    return json.loads(line).get("ok") is True
            return False
        run("cad_20k", [BIN + "/protojs", "protojs_cad.js"],
            {"PARTS": 20000, "NS": "1,12", "REPS": 1, "PROTOCORE_HEAP_LIMIT_CELLS": 16000000},
            "3G", JS + "/cad", cad_ok)

if __name__ == "__main__":
    main(sys.argv[1:])
