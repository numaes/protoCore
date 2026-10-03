#!/usr/bin/env python3
"""One row per workload: fixed vs variants, medians; verdict for the last variant."""
import json, statistics, sys
from collections import defaultdict
f = sys.argv[1]; variants = sys.argv[2].split(","); base = variants[0]
rows = defaultdict(list); order = []
for l in open(f):
    d = json.loads(l); rows[(d["workload"], d["variant"])].append(d)
    if d["workload"] not in order: order.append(d["workload"])
def med(rs, k): return statistics.median([r[k] for r in rs if r[k] is not None])
def cell(rs):
    rss = med(rs, "rssKiB") / 1024; t = med(rs, "elapsed")
    cyc = statistics.median([r["adaptiveCycles"] + r["fixedCycles"] for r in rs])
    Ss = [r["finalS"] for r in rs if r["finalS"]]
    S = (" S %.1f M" % (statistics.median(Ss) / 1e6)) if Ss else ""
    return rss, t, "%.0f MB, %.2f s, %d cyc%s" % (rss, t, cyc, S)
hdr = "| Workload | " + " | ".join(variants) + " | %s vs %s: RSS, wall |" % (variants[-1], base)
print(hdr); print("|---" * (len(variants) + 2) + "|")
for w in order:
    cs = []; vals = {}
    for v in variants:
        rs = rows.get((w, v))
        if not rs: cs.append("-"); continue
        ok = all(r["ok"] for r in rs)
        rss, t, txt = cell(rs); vals[v] = (rss, t)
        cs.append(txt + ("" if ok else " **FAILED**"))
    last = variants[-1]
    if base in vals and last in vals:
        dr = 100 * (vals[last][0] / vals[base][0] - 1); dt = 100 * (vals[last][1] / vals[base][1] - 1)
        verdict = "%+.0f %%, %+.0f %%" % (dr, dt)
    else:
        verdict = "-"
    print("| %s | %s | %s |" % (w, " | ".join(cs), verdict))
