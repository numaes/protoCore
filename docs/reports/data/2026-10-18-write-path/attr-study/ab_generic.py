#!/usr/bin/env python3
"""Interleaved A/B timing: current attribute-cache index (A) vs Fibonacci hash (B).

Each workload runs REPS times per variant, alternating A,B,A,B... so drift in
machine state hits both equally.  perf stat counts cycles, instructions and
task-clock across all threads; the workload's own stdout is compared between
variants so a broken run cannot pass as a fast one.
"""
import os, statistics, subprocess, sys

S = "/tmp/claude-1000/-home-gamarino-Documentos-proyectos/9cb98613-5533-4787-ae0f-b301a4c38554/scratchpad"
P = "/home/gamarino/Documentos/proyectos"
LIBS = {"A": f"{S}/bld", "B": os.environ.get("BLIB", f"{S}/bld_exp")}
REPS = int(os.environ.get("REPS", "5"))
JS = {"MODE": "seq", "N": "1", "REPS": "1", "PROTOCORE_HEAP_LIMIT_CELLS": "40000000"}

W = [
    ("py:richards", f"{P}/protoPython", ["protopy", "--path", "benchmarks", "--script", "benchmarks/pyperf/bench_richards_lite.py", "2000"], {}),
    ("py:binary_trees", f"{P}/protoPython", ["protopy", "--path", "benchmarks", "--script", "benchmarks/pyperf/bench_binary_trees.py"], {}),
    ("py:fannkuch", f"{P}/protoPython", ["protopy", "--path", "benchmarks", "--script", "benchmarks/pyperf/bench_fannkuch.py"], {}),
    ("py:nqueens", f"{P}/protoPython", ["protopy", "--path", "benchmarks", "--script", "benchmarks/pyperf/bench_nqueens.py", "10"], {}),
] + [
    (f"js:{w}", f"{P}/protoJS/benchmarks/structures", ["protojs", "protojs_bench.js"], dict(JS, WORKLOAD=w))
    for w in ("records", "join", "doctree", "wordfreq", "graph")
] + [
    ("scala:object_tree", f"{P}/protoScala", ["protoscala", "benchmarks/comparable/object_tree.scala"], {}),
    ("scala:attr_lookup", f"{P}/protoScala", ["protoscala", "benchmarks/comparable/attr_lookup.scala"], {}),
    ("clj:tak", f"{P}/protoClojure", ["protoclj", "benchmarks/tak.clj"], {}),
    ("clj:fib", f"{P}/protoClojure", ["protoclj", "benchmarks/fib.clj"], {}),
    ("st:attr_lookup", f"{P}/protoST", ["protost", "benchmarks/comparable/attr_lookup.st"], {}),
    ("st:json_transform", f"{P}/protoST", ["protost", "examples/programs/json_transform.st"], {}),
    ("micro:own+inh", S, None, {}),   # attribute_lookup_benchmark, built per variant
]

VOLATILE = ("ms", "min=", "median", "times_ms", "build_ms", "peak_rss", "ns/op")


def stable_output(text):
    """Output lines with timing fields removed, for A/B result comparison."""
    out = []
    for line in text.splitlines():
        if any(v in line for v in VOLATILE):
            if line.startswith("{"):          # protojs JSON: keep checksums only
                i = line.find('"checksums"')
                out.append(line[i:line.find("]", i) + 1] if i >= 0 else "")
            elif "checksum ok" in line:
                out.append(line.split()[0] + " ok")
            continue
        out.append(line)
    return "\n".join(out)


def run(variant, cwd, cmd, env_extra, name):
    env = dict(os.environ, LD_LIBRARY_PATH=LIBS[variant], LC_ALL="C", **env_extra)
    if cmd is None:
        cmd = [f"{LIBS[variant]}/attribute_lookup_benchmark", os.environ.get("MICRO1", "own")]
        cmd2 = [f"{LIBS[variant]}/attribute_lookup_benchmark", os.environ.get("MICRO2", "inh")]
        cmd = ["sh", "-c", " ".join(cmd) + " && " + " ".join(cmd2)]
    lib = subprocess.run(["sh", "-c", f"ldd $(command -v {cmd[0] if cmd[0] != 'sh' else LIBS[variant] + '/attribute_lookup_benchmark'}) | grep libprotoCore"],
                         env=env, capture_output=True, text=True).stdout
    if LIBS[variant] not in lib:
        sys.exit(f"{name} {variant}: wrong libprotoCore: {lib.strip()}")
    r = subprocess.run(["perf", "stat", "-x", ";", "-e", "cycles,instructions,task-clock", "--"] + cmd,
                       cwd=cwd, env=env, capture_output=True, text=True, timeout=900)
    if r.returncode != 0:
        sys.exit(f"{name} {variant}: exit {r.returncode}\n{r.stderr[-2000:]}")
    counters = {}
    for line in r.stderr.splitlines():
        parts = line.split(";")
        for key in ("cycles", "instructions", "task-clock"):
            if len(parts) > 3 and parts[2].split(":")[0].endswith(key):
                try:
                    counters[key] = counters.get(key, 0.0) + float(parts[0])
                except ValueError:
                    pass
    return counters, stable_output(r.stdout)


def main():
    only = sys.argv[1:] or None
    print(f"# REPS={REPS}, interleaved A/B; A=current index, B=$BLIB")
    print(f"{'workload':20} {'cyc A (G)':>10} {'cyc B (G)':>10} {'dCyc':>7} {'spreadA':>8} {'spreadB':>8} {'dInstr':>7} {'dCPU':>7}")
    for name, cwd, cmd, env in W:
        if only and name not in only:
            continue
        res = {"A": [], "B": []}
        outs = {}
        for _ in range(REPS):
            for v in ("A", "B"):
                c, o = run(v, cwd, cmd, env, name)
                res[v].append(c)
                outs.setdefault(v, o)
                if o != outs[v]:
                    sys.exit(f"{name} {v}: output changed between reps")
        if outs["A"] != outs["B"]:
            sys.exit(f"{name}: A and B outputs differ\nA:\n{outs['A']}\nB:\n{outs['B']}")
        med = {v: {k: statistics.median(r[k] for r in res[v]) for k in res[v][0]} for v in res}
        spread = {v: (max(r["cycles"] for r in res[v]) - min(r["cycles"] for r in res[v])) / med[v]["cycles"] * 100 for v in res}
        d = lambda k: (med["B"][k] / med["A"][k] - 1) * 100
        print(f"{name:20} {med['A']['cycles']/1e9:10.3f} {med['B']['cycles']/1e9:10.3f} {d('cycles'):+6.1f}% "
              f"{spread['A']:7.1f}% {spread['B']:7.1f}% {d('instructions'):+6.1f}% {d('task-clock'):+6.1f}%", flush=True)


if __name__ == "__main__":
    main()
