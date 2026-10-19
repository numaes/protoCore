#!/usr/bin/env python3
"""protoClojure against Clojure 1.12.0 on the JVM (Java 25) and Babashka
1.4.192, on protoClojure's own benchmark programs (benchmarks/bench.sh's
workloads).  Per workload and round, interleaved, each run behind the quiet
gate:
  protoclj, bb, JVM   whole process (start-up included), as bench.sh measures;
  JVM steady state    the program loaded 5 times in one process (load-file,
                      output captured); the best of iterations 2-5.
Every whole-process run must print the same result.

usage: clj_jvm.py <results.jsonl> <jar dir> [rounds]
"""
import glob, json, os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "../2026-10-12-mark-stack-int-compare"))
import fm  # quiet_gate

WS = os.environ.get("PROTO_WORKSPACE", os.path.abspath(os.path.join(HERE, "../../../../..")))
BENCH = os.path.join(WS, "protoClojure", "benchmarks")
PROTOCLJ = os.path.join(WS, "protoClojure", "build_cmp2146", "protoclj")  # + the 2.14.7 library (fresh_env)
BB = os.path.join(WS, "tools", "bb-1.4.192", "bb")
WORKLOADS = ["fib", "tak", "sum-loop", "reduce-list", "sum-squares"]


STAGE = os.path.join(WS, ".agent_scratch", "installers-oct2026")


def fresh_env():
    """protoclj loads a fresh copy of the 2.14.7 library (see cross_run.sh, fresh())."""
    import shutil
    d = os.path.join(os.environ.get("PROBE_LIBS", "/tmp/probe-libs"), "clj_%d" % time.time_ns())
    os.makedirs(d)
    shutil.copy(os.path.join(STAGE, "stage2147/usr/lib/x86_64-linux-gnu/libprotoCore.so.3"), d)
    return dict(os.environ, LD_LIBRARY_PATH=d)


def wall(cmd, env=None):
    t = time.perf_counter()
    p = subprocess.run(cmd, cwd=BENCH, capture_output=True, text=True, timeout=600, env=env)
    return time.perf_counter() - t, p.returncode, (p.stdout.strip().splitlines() or [""])[-1]


def steady(cp, wl):
    expr = ('(dotimes [i 5] (let [t (System/nanoTime)] (with-out-str (load-file "%s.clj")) '
            '(println "ITER" i (/ (- (System/nanoTime) t) 1e6))))' % wl)
    p = subprocess.run(["java", "-cp", cp, "clojure.main", "-e", expr], cwd=BENCH,
                       capture_output=True, text=True, timeout=900)
    its = [float(m) for m in re.findall(r"ITER \d+ ([\d.]+)", p.stdout)]
    return (min(its[1:]) / 1e3 if len(its) == 5 else None), its


def main():
    out = open(sys.argv[1], "a")
    jars = sys.argv[2]
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    cp = ":".join(sorted(glob.glob(os.path.join(jars, "clojure-*.jar")) + glob.glob(os.path.join(jars, "*spec*.jar"))))
    for r in range(rounds):
        for wl in WORKLOADS:
            rec = {"workload": wl, "round": r}
            results = set()
            for name, cmd in (("protoclj", [PROTOCLJ, wl + ".clj"]), ("bb", [BB, wl + ".clj"]),
                              ("jvm", ["java", "-cp", cp, "clojure.main", wl + ".clj"])):
                fm.quiet_gate()
                s, rc, res = wall(cmd, fresh_env() if name == "protoclj" else None)
                rec[name + "_s"] = s
                rec[name + "_ok"] = rc == 0
                results.add(res)
            fm.quiet_gate()
            rec["jvm_steady_s"], rec["jvm_iters_ms"] = steady(cp, wl)
            rec["result"] = sorted(results)
            rec["agree"] = len(results) == 1
            out.write(json.dumps(rec) + "\n"); out.flush()
            print(json.dumps(rec), flush=True)


if __name__ == "__main__":
    main()
