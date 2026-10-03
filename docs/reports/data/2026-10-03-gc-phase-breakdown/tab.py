#!/usr/bin/env python3
import json, sys
recs = {}
import os
OUTD = os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), "out")
ref = {}   # workload -> {k: checksum}, cross-checked across N and heap settings
for line in open(sys.argv[1]):
    r = json.loads(line); recs[r["name"]] = r
    if r["name"].startswith("js") and not r["name"].startswith("jsx"):
        # protojs_bench.js itself checks the checksums across repetitions;
        # here every task k must also agree across N and heap settings.
        j = None
        for l in open(os.path.join(OUTD, r["name"] + ".out")):
            if l.startswith("{"): j = json.loads(l)
        r["ok"] = bool(r["exit"] == 0 and j and j.get("ok") is True)
        if j:
            d = ref.setdefault(r["env"]["WORKLOAD"], {})
            for k, c in enumerate(j["checksums"]):
                if d.setdefault(k, c) != c:
                    r["ok"] = False
def threads(r):
    e = r["env"]; return int(e.get("N", e.get("T", 1)))
print("| Workload | ok | Wall s | Cycles | GC busy s (% wall) | STW total / max ms | Quorum ms | Mark s (young+trace) | Sweep s | 5b ms | Unmark ms | Headroom wait s (sum, waits) | STW park s | Marked M | Swept M | Freed M | Mark Mc/s | Sweep Mc/s |")
print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
for n, r in recs.items():
    g = r["gc"]; w = r.get("wall_s", 0)
    if not g:
        print("| %s | %s | %.1f | 0 | - | - | - | - | - | - | - | - | - | - | - | - | - | - |" % (n, "yes" if r["ok"] else "**no**", w)); continue
    busy = g["busy"]/1e6; mark = (g["young"]+g["trace"])/1e6; sweep = g["sweep"]/1e6
    mmc = (g["marked"]+g["young_cells"])/1e6/mark if mark else 0
    smc = g["swept_cells"]/1e6/sweep if sweep else 0
    print("| %s | %s | %.1f | %d | %.2f (%.0f%%) | %.1f / %.2f | %.1f | %.2f | %.2f | %.1f | %.0f | %.2f (%d) | %.3f | %.1f | %.1f | %.1f | %.0f | %.0f |" % (
        n, "yes" if r["ok"] else "**no**", w, g["cycles"], busy, 100*busy/w if w else 0,
        g["stw"]/1e3, g["stw_max"]/1e3, g["quorum"]/1e3, mark, sweep, g["rel"]/1e3, g["unmark"]/1e3,
        g["headroom_wait"]/1e6, g["headroom_waits"], g["mut_park"]/1e6,
        g["marked"]/1e6, g["swept_cells"]/1e6, g["freed_cells"]/1e6, mmc, smc))
