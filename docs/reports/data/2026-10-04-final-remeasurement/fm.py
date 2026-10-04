#!/usr/bin/env python3
"""Final re-measurement (2026-10-04): the same synthetic workloads as the
2026-10-03 baselines, run BEFORE (the 2026-10-03 runtime binaries on protoCore
2.10.2) and AFTER (the runtime masters with write coalescing, packaged on
2026-10-04, on protoCore 2.14.1), interleaved run by run in one session.

usage: fm.py <group> <results.jsonl> [--reps N] [--only glob,...]

Groups:
  gc     the phase-breakdown workloads (2026-10-03-gc-phase-breakdown), with
         the instrumented libraries (PROTOCORE_GC_INSTRUMENT=ON) of each side:
         wall, waits, sweep ns per cell, collector CPU, peak RSS.
  cal    the calibration workloads (2026-10-03-adaptive-heap-calibration)
         with the release libraries: each runtime's default policy before and
         after, and the adaptive controller (PROTOCORE_ADAPTIVE_HEAP=1) after
         (and before, for reference).
  rt6    perf stat -r 3 of six single-threaded programs per runtime, before
         and after, release libraries.
  cells  cells per operation of the write-coalescing micro-cases, from the
         peak resident set of two loop lengths with no collection.
  policy the phase-breakdown workloads with no heap limit given: each
         runtime's default policy against the controller (after side).

Layout (FM_ROOT, default: this workspace's .agent_scratch/final-oct04):
  {base,new}/root     the five runtime .deb packages of that side, extracted
                      with dpkg-deb -x (protoCore's package is NOT extracted
                      there): each binary finds its own standard library
                      (relative to the executable) and, through its RUNPATH,
                      its own libprotoPython / libprotoScala; libprotoCore
                      comes from LD_LIBRARY_PATH (checked with ldd)
  base/lib210inst     protoCore 2.10.2 + instrumentation, adaptive_heap_benchmark
  new/lib2141inst     protoCore 2.14.1 + instrumentation, adaptive_heap_benchmark
  base/lib210         protoCore 2.10.2 release (the packaged build)
  new/lib2141         protoCore 2.14.1 release (the packaged build)

Every run verifies its own output; a run that does not verify is recorded
with ok=false and is never a data point.  Before each run the harness waits
until no compiler, linker or test suite runs and fewer than QUIET_CORES CPUs
are busy over 3 s (quiet_gate), and records the load average, the busy CPUs
and the busiest processes.  (The calibration group of the 2026-10-04 session
ran with an earlier gate on the 1-minute load average, MAX_LOAD = 4.5.)
"""
import fnmatch, json, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
WS = os.environ.get("PROTO_WORKSPACE", os.path.abspath(os.path.join(HERE, "../../../../..")))
ROOT = os.environ.get("FM_ROOT", os.path.join(WS, ".agent_scratch/final-oct04"))
WL = os.path.join(HERE, "workloads")
JS = os.path.join(WS, "protoJS/benchmarks/structures")
MAX_LOAD = float(os.environ.get("MAX_LOAD", "4.0"))

SIDES = {
    "before": {"bin": ROOT + "/base/root/usr/bin", "rtlib": ROOT + "/base/root/usr/lib/x86_64-linux-gnu",
               "inst": ROOT + "/base/lib210inst", "rel": ROOT + "/base/lib210"},
    "after": {"bin": ROOT + "/new/root/usr/bin", "rtlib": ROOT + "/new/root/usr/lib/x86_64-linux-gnu",
              "inst": ROOT + "/new/lib2141inst", "rel": ROOT + "/new/lib2141"},
}

BUSY = ("cc1plus", "cc1", "ctest", "proto_tests", "ld", "collect2", "make", "ninja",
        "cmake", "cpack", "test262_runner")


QUIET_CORES = float(os.environ.get("QUIET_CORES", "2.5"))


def busy_cores(seconds=3.0):
    """CPUs in use over the last `seconds`, from /proc/stat: what the machine
    runs now (the 1-minute load average lags a run that just ended by
    minutes)."""
    def snap():
        f = open("/proc/stat").readline().split()[1:]
        v = [int(x) for x in f]
        idle = v[3] + v[4]
        return sum(v), idle
    t0, i0 = snap()
    time.sleep(seconds)
    t1, i1 = snap()
    ncpu = os.cpu_count() or 1
    return (1.0 - (i1 - i0) / float(max(1, t1 - t0))) * ncpu


def quiet_gate():
    """Waits until no compiler, linker or test suite runs and fewer than
    QUIET_CORES CPUs are busy (default 2.5: on the measuring desktop an
    editor's GPU process and a browser take about 1.5 CPUs at all times).
    Records the load average, the busy CPUs and the two busiest processes."""
    while True:
        comms = subprocess.run(["ps", "-eo", "comm"], capture_output=True, text=True).stdout.split()
        busy = sorted(set(c for c in comms if c in BUSY))
        cores = busy_cores()
        load = os.getloadavg()[0]
        if not busy and cores <= QUIET_CORES:
            top = subprocess.run(["ps", "-eo", "pcpu,comm", "--sort=-pcpu"], capture_output=True,
                                 text=True).stdout.splitlines()[1:3]
            return load, [t.strip() for t in top] + ["busy_cpus=%.2f" % cores]
        print("  waiting: busy=%s cpus=%.2f load=%.2f" % (busy, cores, load), flush=True)
        time.sleep(10)


def parse_phases(err):
    last = None
    first_warm = None
    for line in err.splitlines():
        if line.startswith("[GC-PHASES]"):
            last = line
    d = {}
    if last:
        for k, v in re.findall(r"(\w+)=(\d+)", last):
            d[k] = int(v)
    return d


def warm_window(err, limit):
    """Cumulative counters at the first cycle by which the collector had freed
    3 x the heap limit: last - warm is the run's steady state on an aged heap."""
    if not limit:
        return None
    for line in err.splitlines():
        if line.startswith("[GC-PHASES]"):
            d = dict((k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)", line))
            if d.get("freed_cells", 0) >= 3 * limit:
                return d
    return None


def heap_trace(err):
    cyc = fixed = 0
    finalS = None
    for line in err.splitlines():
        if not line.startswith("protoCore heap:"):
            continue
        if " fixed " in line:
            fixed += 1
            continue
        cyc += 1
        m = re.search(r"S=\d+->(\d+)", line)
        if m:
            finalS = int(m.group(1))
    return cyc, fixed, finalS


def execute(rec, cmd, env_extra, lib, mem, cwd, check, timeout=3600, trace=None):
    load, top = quiet_gate()
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("PROTOCORE_") or k.startswith("PROTOST_") or k.startswith("PROTOPY"):
            del env[k]
    env["LD_LIBRARY_PATH"] = lib
    if trace == "gc":
        env["PROTOCORE_GC_PROFILE"] = "1"
    elif trace == "heap":
        env["PROTOCORE_HEAP_TRACE"] = "1"
    env.update({k: str(v) for k, v in env_extra.items()})
    full = ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=" + mem, "-p", "MemorySwapMax=0",
            "/usr/bin/time", "-f", "TIME wall=%e user=%U sys=%S rss_kb=%M", "timeout", str(timeout)] + cmd
    p = subprocess.run(full, env=env, capture_output=True, text=True, cwd=cwd, errors="replace")
    m = re.search(r"TIME wall=([\d.]+) user=([\d.]+) sys=([\d.]+) rss_kb=(\d+)", p.stderr)
    rec.update({"exit": p.returncode, "env": env_extra, "mem": mem, "load1_before": load, "top_before": top})
    if m:
        rec.update(wall_s=float(m.group(1)), user_s=float(m.group(2)), sys_s=float(m.group(3)),
                   rss_kb=int(m.group(4)))
    try:
        ok = bool(p.returncode == 0 and check(p.stdout, p.stderr))
    except Exception as e:  # a malformed output is a failed run, never a data point
        ok = False
        rec["check_error"] = repr(e)
    rec["ok"] = ok
    if trace == "gc":
        rec["gc"] = parse_phases(p.stderr)
        rec["gc_warm"] = warm_window(p.stderr, int(env_extra.get("PROTOCORE_HEAP_LIMIT_CELLS", 0) or 0))
        rec["lib_loaded"] = "[GC-PHASES]" in p.stderr
    elif trace == "heap":
        rec["adaptive_cycles"], rec["fixed_cycles"], rec["finalS"] = heap_trace(p.stderr)
    rec["json"] = [l for l in p.stdout.splitlines() if l.startswith("{")][-1:]
    rec["stdout_tail"] = p.stdout[-300:]
    if not ok:
        rec["stderr_tail"] = p.stderr[-1200:]
    return rec


def js_ok(out, err):
    for line in out.splitlines():
        if line.startswith("{"):
            j = json.loads(line)
            # Each task's checksum differs by task; the tabulator checks that
            # task k's checksum is the same in every run of a workload.
            return j.get("ok") is True and len(j.get("checksums", [])) == j.get("n")
    return False


def cad_ok(out, err):
    for line in out.splitlines():
        if line.startswith("{"):
            return json.loads(line).get("ok") is True
    return False


# ---------------------------------------------------------------------------
# gc: the phase-breakdown workloads
# ---------------------------------------------------------------------------

def gc_workloads():
    W = []
    for wl in ["records", "join", "doctree", "wordfreq", "graph"]:
        for n in [1, 6, 12]:
            W.append(("js40_%s_n%d" % (wl, n), "protojs", ["protojs_bench.js"],
                      {"WORKLOAD": wl, "MODE": "par", "N": n, "REPS": 3, "SCALE": 1,
                       "PROTOCORE_HEAP_LIMIT_CELLS": 40000000}, "6G", JS, js_ok, 1800))
    for wl in ["records", "join", "doctree", "wordfreq", "graph"]:
        for n in [1, 6, 12]:
            W.append(("js10_%s_n%d" % (wl, n), "protojs", ["protojs_bench.js"],
                      {"WORKLOAD": wl, "MODE": "par", "N": n, "REPS": 1, "SCALE": 1,
                       "PROTOCORE_HEAP_LIMIT_CELLS": 10000000}, "4G", JS, js_ok, 1200))
    for wl in ["records", "join", "doctree", "wordfreq", "graph"]:
        W.append(("jsad_%s_n12" % wl, "protojs", ["protojs_bench.js"],
                  {"WORKLOAD": wl, "MODE": "par", "N": 12, "REPS": 3, "SCALE": 1,
                   "PROTOCORE_HEAP_LIMIT_CELLS": 40000000, "PROTOCORE_ADAPTIVE_HEAP": 1}, "6G", JS, js_ok, 1800))
    sok = lambda o, e: "checksum=540451801" in o and o.strip().endswith("ok")
    for t in [1, 6]:
        W.append(("scala_tree_t%d" % t, "protoscala", [WL + "/tree_alloc.scala"],
                  {"T": t, "BATCHES": 100, "PROTOCORE_HEAP_LIMIT_CELLS": 2000000}, "4G", WL, sok, 1800))
    R = 1500
    expect = 6001000 * R + 1000 * R * (R - 1)
    cok = lambda o, e: ("checksum %d" % expect) in o and o.strip().endswith("ok")
    for t in [1, 6]:
        path = WL + "/coll_alloc_r%d_t%d.clj" % (R, t)
        if not os.path.exists(path):
            src = open(WL + "/coll_alloc.clj.in").read().replace("@ROUNDS@", str(R)).replace(
                "@TASKS@", "[" + " ".join(str(i) for i in range(t)) + "]")
            open(path, "w").write(src)
        W.append(("clj_coll_t%d" % t, "protoclj", [path], {"PROTOCORE_HEAP_LIMIT_CELLS": 2000000},
                  "4G", WL, cok, 1800))
    W.append(("cad_20k", "protojs", ["protojs_cad.js"],
              {"PARTS": 20000, "NS": "1,12", "REPS": 1, "PROTOCORE_HEAP_LIMIT_CELLS": 16000000},
              "3G", JS + "/cad", cad_ok, 1800))
    return W


def core_workloads():
    ok = lambda o, e: "verified=yes" in o
    return [("core_fixed640_live1M", ["fixed", "10485760", "1000000"], {}, ok),
            ("core_adaptive_live1M", ["adaptive", "1000000"], {}, ok)]


def run_gc(res, reps, globs):
    out = open(res, "a")
    for rep in range(reps):
        for name, args, env, ok in core_workloads():
            if not any(fnmatch.fnmatch(name, g) for g in globs):
                continue
            for side in ("before", "after"):
                s = SIDES[side]
                rec = {"group": "gc", "name": name, "side": side, "rep": rep}
                execute(rec, [s["inst"] + "/adaptive_heap_benchmark"] + args, env, s["inst"], "10G", HERE, ok,
                        trace="gc")
                emit(out, rec)
        for name, binname, args, env, mem, cwd, ok, to in gc_workloads():
            if not any(fnmatch.fnmatch(name, g) for g in globs):
                continue
            for side in ("before", "after"):
                s = SIDES[side]
                rec = {"group": "gc", "name": name, "side": side, "rep": rep}
                execute(rec, [s["bin"] + "/" + binname] + args, env, s["inst"] + ":" + s["rtlib"], mem, cwd,
                        ok, timeout=to, trace="gc")
                emit(out, rec)


# ---------------------------------------------------------------------------
# cal: the calibration workloads, default policy vs the controller
# ---------------------------------------------------------------------------

def expect_line(cwd, script):
    src = open(os.path.join(cwd, script)).read()
    m = re.search(r"//\s*EXPECT:\s*(.*)", src)
    want = m.group(1).strip()

    def ok(o, e):
        lines = [l for l in o.strip().splitlines() if l.strip()]
        return bool(lines) and lines[-1].strip() == want
    return ok


def rx(pattern):
    return lambda o, e: re.search(pattern, o) is not None


def cal_workloads():
    W = []
    for live in (0, 100000, 1000000, 5000000):
        W.append(("core/ahb-%dk" % (live // 1000), "@ahb", ["fixed", "10485760", str(live)], {}, HERE,
                  rx(r"verified=yes")))
    st = WS + "/protoST"
    for f, exp in [("benchmarks/actors/saturation_big.st", "VERIFIED 20004000000"),
                   ("benchmarks/actors/parallel_speedup.st", "VERIFIED 135000900000"),
                   ("benchmarks/comparable/list_append.st", "VERIFIED 10000"),
                   ("benchmarks/comparable/str_concat.st", "VERIFIED 2000"),
                   ("benchmarks/comparable/fib.st", "VERIFIED 75025"),
                   ("benchmarks/actors/message_throughput.st", "VERIFIED 2000")]:
        W.append(("st/" + os.path.basename(f)[:-3], "protost", [f], {"PROTOST_WORKERS": 4}, st, rx(exp)))
    py = WS + "/protoPython"
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
        W.append(("py/" + name, "protopy", ["benchmarks/" + f] + args, env, py, rx(pat)))
    clj = WS + "/protoClojure"
    for f, exp in [("fib.clj", r"832040"), ("tak.clj", r"\S"), ("sum-loop.clj", r"\S"),
                   ("reduce-list.clj", r"\S"), ("sum-squares.clj", r"\S"),
                   ("actor-saturation-32.clj", r"(?m)^ok$")]:
        W.append(("clj/" + f[:-4], "protoclj", ["benchmarks/" + f], {"PROTOCLJ_ACTOR_WORKERS": 4}, clj, rx(exp)))
    sc = WS + "/protoScala"
    for f in ["fib30", "list_ops", "map_build", "object_tree", "str_concat", "tak", "sum_loop"]:
        script = "benchmarks/comparable/%s.scala" % f
        W.append(("scala/" + f, "protoscala", [script], {}, sc, expect_line(sc, script)))
    W.append(("scala/actor-saturation-32", "protoscala", ["benchmarks/actors/actor-saturation-32.scala"],
              {"PROTOSCALA_ACTOR_WORKERS": 4}, sc, rx(r"(?m)^ok$")))
    for wl in ["records", "doctree", "graph", "join", "wordfreq"]:
        W.append(("js/%s-seq" % wl, "protojs", ["protojs_bench.js"],
                  {"WORKLOAD": wl, "MODE": "seq", "N": 6, "REPS": 3, "SCALE": 1}, JS, rx(r'"ok":true')))
    W.append(("js/records-par", "protojs", ["protojs_bench.js"],
              {"WORKLOAD": "records", "MODE": "par", "N": 6, "REPS": 3, "SCALE": 1}, JS, rx(r'"ok":true')))
    W.append(("js/probe-100k", "protojs", [WL + "/probe100k.js"], {}, WL,
              rx("objects 100000 sumId 9999900000 sumX 2499975000 n7 1000")))
    return W


CAL_VARIANTS = {
    "before-default": ("before", {}),
    "after-default": ("after", {}),
    "after-adaptive": ("after", {"PROTOCORE_ADAPTIVE_HEAP": 1}),
    "before-adaptive": ("before", {"PROTOCORE_ADAPTIVE_HEAP": 1}),
}


def run_cal(res, reps, globs, variants):
    out = open(res, "a")
    for name, binname, args, env, cwd, ok in cal_workloads():
        if not any(fnmatch.fnmatch(name, g) for g in globs):
            continue
        for rep in range(reps):
            for vn in variants:
                side, venv = CAL_VARIANTS[vn]
                s = SIDES[side]
                e = dict(env)
                e.update(venv)
                exe = s["rel"] + "/adaptive_heap_benchmark" if binname == "@ahb" else s["bin"] + "/" + binname
                rec = {"group": "cal", "name": name, "variant": vn, "side": side, "rep": rep}
                execute(rec, [exe] + args, e, s["rel"] + ":" + s["rtlib"], "10G", cwd, ok, timeout=900,
                        trace="heap")
                emit(out, rec)


# ---------------------------------------------------------------------------
# policy: the phase-breakdown workloads under each runtime's own default
# policy (no PROTOCORE_HEAP_LIMIT_CELLS: protoScala and protoClojure set no
# limit, protoJS 75 % of the memory available) against the controller
# (PROTOCORE_ADAPTIVE_HEAP=1, its default budget H = 75 % of the cap), after
# side only, release library, 16 GB cap.
# ---------------------------------------------------------------------------

def policy_workloads():
    W = []
    sok = lambda o, e: "checksum=540451801" in o and o.strip().endswith("ok")
    for t in [1, 6]:
        W.append(("scala_tree_t%d" % t, "protoscala", [WL + "/tree_alloc.scala"], {"T": t, "BATCHES": 100}, WL, sok))
    R = 1500
    expect = 6001000 * R + 1000 * R * (R - 1)
    cok = lambda o, e: ("checksum %d" % expect) in o and o.strip().endswith("ok")
    for t in [1, 6]:
        W.append(("clj_coll_t%d" % t, "protoclj", [WL + "/coll_alloc_r%d_t%d.clj" % (R, t)], {}, WL, cok))
    for wl in ["records", "wordfreq", "graph"]:
        W.append(("js_%s_n12" % wl, "protojs", ["protojs_bench.js"],
                  {"WORKLOAD": wl, "MODE": "par", "N": 12, "REPS": 3, "SCALE": 1}, JS, js_ok))
    return W


def run_policy(res, reps, globs):
    out = open(res, "a")
    for name, binname, args, env, cwd, ok in policy_workloads():
        if not any(fnmatch.fnmatch(name, g) for g in globs):
            continue
        for rep in range(reps):
            for vn, venv in (("after-default", {}), ("after-adaptive", {"PROTOCORE_ADAPTIVE_HEAP": 1})):
                s = SIDES["after"]
                e = dict(env)
                e.update(venv)
                rec = {"group": "cal", "name": "policy/" + name, "variant": vn, "side": "after", "rep": rep}
                execute(rec, [s["bin"] + "/" + binname] + args, e, s["rel"] + ":" + s["rtlib"], "16G", cwd, ok,
                        timeout=1800, trace="heap")
                emit(out, rec)


# ---------------------------------------------------------------------------
# diag: one workload that got slower (clj_coll_t1), with 2.14.1's mechanisms
# switched off one at a time (instrumented library, interleaved).
# ---------------------------------------------------------------------------

DIAG_VARIANTS = [
    ("before", "before", {}),
    ("after", "after", {}),
    ("after-no-pacing", "after", {"PROTOCORE_GC_PACING": 0}),
    ("after-no-helpers", "after", {"PROTOCORE_GC_SWEEP_THREADS": 0}),
    ("after-1-chain-no-prefetch", "after", {"PROTOCORE_GC_SWEEP_CURSORS": 1, "PROTOCORE_GC_SWEEP_PREFETCH": 0}),
    ("after-8-chains-no-prefetch", "after", {"PROTOCORE_GC_SWEEP_CURSORS": 8, "PROTOCORE_GC_SWEEP_PREFETCH": 0}),
    ("after-1-chain-prefetch", "after", {"PROTOCORE_GC_SWEEP_CURSORS": 1, "PROTOCORE_GC_SWEEP_PREFETCH": 1}),
]


def run_diag(res, reps, globs):
    out = open(res, "a")
    R = 1500
    expect = 6001000 * R + 1000 * R * (R - 1)
    cok = lambda o, e: ("checksum %d" % expect) in o and o.strip().endswith("ok")
    for rep in range(reps):
        for vn, side, venv in DIAG_VARIANTS:
            if not any(fnmatch.fnmatch(vn, g) for g in globs):
                continue
            s = SIDES[side]
            e = {"PROTOCORE_HEAP_LIMIT_CELLS": 2000000}
            e.update(venv)
            rec = {"group": "gc", "name": "diag_clj_coll_t1/" + vn, "side": side, "rep": rep}
            execute(rec, [s["bin"] + "/protoclj", WL + "/coll_alloc_r%d_t1.clj" % R], e, s["inst"] + ":" + s["rtlib"],
                    "4G", WL, cok, timeout=900, trace="gc")
            emit(out, rec)


# ---------------------------------------------------------------------------
# rt6: six single-threaded programs per runtime, perf stat -r 3
# ---------------------------------------------------------------------------

def rt6_workloads():
    W = []
    st = WS + "/protoST"
    for f in ["fib", "list_append", "str_concat", "attr_lookup", "int_sum_loop", "range_iterate"]:
        W.append(("st/" + f, "protost", ["benchmarks/comparable/%s.st" % f], {}, st, r"VERIFIED \S+"))
    py = WS + "/protoPython"
    for name, f, args, env, pat in [
            ("int_sum_loop", "int_sum_loop.py", [], {"BENCH_N": "2000000"}, r"BENCH_RESULT.*"),
            ("list_append_loop", "list_append_loop.py", [], {"BENCH_N": "200000"}, r"BENCH_RESULT.*"),
            ("str_concat_loop", "str_concat_loop.py", [], {"BENCH_N": "20000"}, r"BENCH_RESULT.*"),
            ("attr_lookup", "attr_lookup.py", ["5000000"], {}, r"BENCH_RESULT.*"),
            ("call_recursion", "call_recursion.py", [], {"BENCH_N": "27"}, r"BENCH_RESULT.*"),
            ("richards_lite", "pyperf/bench_richards_lite.py", [], {}, r"richards.*")]:
        W.append(("py/" + name, "protopy", ["benchmarks/" + f] + args, env, py, pat))
    clj = WS + "/protoClojure"
    for f in ["fib", "tak", "sum-loop", "reduce-list", "sum-squares", "factorial-100"]:
        W.append(("clj/" + f, "protoclj", ["benchmarks/%s.clj" % f], {}, clj, r"(?s)\S.*"))
    sc = WS + "/protoScala"
    for f in ["fib30", "list_ops", "map_build", "object_tree", "str_concat", "tak"]:
        W.append(("scala/" + f, "protoscala", ["benchmarks/comparable/%s.scala" % f], {}, sc, "EXPECT"))
    for wl in ["records", "doctree", "graph", "join", "wordfreq"]:
        W.append(("js/%s-seq1" % wl, "protojs", ["protojs_bench.js"],
                  {"WORKLOAD": wl, "MODE": "seq", "N": 1, "REPS": 1, "SCALE": 1}, JS, r'"checksums":\[[^\]]*\]'))
    W.append(("js/probe-100k", "protojs", [WL + "/probe100k.js"], {}, WL, r"objects .*"))
    return W


def perf_stat(res, reps, globs):
    out = open(res, "a")
    for name, binname, args, env, cwd, pat in rt6_workloads():
        if not any(fnmatch.fnmatch(name, g) for g in globs):
            continue
        for rep in range(reps):
            outputs = {}
            for side in ("before", "after"):
                s = SIDES[side]
                load, top = quiet_gate()
                e = dict(os.environ)
                for k in list(e):
                    if k.startswith("PROTOCORE_"):
                        del e[k]
                e["LD_LIBRARY_PATH"] = s["rel"] + ":" + s["rtlib"]
                e["LC_ALL"] = "C"  # perf's -x output uses the locale's decimal separator
                e.update({k: str(v) for k, v in env.items()})
                csv = os.path.join(ROOT, "perf_tmp.csv")
                cmd = ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=10G", "-p", "MemorySwapMax=0",
                       "perf", "stat", "-r", "3", "-x", ",", "-o", csv, "-e", "instructions:u,cycles:u,task-clock",
                       s["bin"] + "/" + binname] + args
                p = subprocess.run(cmd, env=e, cwd=cwd, capture_output=True, text=True, errors="replace")
                if pat == "EXPECT":
                    ok = expect_line(cwd, args[0])(p.stdout.strip().splitlines()[-1] if p.stdout.strip() else "", "")
                    sig = p.stdout.strip().splitlines()[-1] if p.stdout.strip() else ""
                else:
                    m = re.findall(pat, p.stdout)
                    ok = bool(m)
                    sig = m[-1] if m else ""
                    # A benchmark that prints its own timing must not make the
                    # two sides differ: keep only the verification token.
                    sig = re.sub(r"\b(ms|sec|seconds|elapsed_ms|elapsed|time_ms|time)\s*[=:]\s*[\d.]+", "", sig)
                    sig = re.sub(r"\b\w+=[\d.]+\s*(ms|s)\b", "", sig)
                    sig = re.sub(r"(?<![=\w.])[\d.]+\s*(ms|s|sec|seconds)\b", "<t>", sig).strip()
                stats = {}
                for line in open(csv).read().splitlines():
                    f = line.split(",")
                    if len(f) > 3 and f[2] in ("instructions:u", "cycles:u", "task-clock"):
                        try:
                            stats[f[2]] = float(f[0])
                            stats[f[2] + "_rsd"] = f[3]
                        except ValueError:
                            stats[f[2]] = None
                rec = {"group": "rt6", "name": name, "side": side, "rep": rep, "exit": p.returncode,
                       "ok": ok and p.returncode == 0, "perf": stats, "signature": sig[-200:],
                       "load1_before": load, "top_before": top}
                if not rec["ok"]:
                    rec["stderr_tail"] = p.stderr[-800:]
                outputs[side] = sig
                emit(out, rec)
            same = outputs.get("before") == outputs.get("after")
            emit(out, {"group": "rt6", "name": name, "rep": rep, "outputs_agree": same,
                       "before_sig": outputs.get("before", "")[-120:], "after_sig": outputs.get("after", "")[-120:]})


# ---------------------------------------------------------------------------
# cells: cells per operation of the write-coalescing micro-cases
# ---------------------------------------------------------------------------

CELL_CASES = {
    # name: (binary, script template file, the switch that turns the
    # runtime's write groups off (the "after-off" side), sentinel regex)
    "js/ctor5": ("protojs", "cells_ctor5.js", {"PROTOJS_PUTFIELD_GROUPS": "off"}, r"done (\d+)"),
    "js/move2": ("protojs", "cells_move2.js", {"PROTOJS_PUTFIELD_GROUPS": "off"}, r"done (\d+)"),
    "py/init5": ("protopy", "cells_init5.py", {"PROTOPY_ATTR_GROUPS": "off"}, r"done (\d+)"),
    "st/setId5": ("protost", "cells_setid5.st", {"PROTOST_IVAR_GROUPS": "off"}, r"done (\d+)"),
    "scala/case5": ("protoscala", "cells_case5.scala", {"PROTOSCALA_FIELD_GROUPS": "off"}, r"done (\d+)"),
    "clj/assoc5": ("protoclj", "cells_assoc5.clj", None, r"done (\d+)"),
}


def run_cells(res, reps, globs, n1=100000, n2=400000):
    out = open(res, "a")
    tmp = os.path.join(ROOT, "cells_tmp")
    os.makedirs(tmp, exist_ok=True)
    for name, (binname, tmpl, offenv, pat) in CELL_CASES.items():
        if not any(fnmatch.fnmatch(name, g) for g in globs):
            continue
        for rep in range(reps):
            for side in ("before", "after", "after-off"):
                if side == "after-off" and offenv is None:
                    continue
                env = offenv if side == "after-off" else {}
                s = SIDES["after" if side == "after-off" else side]
                rss = {}
                okall = True
                cyc = {}
                for n in (n1, n2):
                    src = open(os.path.join(WL, tmpl)).read().replace("@N@", str(n))
                    path = os.path.join(tmp, "%d_%s" % (n, tmpl))
                    open(path, "w").write(src)
                    want = str(n)
                    rec = {}
                    e = dict(env)
                    # No collection during the measurement: a run with a cycle
                    # is rejected (the instrumented library prints a
                    # [GC-PHASES] line per cycle).
                    e["PROTOCORE_HEAP_LIMIT_CELLS"] = 400000000
                    execute(rec, [s["bin"] + "/" + binname, path], e, s["inst"] + ":" + s["rtlib"], "40G", tmp,
                            lambda o, er, want=want: (re.search(pat, o) or [None, None])[1] == want,
                            timeout=900, trace="gc")
                    rss[n] = rec.get("rss_kb")
                    cyc[n] = rec.get("gc", {}).get("cycles", 0)
                    okall = okall and rec["ok"] and cyc[n] == 0
                    if not rec["ok"]:
                        print("  failed:", name, side, n, rec.get("stderr_tail", "")[-300:], rec.get("stdout_tail"))
                cells = None
                if okall and rss[n1] and rss[n2]:
                    cells = (rss[n2] - rss[n1]) * 1024.0 / 64.0 / (n2 - n1)
                emit(out, {"group": "cells", "name": name, "side": side, "rep": rep, "ok": okall,
                           "rss_kb": rss, "cycles": cyc, "cells_per_op": cells, "n": [n1, n2]})


def emit(out, rec):
    out.write(json.dumps(rec) + "\n")
    out.flush()
    if rec.get("group") == "gc":
        g = rec.get("gc", {})
        sw = g.get("swept_cells", 0)
        print("%-22s %-6s r%d ok=%s wall=%s rss=%sMB cyc=%s wait=%.1fs sweep_ns/cell=%.1f load=%.1f" % (
            rec["name"], rec["side"], rec["rep"], rec["ok"], rec.get("wall_s"), rec.get("rss_kb", 0) // 1024,
            g.get("cycles"), g.get("headroom_wait", 0) / 1e6, (g.get("sweep", 0) * 1000.0 / sw) if sw else 0,
            rec["load1_before"]), flush=True)
    elif rec.get("group") == "cal":
        print("%-28s %-16s r%d ok=%s wall=%s rss=%sMB cyc=%s/%s load=%.1f" % (
            rec["name"], rec["variant"], rec["rep"], rec["ok"], rec.get("wall_s"), rec.get("rss_kb", 0) // 1024,
            rec.get("adaptive_cycles"), rec.get("fixed_cycles"), rec["load1_before"]), flush=True)
    else:
        print(json.dumps(rec)[:300], flush=True)


def main():
    group, res = sys.argv[1], sys.argv[2]
    reps = 3
    globs = ["*"]
    variants = ["before-default", "after-default", "after-adaptive", "before-adaptive"]
    a = sys.argv[3:]
    i = 0
    while i < len(a):
        if a[i] == "--reps":
            reps = int(a[i + 1]); i += 2
        elif a[i] == "--only":
            globs = a[i + 1].split(","); i += 2
        elif a[i] == "--variants":
            variants = a[i + 1].split(","); i += 2
        else:
            raise SystemExit("unknown argument " + a[i])
    if group == "gc":
        run_gc(res, reps, globs)
    elif group == "cal":
        run_cal(res, reps, globs, variants)
    elif group == "rt6":
        perf_stat(res, reps, globs)
    elif group == "cells":
        run_cells(res, reps, globs)
    elif group == "policy":
        run_policy(res, reps, globs)
    elif group == "diag":
        run_diag(res, reps, globs)
    else:
        raise SystemExit("unknown group " + group)


if __name__ == "__main__":
    main()
