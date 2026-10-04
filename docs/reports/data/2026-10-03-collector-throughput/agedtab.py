#!/usr/bin/env python3
"""Fresh-vs-recycled and aged-window table.  For every run: the share of
cells handed out from fresh OS blocks over the whole run, and the sweep's ns
per swept cell over the whole run and over the aged window (after the
collector had freed 3 x the heap limit: last [GC-PHASES] minus the first
line past that point).  usage: agedtab.py results.jsonl [label...]"""
import json, sys, statistics as st
from collections import defaultdict
recs = [json.loads(l) for l in open(sys.argv[1])]
labels = sys.argv[2:]
g = defaultdict(list)
for r in recs:
    if labels and r["label"] not in labels: continue
    g[(r["name"], r["label"])].append(r)
print("| Workload | Build | Wall s | Fresh share (whole run) | Sweep ns/cell (whole run) | Aged window: share of swept cells | Fresh share in window | Sweep ns/cell in window | Wait share |")
print("|---|---|---:|---:|---:|---:|---:|---:|---:|")
def med(xs):
    xs = [x for x in xs if x is not None]
    return st.median(xs) if xs else None
for (n, lab), rs in sorted(g.items()):
    def per(r):
        a = r["gc"]; w = r.get("gc_warm")
        fr = a.get("refill_fresh"); rc = a.get("refill_recycled")
        share = fr / (fr + rc) if fr is not None and rc is not None and fr + rc else None
        whole = a["sweep"] * 1e3 / a["swept_cells"] if a.get("swept_cells") else None
        win = winshare = wfresh = None
        if w and a.get("swept_cells", 0) > w.get("swept_cells", 0):
            ds = a["swept_cells"] - w["swept_cells"]
            win = (a["sweep"] - w["sweep"]) * 1e3 / ds
            winshare = ds / a["swept_cells"]
            if fr is not None and "refill_fresh" in w:
                dfr = fr - w["refill_fresh"]; drc = rc - w["refill_recycled"]
                wfresh = dfr / (dfr + drc) if dfr + drc else None
        thr = int(r["env"].get("N", r["env"].get("T", 6 if "t6" in n else 1)))
        wait = a.get("headroom_wait", 0) / 1e6 / (r["wall_s"] * thr) if r.get("wall_s") else None
        return r.get("wall_s"), share, whole, winshare, wfresh, win, wait
    vals = [per(r) for r in rs]
    m = [med([v[i] for v in vals]) for i in range(7)]
    f = lambda x, fmt: (fmt % x) if x is not None else "n/a"
    print("| %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (n, lab, f(m[0], "%.2f"), f(m[1] and 100*m[1], "%.0f %%"),
          f(m[2], "%.1f"), f(m[3] and 100*m[3], "%.0f %%"), f(m[4] and 100*m[4], "%.0f %%"), f(m[5], "%.1f"), f(m[6] and 100*m[6], "%.1f %%")))
