#!/usr/bin/env python3
"""Collector-throughput runs: synthetic workloads against a given libprotoCore.

usage: runner.py <lib_dir> <label> <results.jsonl> <group>... [--reps N] [--env K=V]...
Every run verifies its own output; a run that does not verify is recorded
with ok=false and is never a data point.
"""
import json, os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
# PROTO_BIN: the directory holding protojs, protoscala and protoclj (the
# installed Release runtimes, run against <lib_dir> through LD_LIBRARY_PATH;
# check with ldd).  PROTO_JS_BENCH: protoJS's benchmarks/structures directory
# (measured at protoJS b493dc270).  Workloads: ./workloads.  Each run waits
# until no compiler or test suite runs and the load average is below
# MAX_LOAD (default 6).
BIN = os.environ.get("PROTO_BIN", "/usr/bin")
JS = os.environ.get("PROTO_JS_BENCH", os.path.join(os.environ.get("PROTO_WORKSPACE", "."), "protoJS/benchmarks/structures"))
WL = os.environ.get("PROTO_WL", os.path.join(HERE, "workloads"))

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

def busy_machine():
    out = subprocess.run(["ps", "-eo", "comm"], capture_output=True, text=True).stdout.split()
    busy = [c for c in out if c in ("cc1plus", "ctest", "proto_tests", "ld", "make", "ninja")]
    # Other agents' benchmarks are not compilers: gate on the load too.
    if os.getloadavg()[0] > float(os.environ.get("MAX_LOAD", "6.0")):
        busy.append("load=%.1f" % os.getloadavg()[0])
    return busy

def run(lib, label, res, name, cmd, env_extra, mem, cwd, check, rep):
    while True:
        b = busy_machine()
        if not b:
            break
        print("  waiting, machine busy:", sorted(set(b)), flush=True)
        time.sleep(30)
    env = dict(os.environ)
    env.update({"LD_LIBRARY_PATH": lib, "PROTOCORE_GC_PROFILE": "1"})
    env.update({k: str(v) for k, v in env_extra.items()})
    full = ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=" + mem, "-p", "MemorySwapMax=0",
            "/usr/bin/time", "-f", "TIME wall=%e user=%U sys=%S rss_kb=%M"] + cmd
    load = os.getloadavg()[0]
    p = subprocess.run(full, env=env, capture_output=True, text=True, cwd=cwd, timeout=3600)
    m = re.search(r"TIME wall=([\d.]+) user=([\d.]+) sys=([\d.]+) rss_kb=(\d+)", p.stderr)
    rec = {"name": name, "label": label, "rep": rep, "exit": p.returncode, "env": env_extra, "mem": mem,
           "load1_before": load}
    if m:
        rec.update(wall_s=float(m.group(1)), user_s=float(m.group(2)), sys_s=float(m.group(3)), rss_kb=int(m.group(4)))
    rec["ok"] = bool(p.returncode == 0 and check(p.stdout, p.stderr))
    rec["gc"] = parse_phases(p.stderr)
    # The aged-heap window: the cumulative counters at the first cycle by
    # which the collector had freed WARM x the heap limit (the heap cycled
    # WARM times).  last - warm is the run's steady state on an aged heap.
    limit = int(env_extra.get("PROTOCORE_HEAP_LIMIT_CELLS", 0) or 0)
    if limit:
        warm = 3 * limit
        for line in p.stderr.splitlines():
            if line.startswith("[GC-PHASES]"):
                d = dict((k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)", line))
                if d.get("freed_cells", 0) >= warm:
                    rec["gc_warm"] = d
                    break
    rec["lib_loaded"] = "[GC-PHASES]" in p.stderr or "[GC-PROFILE]" in p.stderr
    rec["stdout_tail"] = p.stdout[-400:]
    rec["json"] = [l for l in p.stdout.splitlines() if l.startswith("{")][-1:]
    rec["user_s_total"] = rec.get("user_s")
    if not rec["ok"]:
        rec["stderr_tail"] = p.stderr[-800:]
    with open(res, "a") as f:
        f.write(json.dumps(rec) + "\n")
    g = rec["gc"]
    w = g.get("headroom_wait", 0); nw = g.get("headroom_waits", 0)
    print("%-22s %-10s r%d ok=%s wall=%s rss=%sMB cyc=%s wait=%.1fs n=%d mean=%.1fms sweep=%.2fs swept=%s" % (
        name, label, rep, rec["ok"], rec.get("wall_s"), rec.get("rss_kb", 0)//1024, g.get("cycles"),
        w/1e6, nw, (w/nw/1e3 if nw else 0), g.get("sweep", 0)/1e6, g.get("swept_cells")), flush=True)
    return rec

def js_ok(out, err):
    for line in out.splitlines():
        if line.startswith("{"):
            j = json.loads(line)
            # Each task's checksum differs by task; the tabulator checks that
            # task k's checksum is the same in every run of a workload.
            return j.get("ok") is True and len(j.get("checksums", [])) == j.get("n")
    return False

def main():
    args = sys.argv[1:]
    reps = 1
    extra = {}
    rest = []
    i = 0
    while i < len(args):
        if args[i] == "--reps":
            reps = int(args[i + 1]); i += 2
        elif args[i] == "--env":
            k, v = args[i + 1].split("=", 1); extra[k] = v; i += 2
        else:
            rest.append(args[i]); i += 1
    lib, label, res = rest[0], rest[1], rest[2]
    groups = rest[3:]
    def R(name, cmd, env, mem, cwd, check):
        e = dict(env); e.update(extra)
        for r in range(reps):
            run(lib, label, res, name, cmd, e, mem, cwd, check, r)
    core_ok = lambda o, e: "verified=yes" in o
    bench = lib + "/adaptive_heap_benchmark"
    if "core" in groups:
        R("core_fixed640_live1M", [bench, "fixed", "10485760", "1000000"], {}, "10G", HERE, core_ok)
        R("core_adaptive_live1M", [bench, "adaptive", "1000000"], {}, "10G", HERE, core_ok)
    def js(name, wl, n, reps_, limit, mem, more=None):
        env = {"WORKLOAD": wl, "MODE": "par", "N": n, "REPS": reps_, "SCALE": 1,
               "PROTOCORE_HEAP_LIMIT_CELLS": limit}
        if more: env.update(more)
        R(name, [BIN + "/protojs", "protojs_bench.js"], env, mem, JS, js_ok)
    if "js" in groups:
        js("js40_records_n6", "records", 6, 3, 40000000, "6G")
        js("js40_graph_n6", "graph", 6, 3, 40000000, "6G")
        js("js40_wordfreq_n12", "wordfreq", 12, 3, 40000000, "6G")
        js("js10_records_n12", "records", 12, 1, 10000000, "4G")
    if "jsn12" in groups:
        js("js40_wordfreq_n12", "wordfreq", 12, 3, 40000000, "6G")
        js("js10_records_n12", "records", 12, 1, 10000000, "4G")
    if "js10r12" in groups:
        js("js10_records_n12", "records", 12, 1, 10000000, "4G")
    if "aged" in groups:
        js("aged_js10_records_n12", "records", 12, 3, 10000000, "4G")
        Ra = 1500
        expect = 6001000 * Ra + 1000 * Ra * (Ra - 1)
        oka = lambda o, e: ("checksum %d" % expect) in o and o.strip().endswith("ok")
        src = open(WL + "/coll_alloc.clj.in").read().replace("@ROUNDS@", str(Ra)).replace(
            "@TASKS@", "[0 1 2 3 4 5]")
        path = WL + "/coll_alloc_r%d_t6.clj" % Ra
        open(path, "w").write(src)
        R("aged_clj_coll_t6", [BIN + "/protoclj", path], {"PROTOCORE_HEAP_LIMIT_CELLS": 2000000}, "4G", WL, oka)
    if "js1" in groups:
        js("js10_records_n1", "records", 1, 1, 10000000, "4G")
    if "jsad" in groups:
        js("jsad_wordfreq_n12", "wordfreq", 12, 3, 40000000, "6G", {"PROTOCORE_ADAPTIVE_HEAP": 1})
        js("jsad_records_n12", "records", 12, 3, 40000000, "6G", {"PROTOCORE_ADAPTIVE_HEAP": 1})
    if "scala" in groups:
        ok = lambda o, e: "checksum=540451801" in o and o.strip().endswith("ok")
        for t in [1, 6]:
            R("scala_tree_t%d" % t, [BIN + "/protoscala", WL + "/tree_alloc.scala"],
              {"T": t, "BATCHES": 30, "PROTOCORE_HEAP_LIMIT_CELLS": 2000000}, "4G", WL, ok)
    if "clj" in groups or "clj6" in groups:
        Rn = 500
        expect = 6001000 * Rn + 1000 * Rn * (Rn - 1)
        ok = lambda o, e: ("checksum %d" % expect) in o and o.strip().endswith("ok")
        for t in ([6] if "clj6" in groups else [1, 6]):
            src = open(WL + "/coll_alloc.clj.in").read().replace("@ROUNDS@", str(Rn)).replace(
                "@TASKS@", "[" + " ".join(str(i) for i in range(t)) + "]")
            path = WL + "/coll_alloc_r%d_t%d.clj" % (Rn, t)
            open(path, "w").write(src)
            R("clj_coll_t%d" % t, [BIN + "/protoclj", path],
              {"PROTOCORE_HEAP_LIMIT_CELLS": 2000000}, "4G", WL, ok)

if __name__ == "__main__":
    main()
