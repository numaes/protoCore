#!/usr/bin/env python3
"""Tables of the cross-runtime re-measurement (cross_run.sh outputs).

usage: xrt_tab.py <outdir> [rounds, e.g. 1,2]

Per runtime and workload: the median over rounds of the runtime's time with
protoCore 2.14.6 (before) and 2.14.7 (after), the change, the spread of the
before side across rounds (max-min over median: the A/A noise floor of this
harness), the competitor's median over every run of both sides, and the
runtime/competitor ratio before and after (> 1: the competitor is faster).
"""
import glob, os, re, statistics as st, sys, collections

OUT = sys.argv[1]
ROUNDS = set(sys.argv[2].split(",")) if len(sys.argv) > 2 else None


def runs(prefix):
    for side in ("before", "after"):
        for path in sorted(glob.glob(os.path.join(OUT, "%s_%s_r*.*" % (prefix, side)))):
            r = re.search(r"_r(\d+)\.", path).group(1)
            if ROUNDS is None or r in ROUNDS:
                yield side, path


def table(name, comp_name, data):
    # data[workload][side] -> runtime ms per run; data[workload]["comp_" + side]
    # -> the competitor's ms in the same run.  The primary change is that of
    # the per-run ratio runtime/competitor (median per side): the harness
    # interleaves the competitor within each run, so a load change during the
    # session cancels out of it.  The raw medians are shown as well.
    print("\n### %s (competitor: %s)\n" % (name, comp_name))
    print("| Workload | 2.14.6 ms | 2.14.7 ms | %s ms | Ratio before | Ratio after | Change (ratio) | Change (raw) | A/A ratio spread |" % comp_name)
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for wl, d in data.items():
        b, a = d.get("before", []), d.get("after", [])
        cb, ca = d.get("comp_before", []), d.get("comp_after", [])
        if not b or not a:
            continue
        mb, ma = st.median(b), st.median(a)
        if not mb:
            continue    # below the harness's 1 ms resolution
        rb_l = [x / y for x, y in zip(b, cb) if y]
        ra_l = [x / y for x, y in zip(a, ca) if y]
        comp = [x for x in cb + ca if x]
        mc = st.median(comp) if comp else None
        if rb_l and ra_l:
            rb, ra = st.median(rb_l), st.median(ra_l)
            spread = (max(rb_l) - min(rb_l)) / rb * 100
            print("| %s | %.1f | %.1f | %.1f | %.2f | %.2f | %+.1f %% | %+.1f %% | %.1f %% |" % (
                wl, mb, ma, mc or 0, rb, ra, (ra / rb - 1) * 100, (ma / mb - 1) * 100, spread))
        else:
            print("| %s | %.1f | %.1f | - | - | - | - | %+.1f %% | - |" % (wl, mb, ma, (ma / mb - 1) * 100))


def num(s):
    try:
        return float(s)
    except ValueError:
        return None


# protoPython: run_4way_interleaved.py markdown
py = collections.OrderedDict()
pyc = collections.OrderedDict()
for side, p in runs("py"):
    if not p.endswith(".md"):
        continue
    for line in open(p):
        c = [x.strip() for x in line.split("|")]
        if len(c) < 9 or not c[1] or c[1].startswith("Benchmark") or c[1].startswith("-"):
            continue
        wl = c[1]
        cpt, pp, pc = num(c[2]), num(c[5]), num(c[7])
        if cpt is None:
            continue
        if pp is not None:
            py.setdefault(wl, {}).setdefault(side, []).append(pp)
            py[wl].setdefault("comp_" + side, []).append(cpt)
        if pc is not None:
            pyc.setdefault(wl, {}).setdefault(side, []).append(pc)
            pyc[wl].setdefault("comp_" + side, []).append(cpt)
table("protoPython, protopy (interpreter)", "CPython 3.14t", py)
table("protoPython, protopyc (AOT)", "CPython 3.14t", pyc)

# protoJS: run_standard_comparison.js text
js = collections.OrderedDict()
for side, p in runs("js"):
    if not p.endswith(".txt"):
        continue
    seen = set()
    for m in re.finditer(r"^(\w+): protoJS ([\d.]+) ms, Node ([\d.]+) ms", open(p).read(), re.M):
        wl = m.group(1)
        if wl in seen:      # json_transform prints three sub-results; keep the first
            continue
        seen.add(wl)
        js.setdefault(wl, {}).setdefault(side, []).append(float(m.group(2)))
        js[wl].setdefault("comp_" + side, []).append(float(m.group(3)) or None)
table("protoJS, standard suite (in-process ms, 1 ms resolution)", "Node 22", js)

# protoScala: run_benchmarks.py markdown
sc = collections.OrderedDict()
scj = collections.OrderedDict()
for side, p in runs("scala"):
    if not p.endswith(".md"):
        continue
    for line in open(p):
        m = re.match(r"\| `(\w+)` \| ([\d.]+) \[[^\]]*\] \| ([^|]+) \| ([\d.]+) \[", line)
        if not m:
            continue
        wl = m.group(1)
        sc.setdefault(wl, {}).setdefault(side, []).append(float(m.group(2)))
        sc[wl].setdefault("comp_" + side, []).append(float(m.group(4)))
        jv = re.match(r"\s*([\d.]+)", m.group(3))
        scj.setdefault(wl, {}).setdefault(side, []).append(float(m.group(2)))
        scj[wl].setdefault("comp_" + side, []).append(float(jv.group(1)) if jv else None)
table("protoScala (wall ms, cold start included)", "CPython 3.14", sc)
table("protoScala against the JVM (cold start included)", "Scala 3.9.0 / OpenJDK 25", scj)

# protoClojure: bench.sh text (best of 3)
cl = collections.OrderedDict()
for side, p in runs("clj"):
    for line in open(p):
        m = re.match(r"(\S+)\s+(\d+)\s+(\d+)\s+[\d.]+x\s+\S+\s+(\S+)", line)
        if not m:
            continue
        if m.group(4) != "✓":
            print("UNVERIFIED", p, line.strip())
            continue
        cl.setdefault(m.group(1), {}).setdefault(side, []).append(float(m.group(2)))
        cl[m.group(1)].setdefault("comp_" + side, []).append(float(m.group(3)))
table("protoClojure (wall ms, best of 3, cold start included)", "Babashka 1.4.192", cl)

# protoST: run_benchmarks.py text (median of 5)
stt = collections.OrderedDict()
for side, p in runs("st"):
    if not p.endswith(".log"):
        continue
    for line in open(p):
        m = re.match(r"\s+(\w+)\s+protoST\s+([\d.]+) ms\s+CPython\s+([\d.]+) ms", line)
        if not m:
            continue
        stt.setdefault(m.group(1), {}).setdefault(side, []).append(float(m.group(2)))
        stt[m.group(1)].setdefault("comp_" + side, []).append(float(m.group(3)))
table("protoST (wall ms, median of 5, cold start included)", "CPython 3.14", stt)

# protoJS structures: one run per side, protojs times against Node's in the
# same file (median of the three timed repetitions).
import json
sj = collections.OrderedDict()
for side in ("before", "after"):
    path = os.path.join(OUT, "structures_%s.jsonl" % side)
    if not os.path.exists(path):
        continue
    node = {}
    rows = [json.loads(l) for l in open(path) if l.startswith("{")]
    for r in rows:
        if r.get("runtime") == "node" and r.get("completed"):
            node[(r["workload"], r["mode"], r["n"])] = r["median_ms"]
    for r in rows:
        if r.get("runtime") != "protojs":
            continue
        key = "%s %s N=%d" % (r["workload"], r["mode"], r["n"])
        if not r.get("completed"):
            print("INCOMPLETE", side, key)
            continue
        d = sj.setdefault(key, {})
        d.setdefault(side, []).append(float(r["median_ms"]))
        d.setdefault("comp_" + side, []).append(node.get((r["workload"], r["mode"], r["n"])))
table("protoJS structures (median ms of 3; one run per side)", "Node 22", sj)
