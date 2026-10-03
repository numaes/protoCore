#!/usr/bin/env python3
"""Summarise run.py JSONL: median per (workload, variant); ratios vs the baseline variant.

usage: summ.py <file.jsonl>... [--base fixed] [--md]
"""
import json, statistics, sys
from collections import defaultdict

args = [a for a in sys.argv[1:] if not a.startswith("--")]
base = "fixed"
md = "--md" in sys.argv
for i, a in enumerate(sys.argv):
    if a == "--base":
        base = sys.argv[i + 1]
        args.remove(base)
rows = defaultdict(list)
order_w, order_v = [], []
for f in args:
    for line in open(f):
        d = json.loads(line)
        rows[(d["workload"], d["variant"])].append(d)
        if d["workload"] not in order_w:
            order_w.append(d["workload"])
        if d["variant"] not in order_v:
            order_v.append(d["variant"])

def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None

def agg(rs):
    ok = all(r["ok"] for r in rs)
    return dict(n=len(rs), ok=ok, rss=med([r["rssKiB"] / 1024 for r in rs if r["rssKiB"]]),
                t=med([r["elapsed"] for r in rs]), cpu=med([(r["user"] or 0) + (r["sys"] or 0) for r in rs]),
                cyc=med([r["adaptiveCycles"] + r["fixedCycles"] for r in rs]),
                S=med([r["finalS"] for r in rs]), p=max(r["maxP"] for r in rs))

if md:
    print("| workload | variant | n | ok | peak RSS MB | wall s | user+sys s | cycles | final S (Mcells) | max p | RSS vs %s | wall vs %s |" % (base, base))
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
for w in order_w:
    b = rows.get((w, base))
    ba = agg(b) if b else None
    for v in order_v:
        rs = rows.get((w, v))
        if not rs:
            continue
        a = agg(rs)
        rr = tr = ""
        if ba and a["rss"] and ba["rss"] and a["t"] and ba["t"]:
            rr = "%+.0f%%" % (100 * (a["rss"] / ba["rss"] - 1))
            tr = "%+.0f%%" % (100 * (a["t"] / ba["t"] - 1))
        S = "%.2f" % (a["S"] / 1e6) if a["S"] else "-"
        if md:
            print("| %s | %s | %d | %s | %.0f | %.2f | %.2f | %s | %s | %.2f | %s | %s |" % (
                w, v, a["n"], "yes" if a["ok"] else "**NO**", a["rss"] or 0, a["t"] or -1, a["cpu"] or 0,
                "%.0f" % a["cyc"] if a["cyc"] is not None else "-", S, a["p"], rr, tr))
        else:
            print("%-24s %-8s n=%d ok=%-5s rss=%7.0f t=%6.2f cpu=%6.2f cyc=%4s S=%6s p=%.2f  rss%6s t%6s" % (
                w, v, a["n"], a["ok"], a["rss"] or 0, a["t"] or -1, a["cpu"] or 0,
                "%.0f" % a["cyc"] if a["cyc"] is not None else "-", S, a["p"], rr, tr))
    if not md:
        print()
