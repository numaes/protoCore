#!/usr/bin/env python3
"""Median-of-reps table per (workload, label) from runner.py records.
usage: tab.py results.jsonl [label...]"""
import json, sys, statistics as st
from collections import defaultdict
recs = [json.loads(l) for l in open(sys.argv[1])]
labels = sys.argv[2:] or sorted({r["label"] for r in recs})
# JS checksum consistency: task k's checksum identical in every run of a workload
cs = defaultdict(set)
def jsonline(r):
    if r.get("json"):
        return json.loads(r["json"][0])
    for line in r.get("stdout_tail", "").splitlines():
        if line.startswith("{"):
            try: return json.loads(line)
            except Exception: return {"truncated": True}
for r in recs:
    j = jsonline(r)
    if j and "checksums" in j:
        cs[r["name"]].add(tuple(j["checksums"]))
def ok(r):
    j = jsonline(r)
    if (j is None and r["name"].startswith("js") and r["exit"] == 0) or (j is not None and j.get("truncated")):
        return None   # the JSON line was cut by an earlier runner: unverified
    if j is not None:
        return r["exit"] == 0 and j.get("ok") is True and len(cs[r["name"]]) == 1
    return r["ok"]
groups = defaultdict(list)
for r in recs:
    groups[(r["name"], r["label"])].append(r)
names = []
for r in recs:
    if r["name"] not in names: names.append(r["name"])
print("| Workload | Build | ok | Wall s | Peak RSS MB | Cycles | Wait share | Waits | Mean wait ms | GC busy s | Sweep s | Sweep ns/cell |")
print("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
for n in names:
    for lab in labels:
        rs = groups.get((n, lab))
        if not rs: continue
        vals = [ok(r) for r in rs]
        oks = "unverified" if None in vals else ("yes" if all(vals) else "**no**")
        med = lambda f: st.median([f(r) for r in rs])
        thr = rs[0]["env"].get("N", rs[0]["env"].get("T", 1))
        try: thr = int(thr)
        except Exception: thr = 1
        if n.startswith("clj_coll_t"): thr = int(n[-1])
        g = lambda r, k: r["gc"].get(k, 0)
        wall = med(lambda r: r.get("wall_s", 0))
        share = med(lambda r: g(r, "headroom_wait") / 1e6 / (r.get("wall_s", 1) * thr))
        waits = med(lambda r: g(r, "headroom_waits"))
        mw = med(lambda r: g(r, "headroom_wait") / g(r, "headroom_waits") / 1e3 if g(r, "headroom_waits") else 0)
        nspc = med(lambda r: g(r, "sweep") * 1e3 / g(r, "swept_cells") if g(r, "swept_cells") else 0)
        print("| %s | %s | %s | %.2f | %d | %d | %.1f %% | %d | %.1f | %.2f | %.2f | %.1f |" % (
            n, lab, oks, wall, med(lambda r: r.get("rss_kb", 0)) // 1024,
            med(lambda r: g(r, "cycles")), 100 * share, waits, mw, med(lambda r: g(r, "busy")) / 1e6,
            med(lambda r: g(r, "sweep")) / 1e6, nspc))
