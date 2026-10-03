#!/usr/bin/env python3
"""Calibration harness: runs workloads under heap-policy variants, sequentially.

usage: run.py <out.jsonl> <reps> <variant,...> [workload-glob ...]
Each run: systemd-run --user --scope MemoryMax=10G, /usr/bin/time for peak RSS,
PROTOCORE_HEAP_TRACE=1 parsed for cycles / final S / max pressure, stdout
checked against the workload's expected output.
"""
import fnmatch, json, os, re, subprocess, sys, time

S = os.path.dirname(os.path.abspath(__file__))
# The directory that holds the protoCore, protoST, protoPython, ... checkouts.
P = os.environ.get("PROTO_WORKSPACE", os.path.abspath(os.path.join(S, "../../../../..")))
B = S + "/bin"

def W(name, cwd, argv, expect, env=None, timeout=600):
    return dict(name=name, cwd=cwd, argv=argv, expect=expect, env=env or {}, timeout=timeout)

AHB = B + "/ahb"  # the benchmark binary is linked against libprotoCore; any lib works
PST = P + "/protoST/build_wr/protost"
PPY = P + "/protoPython/build_release/src/runtime/protopy"
PCL = P + "/protoClojure/build_pkg_oct2026/protoclj"
PSC = P + "/protoScala/build_pkg_oct2026/protoscala"
PJS = B + "/protojs_b"   # protoJS/build/protojs (newest; H = 75 % of memory)
PJS640 = B + "/protojs"  # protoJS/build_release/protojs (10 M-cell fixed limit)

WORKLOADS = []
for live in (0, 100000, 1000000, 5000000):
    WORKLOADS.append(W("core/ahb-%dk" % (live // 1000), S, [AHB, "fixed", "10485760", str(live)],
                       r"verified=yes"))
for f, exp in [("benchmarks/actors/saturation_big.st", "VERIFIED 20004000000"),
               ("benchmarks/actors/parallel_speedup.st", "VERIFIED 135000900000"),
               ("benchmarks/comparable/list_append.st", "VERIFIED 10000"),
               ("benchmarks/comparable/str_concat.st", "VERIFIED 2000"),
               ("benchmarks/comparable/fib.st", "VERIFIED 75025"),
               ("benchmarks/actors/message_throughput.st", "VERIFIED 2000")]:
    WORKLOADS.append(W("st/" + os.path.basename(f)[:-3], P + "/protoST", [PST, f], exp,
                       {"PROTOST_WORKERS": "4"}))
for name, f, args, env, pat in [
        ("int_sum_loop", "int_sum_loop.py", [], {"BENCH_N": "2000000"}, r"BENCH_RESULT"),
        ("list_append_loop", "list_append_loop.py", [], {"BENCH_N": "200000"}, r"BENCH_RESULT"),
        ("str_concat_loop", "str_concat_loop.py", [], {"BENCH_N": "20000"}, r"BENCH_RESULT"),
        ("range_iterate", "range_iterate.py", [], {"BENCH_N": "2000000"}, r"BENCH_RESULT"),
        ("multithread_cpu", "multithreaded_cpu.py", [], {}, r"BENCH_RESULT|result"),
        ("attr_lookup", "attr_lookup.py", ["5000000"], {}, r"BENCH_RESULT"),
        ("call_recursion", "call_recursion.py", [], {"BENCH_N": "27"}, r"BENCH_RESULT"),
        ("fib", "pyperf/bench_fib.py", ["29"], {}, r"fib n="),
        ("binary_trees", "pyperf/bench_binary_trees.py", ["9"], {}, r"binary_trees|depth"),
        ("nqueens", "pyperf/bench_nqueens.py", ["9"], {}, r"nqueens"),
        ("richards_lite", "pyperf/bench_richards_lite.py", [], {}, r"richards"),
        ("sieve", "pyperf/bench_sieve.py", ["50000"], {}, r"sieve")]:
    WORKLOADS.append(W("py/" + name, P + "/protoPython", [PPY, "benchmarks/" + f] + args, pat, env))
for f, exp in [("fib.clj", "832040"), ("tak.clj", None), ("sum-loop.clj", None),
               ("reduce-list.clj", None), ("sum-squares.clj", None),
               ("actor-saturation-32.clj", r"(?m)^ok$")]:
    WORKLOADS.append(W("clj/" + f[:-4], P + "/protoClojure", [PCL, "benchmarks/" + f], exp,
                       {"PROTOCLJ_ACTOR_WORKERS": "4"}))
for f in ["fib30", "list_ops", "map_build", "object_tree", "str_concat", "tak", "sum_loop"]:
    WORKLOADS.append(W("scala/" + f, P + "/protoScala",
                       [PSC, "benchmarks/comparable/%s.scala" % f], "EXPECT"))
WORKLOADS.append(W("scala/actor-saturation-32", P + "/protoScala",
                   [PSC, "benchmarks/actors/actor-saturation-32.scala"], r"(?m)^ok$",
                   {"PROTOSCALA_ACTOR_WORKERS": "4"}))
for wl in ["records", "doctree", "graph", "join", "wordfreq"]:
    WORKLOADS.append(W("js/%s-seq" % wl, P + "/protoJS/benchmarks/structures",
                       [PJS, "protojs_bench.js"], r'"ok":true',
                       {"WORKLOAD": wl, "MODE": "seq", "N": "6", "REPS": "3", "SCALE": "1"}))
WORKLOADS.append(W("js/records-par", P + "/protoJS/benchmarks/structures",
                   [PJS, "protojs_bench.js"], r'"ok":true',
                   {"WORKLOAD": "records", "MODE": "par", "N": "6", "REPS": "3", "SCALE": "1"}))
for wl in ["records", "graph", "doctree", "join", "wordfreq"]:
    WORKLOADS.append(W("jsq/%s-seq" % wl, P + "/protoJS/benchmarks/structures",
                       [PJS, "protojs_bench.js"], r'"ok":true',
                       {"WORKLOAD": wl, "MODE": "seq", "N": "2", "REPS": "1", "SCALE": "1"}))
WORKLOADS.append(W("jsq/records-par", P + "/protoJS/benchmarks/structures",
                   [PJS, "protojs_bench.js"], r'"ok":true',
                   {"WORKLOAD": "records", "MODE": "par", "N": "6", "REPS": "1", "SCALE": "1"}))
WORKLOADS.append(W("js/probe-100k", S, [PJS, S + "/probe100k.js"],
                   "objects 100000 sumId 9999900000 sumX 2499975000 n7 1000"))
WORKLOADS.append(W("js640/probe-100k", S, [PJS640, S + "/probe100k.js"],
                   "objects 100000 sumId 9999900000 sumX 2499975000 n7 1000"))

def load_variants():
    with open(S + "/variants.json") as f:
        return json.load(f)

def expected_ok(w, out):
    e = w["expect"]
    if e is None:
        return True
    if e == "EXPECT":
        src = open(os.path.join(w["cwd"], w["argv"][1])).read()
        m = re.search(r"//\s*EXPECT:\s*(.*)", src)
        want = m.group(1).strip()
        lines = [l for l in out.strip().splitlines() if l.strip()]
        return bool(lines) and lines[-1].strip() == want
    return re.search(e, out) is not None

def run_one(w, vname, v):
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("PROTOCORE_"):
            del env[k]
    env.update(w["env"])
    env["LD_LIBRARY_PATH"] = v["lib"]
    env["PROTOCORE_HEAP_TRACE"] = "1"
    env.update(v.get("env", {}))
    argv = list(w["argv"])
    tf = S + "/time.out"
    cmd = ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=10G",
           "-p", "MemorySwapMax=0", "/usr/bin/time", "-f", "%M %e %U %S", "-o", tf,
           "timeout", str(w["timeout"])] + argv
    t0 = time.time()
    p = subprocess.run(cmd, cwd=w["cwd"], env=env, capture_output=True, text=True,
                       errors="replace")
    wall = time.time() - t0
    rss = el = us = sy = None
    try:
        parts = open(tf).read().strip().splitlines()[-1].split()
        rss, el, us, sy = int(parts[0]), float(parts[1]), float(parts[2]), float(parts[3])
    except Exception:
        pass
    cyc = fixed = 0
    finalS = None
    maxp = 0.0
    for line in p.stderr.splitlines():
        if not line.startswith("protoCore heap:"):
            continue
        if " fixed " in line:
            fixed += 1
            continue
        cyc += 1
        m = re.search(r"S=\d+->(\d+)", line)
        if m:
            finalS = int(m.group(1))
        m = re.search(r" p=([\d.]+)", line)
        if m:
            maxp = max(maxp, float(m.group(1)))
    ok = p.returncode == 0 and expected_ok(w, p.stdout)
    other = "\n".join(l for l in p.stderr.splitlines() if not l.startswith("protoCore heap:"))
    return dict(workload=w["name"], variant=vname, rc=p.returncode, ok=ok, rssKiB=rss,
                elapsed=el, user=us, sys=sy, wall=wall, adaptiveCycles=cyc, fixedCycles=fixed,
                finalS=finalS, maxP=maxp, stdoutTail=p.stdout.strip()[-200:],
                stderrTail=other.strip()[-300:])

def main():
    out, reps, vnames = sys.argv[1], int(sys.argv[2]), sys.argv[3].split(",")
    globs = sys.argv[4:] or ["*"]
    variants = load_variants()
    ws = [w for w in WORKLOADS if any(fnmatch.fnmatch(w["name"], g) for g in globs)]
    with open(out, "a") as f:
        for w in ws:
            for r in range(reps):
                for vn in vnames:
                    res = run_one(w, vn, variants[vn])
                    res["rep"] = r
                    f.write(json.dumps(res) + "\n")
                    f.flush()
                    print("%-28s %-10s ok=%-5s rss=%7.0fMB t=%6.2fs u+s=%6.2f cyc=%d/%d S=%s maxp=%.3f" % (
                        w["name"], vn, res["ok"], (res["rssKiB"] or 0) / 1024, res["elapsed"] or -1,
                        (res["user"] or 0) + (res["sys"] or 0), res["adaptiveCycles"],
                        res["fixedCycles"], res["finalS"], res["maxP"]), flush=True)

if __name__ == "__main__":
    main()
