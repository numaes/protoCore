import json,sys,re
from collections import defaultdict
recs=[json.loads(l) for l in open(sys.argv[1])]
rows=defaultdict(dict)
for r in recs:
    m=re.match(r"mx_(\w+?)_(\d+)M_(\w+)",r["name"])
    w,lm,pol=m.group(1),int(m.group(2)),m.group(3)
    rows[(w,lm)][pol]=r
thr={"core":1,"js_records":12,"js_wordfreq":12,"scala":6,"clj":6}
print("| Workload | Limit (M cells) | Policy | ok | Wall s | Peak RSS MB | Cycles | Wait share | GC busy s |")
print("|---|---:|---|---|---:|---:|---:|---:|---:|")
for (w,lm) in sorted(rows,key=lambda k:(list(thr).index(k[0]),k[1])):
    for pol in ["fixed","adaptive","helpers"]:
        r=rows[(w,lm)].get(pol)
        if not r: continue
        g=r["gc"]; t=thr[w]
        j=None
        for line in (r.get("json") or []): j=json.loads(line)
        ok = r["exit"]==0 and (r["ok"] if not w.startswith("js") else (j is not None and j.get("ok") is True))
        share = g.get("headroom_wait",0)/1e6/(r["wall_s"]*t) if r.get("wall_s") else 0
        print("| %s | %d | %s | %s | %.2f | %d | %d | %.1f %% | %.2f |"%(w,lm,pol,"yes" if ok else "**no** (exit %d)"%r["exit"],r.get("wall_s",0),r.get("rss_kb",0)//1024,g.get("cycles",0),100*share,g.get("busy",0)/1e6))
