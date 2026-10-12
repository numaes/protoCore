#!/usr/bin/env python3
"""Tabulates fm.py's records: medians per workload and side, before -> after.

usage: tab.py <group> <results.jsonl> [more.jsonl ...]
Only verified runs (ok=true) are data points; the count of failed runs is
printed beside each row.  For the protoJS workloads, task k's checksum must
be the same in every verified run of a workload, before and after; a
mismatch is printed as an error.
"""
import json, re, statistics, sys
from collections import defaultdict


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def pct(a, b):
    if a is None or b is None or a == 0:
        return "-"
    return "%+.1f %%" % ((b - a) * 100.0 / a)


def f1(x, nd=1):
    return "-" if x is None else ("%.*f" % (nd, x))


def threads_of(name):
    m = re.search(r"_n(\d+)$", name) or re.search(r"_t(\d+)$", name)
    if m:
        return int(m.group(1))
    if name.startswith("cad"):
        return 12
    return 1


def load(paths):
    recs = []
    for p in paths:
        for line in open(p):
            line = line.strip()
            if line:
                recs.append(json.loads(line))
    return recs


def gc_table(recs):
    rows = defaultdict(lambda: defaultdict(list))
    fails = defaultdict(int)
    sums = defaultdict(dict)
    order = []
    for r in recs:
        if r.get("group") != "gc":
            continue
        key = r["name"]
        if key not in order:
            order.append(key)
        if not r.get("ok"):
            fails[(key, r["side"])] += 1
            continue
        if r.get("json"):
            j = json.loads(r["json"][0])
            cs = j.get("checksums")
            if cs:
                prev = sums[key].get("cs")
                if prev is not None and prev != cs:
                    print("ERROR: checksum mismatch", key, r["side"], r["rep"], file=sys.stderr)
                sums[key]["cs"] = cs
        g = r.get("gc", {})
        th = threads_of(key)
        d = {"wall": r.get("wall_s"), "rss": r.get("rss_kb", 0) / 1024.0, "cycles": g.get("cycles", 0),
             "cpu": (r.get("user_s") or 0) + (r.get("sys_s") or 0),
             "wait": (g.get("headroom_wait", 0) / 1e6) / (r["wall_s"] * th) * 100.0 if r.get("wall_s") else None,
             "sweep_ns": (g["sweep"] * 1000.0 / g["swept_cells"]) if g.get("swept_cells") else None,
             "busy": g.get("busy", 0) / 1e6, "gc_cpu": g.get("cpu_busy", 0) / 1e6,
             "sweep_s": g.get("sweep", 0) / 1e6, "mark_s": (g.get("young", 0) + g.get("trace", 0)) / 1e6,
             "stw_max_ms": g.get("stw_max", 0) / 1e3}
        fr, rc = g.get("refill_fresh"), g.get("refill_recycled")
        d["fresh"] = (fr * 100.0 / (fr + rc)) if fr is not None and (fr + rc) else None
        w = r.get("gc_warm")
        if w and g.get("swept_cells", 0) > w.get("swept_cells", 0):
            d["aged_sweep_ns"] = (g["sweep"] - w["sweep"]) * 1000.0 / (g["swept_cells"] - w["swept_cells"])
            if fr is not None and "refill_fresh" in w:
                df, dr = fr - w["refill_fresh"], rc - w["refill_recycled"]
                d["aged_fresh"] = df * 100.0 / (df + dr) if (df + dr) else None
        for k, v in d.items():
            rows[(key, r["side"])][k].append(v)
    print("| Workload | Wall s | Peak RSS MB | Cycles | Wait share | Sweep ns/cell | Collector CPU s | Process CPU s | runs ok (b/a) |")
    print("|---|---|---|---|---|---|---|---|---|")
    for key in order:
        b, a = rows[(key, "before")], rows[(key, "after")]
        nb, na = len(b["wall"]), len(a["wall"])
        def ba(k, nd=1, rel=True):
            x, y = med(b[k]), med(a[k])
            s = "%s -> %s" % (f1(x, nd), f1(y, nd))
            if rel and x is not None and y is not None:
                s += " (%s)" % pct(x, y)
            return s
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %d/%d%s |" % (
            key, ba("wall", 2), ba("rss", 0), ba("cycles", 0, False), ba("wait", 1, False) + " %",
            ba("sweep_ns", 1), ba("gc_cpu", 1), ba("cpu", 1), nb, na,
            "" if not (fails[(key, "before")] or fails[(key, "after")]) else
            " (failed %d/%d)" % (fails[(key, "before")], fails[(key, "after")])))
    print()
    print("Detail (after side): fresh share of refills, aged-window sweep, mark, max pause")
    print("| Workload | Fresh refills % (whole run) | Aged window: sweep ns/cell b -> a | fresh % | Mark s b -> a | Sweep s b -> a | STW max ms b -> a |")
    print("|---|---|---|---|---|---|---|")
    for key in order:
        b, a = rows[(key, "before")], rows[(key, "after")]
        print("| %s | %s | %s -> %s | %s | %s -> %s | %s -> %s | %s -> %s |" % (
            key, f1(med(a["fresh"])), f1(med(b.get("aged_sweep_ns", []))), f1(med(a.get("aged_sweep_ns", []))),
            f1(med(a.get("aged_fresh", []))), f1(med(b["mark_s"]), 2), f1(med(a["mark_s"]), 2),
            f1(med(b["sweep_s"]), 2), f1(med(a["sweep_s"]), 2), f1(med(b["stw_max_ms"]), 2),
            f1(med(a["stw_max_ms"]), 2)))


def cal_table(recs):
    rows = defaultdict(lambda: defaultdict(list))
    fails = defaultdict(int)
    order, variants = [], []
    for r in recs:
        if r.get("group") != "cal":
            continue
        if r["name"] not in order:
            order.append(r["name"])
        if r["variant"] not in variants:
            variants.append(r["variant"])
        if not r.get("ok"):
            fails[(r["name"], r["variant"])] += 1
            continue
        rows[(r["name"], r["variant"])]["wall"].append(r.get("wall_s"))
        rows[(r["name"], r["variant"])]["rss"].append(r.get("rss_kb", 0) / 1024.0)
        rows[(r["name"], r["variant"])]["cyc"].append((r.get("adaptive_cycles") or 0) + (r.get("fixed_cycles") or 0))
    print("| Workload | Default policy, before: s / MB / cycles | Default, after: s / MB / cycles | After vs before (time, memory) | Controller, after: s / MB / cycles | Controller vs default, after (time, memory) | Controller, before (2.10.2): s / MB |")
    print("|---|---|---|---|---|---|---|")
    for n in order:
        def cell(v):
            x = rows[(n, v)]
            if not x["wall"]:
                return "FAILED" if fails[(n, v)] else "-"
            return "%s / %s / %s" % (f1(med(x["wall"]), 2), f1(med(x["rss"]), 0), f1(med(x["cyc"]), 0))
        bd, ad, aa, ba_ = (rows[(n, v)] for v in ("before-default", "after-default", "after-adaptive", "before-adaptive"))
        print("| %s | %s | %s | %s, %s | %s | %s, %s | %s |" % (
            n, cell("before-default"), cell("after-default"),
            pct(med(bd["wall"]), med(ad["wall"])), pct(med(bd["rss"]), med(ad["rss"])),
            cell("after-adaptive"), pct(med(ad["wall"]), med(aa["wall"])), pct(med(ad["rss"]), med(aa["rss"])),
            "%s / %s" % (f1(med(ba_["wall"]), 2), f1(med(ba_["rss"]), 0)) if ba_["wall"] else "-"))
    tot = sum(1 for r in recs if r.get("group") == "cal")
    bad = sum(1 for r in recs if r.get("group") == "cal" and not r.get("ok"))
    print("\n%d runs, %d failed verification" % (tot, bad))


def rt6_table(recs):
    rows = defaultdict(lambda: defaultdict(list))
    order = []
    agree = defaultdict(list)
    fails = defaultdict(int)
    for r in recs:
        if r.get("group") != "rt6":
            continue
        if "outputs_agree" in r:
            agree[r["name"]].append(r["outputs_agree"])
            continue
        if r["name"] not in order:
            order.append(r["name"])
        if not r.get("ok"):
            fails[(r["name"], r["side"])] += 1
            continue
        p = r["perf"]
        rows[(r["name"], r["side"])]["ins"].append(p.get("instructions:u"))
        rows[(r["name"], r["side"])]["cyc"].append(p.get("cycles:u"))
        rows[(r["name"], r["side"])]["ms"].append(p.get("task-clock"))
    print("| Program | instructions:u before -> after | cycles:u before -> after | task-clock ms before -> after | outputs agree | runs ok (b/a) |")
    print("|---|---|---|---|---|---|")
    for n in order:
        b, a = rows[(n, "before")], rows[(n, "after")]
        def g(k, scale, nd):
            x, y = med(b[k]), med(a[k])
            if x is None or y is None:
                return "-"
            return "%s -> %s (%s)" % (f1(x / scale, nd), f1(y / scale, nd), pct(x, y))
        print("| %s | %s | %s | %s | %s | %d/%d |" % (n, g("ins", 1e9, 3) + " G", g("cyc", 1e9, 3) + " G",
                                                 g("ms", 1, 0), "yes" if all(agree[n]) else "NO",
                                                 len(b["cyc"]), len(a["cyc"])))


def cells_table(recs):
    rows = defaultdict(list)
    order = []
    for r in recs:
        if r.get("group") != "cells":
            continue
        if r["name"] not in order:
            order.append(r["name"])
        if r.get("ok"):
            rows[(r["name"], r["side"])].append(r["cells_per_op"])
    print("| Case | Cells per operation, before | after | change | after, write groups off | runs ok (b/a/off) |")
    print("|---|---|---|---|---|---|")
    for n in order:
        b, a, o = med(rows[(n, "before")]), med(rows[(n, "after")]), med(rows[(n, "after-off")])
        print("| %s | %s | %s | %s | %s | %d/%d/%d |" % (n, f1(b), f1(a), pct(b, a), f1(o), len(rows[(n, "before")]),
                                                     len(rows[(n, "after")]), len(rows[(n, "after-off")])))




def st6_table(outdir):
    """perf stat -r 3 of protoCore's six benchmarks (st6.sh): median over the
    rounds of each side's mean; A = 2.10.2, B = 2.14.1."""
    import glob, os
    vals = defaultdict(lambda: defaultdict(list))
    for f in glob.glob(os.path.join(outdir, "perf_*_r*.csv")):
        m = re.match(r"perf_(.+)_([AB])_r(\d+)\.csv", os.path.basename(f))
        b, side = m.group(1), m.group(2)
        for line in open(f):
            p = line.strip().split(",")
            if len(p) > 3 and p[2] in ("instructions:u", "cycles:u"):
                vals[(b, side)][p[2]].append(float(p[0]))
            elif len(p) > 3 and p[2] == "task-clock":
                vals[(b, side)]["ms"].append(float(p[0]))
    print("| Benchmark | instructions:u 2.10.2 -> 2.14.1 | cycles:u 2.10.2 -> 2.14.1 | task-clock ms | rounds |")
    print("|---|---|---|---|---|")
    for b in ["microbenchmark_final", "mutable_access_benchmark", "cache_timing_benchmark",
              "hash_quality_benchmark", "object_access_benchmark", "immutable_sharing_benchmark"]:
        A, B = vals[(b, "A")], vals[(b, "B")]
        def g(k, sc, nd):
            x, y = med(A[k]), med(B[k])
            if x is None or y is None:
                return "-"
            return "%s -> %s (%s)" % (f1(x / sc, nd), f1(y / sc, nd), pct(x, y))
        print("| %s | %s | %s | %s | %d/%d |" % (b, g("instructions:u", 1e9, 3) + " G", g("cycles:u", 1e9, 3) + " G",
                                             g("ms", 1, 0), len(A["cycles:u"]), len(B["cycles:u"])))


def struct_table(outdir):
    """protoJS structure benchmarks (struct.sh): per workload and N, the
    in-process median of 3 repetitions, before -> after, seq and Deferreds."""
    import os
    data = {}
    for side in ("before", "after"):
        f = os.path.join(outdir, "structures_%s.jsonl" % side)
        if not os.path.exists(f):
            continue
        for line in open(f):
            r = json.loads(line)
            if r.get("runtime") != "protojs" or not r.get("completed", True):
                continue
            data[(r["workload"], r["n"], r["mode"], side)] = r
    wls = []
    for (w, n, m, sd) in data:
        if w not in wls:
            wls.append(w)
    print("| Workload | N | seq ms before -> after | Deferreds ms before -> after | speed-up (seq/par) before -> after | GC cycles (par) b -> a | peak RSS MB (par) b -> a |")
    print("|---|--:|---|---|---|---|---|")
    for w in ["records", "join", "doctree", "wordfreq", "graph"]:
        for n in (1, 2, 4, 6, 12):
            sb, sa = data.get((w, n, "seq", "before")), data.get((w, n, "seq", "after"))
            pb, pa = data.get((w, n, "par", "before")), data.get((w, n, "par", "after"))
            if not (sb or sa or pb or pa):
                continue
            def ms(a, b):
                if not a or not b:
                    return "%s -> %s" % (a and a["median_ms"], b and b["median_ms"])
                return "%d -> %d (%s)" % (a["median_ms"], b["median_ms"], pct(a["median_ms"], b["median_ms"]))
            def sp(s_, p_):
                return f1(s_["median_ms"] / float(p_["median_ms"]), 2) if s_ and p_ else "-"
            print("| %s | %d | %s | %s | %s -> %s | %s -> %s | %s -> %s |" % (
                w, n, ms(sb, sa), ms(pb, pa), sp(sb, pb), sp(sa, pa),
                pb and pb.get("gc_cycles"), pa and pa.get("gc_cycles"),
                pb and pb["peak_rss_kb"] // 1024, pa and pa["peak_rss_kb"] // 1024))



def policy_table(recs):
    rows = defaultdict(lambda: defaultdict(list))
    fails = defaultdict(int)
    order = []
    for r in recs:
        n = r["name"]
        if n not in order:
            order.append(n)
        if not r.get("ok"):
            fails[(n, r["variant"])] += 1
            continue
        rows[(n, r["variant"])]["wall"].append(r.get("wall_s"))
        rows[(n, r["variant"])]["rss"].append(r.get("rss_kb", 0) / 1024.0)
        rows[(n, r["variant"])]["cyc"].append((r.get("adaptive_cycles") or 0) + (r.get("fixed_cycles") or 0))
    print("| Workload | Default policy: s / peak RSS MB / cycles | Controller: s / peak RSS MB (range) / cycles | Controller vs default (time, memory) |")
    print("|---|---|---|---|")
    for n in order:
        d, a = rows[(n, "after-default")], rows[(n, "after-adaptive")]
        if d["wall"]:
            dc = "%s / %s / %s" % (f1(med(d["wall"]), 2), f1(med(d["rss"]), 0), f1(med(d["cyc"]), 0))
        else:
            dc = "killed by the 16 GB cap (%d of %d runs)" % (fails[(n, "after-default")], fails[(n, "after-default")])
        ac = "%s / %s (%s-%s) / %s" % (f1(med(a["wall"]), 2), f1(med(a["rss"]), 0), f1(min(a["rss"]), 0),
                                       f1(max(a["rss"]), 0), f1(med(a["cyc"]), 0)) if a["wall"] else "FAILED"
        cmp_ = "%s, %s" % (pct(med(d["wall"]), med(a["wall"])), pct(med(d["rss"]), med(a["rss"]))) if d["wall"] else "completes where the default does not"
        print("| %s | %s | %s | %s |" % (n.replace("policy/", ""), dc, ac, cmp_))


def diag_table(recs):
    rows = defaultdict(lambda: defaultdict(list))
    order = []
    for r in recs:
        n = r["name"].split("/", 1)[1]
        if n not in order:
            order.append(n)
        if r.get("ok"):
            g = r["gc"]
            rows[n]["wall"].append(r["wall_s"])
            rows[n]["cyc"].append(g.get("cycles"))
            rows[n]["sw"].append(g["sweep"] * 1000.0 / g["swept_cells"] if g.get("swept_cells") else None)
            rows[n]["cpu"].append(r["user_s"] + r["sys_s"])
    print("| Variant | Wall s | Cycles | Sweep ns/cell | Process CPU s | runs |")
    print("|---|---|---|---|---|---|")
    for n in order:
        x = rows[n]
        print("| %s | %s | %s | %s | %s | %d |" % (n, f1(med(x["wall"]), 2), f1(med(x["cyc"]), 0), f1(med(x["sw"])),
                                               f1(med(x["cpu"])), len(x["wall"])))


if __name__ == "__main__":
    g = sys.argv[1]
    if g == "st6":
        st6_table(sys.argv[2])
    elif g == "struct":
        struct_table(sys.argv[2])
    else:
        recs = load(sys.argv[2:])
        {"gc": gc_table, "cal": cal_table, "rt6": rt6_table, "cells": cells_table,
         "policy": policy_table, "diag": diag_table}[g](recs)