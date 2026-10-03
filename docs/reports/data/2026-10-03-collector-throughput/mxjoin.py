import json,re,sys
fixed={};ad={}
for f,dst,pol in (('matrix.jsonl',fixed,'fixed'),('matrix_m3b.jsonl',ad,'adaptive')):
    for l in open(f.replace('matrix.jsonl','matrix-fixed-and-first-law.jsonl').replace('matrix_m3b.jsonl','matrix-controller-2.13.0.jsonl')):
        r=json.loads(l); m=re.match(r"mx_(\w+?)_(\d+)M_(\w+)",r["name"])
        if m.group(3)==pol: dst[(m.group(1),int(m.group(2)))]=r
thr={"core":1,"js_records":12,"js_wordfreq":12,"scala":6,"clj":6}
print("| Workload | Limit / H (M cells) | Fixed limit: wall s, peak RSS MB, cycles, wait share | Controller 2.13.0 (H = the limit): wall s, peak RSS MB, cycles, wait share |")
print("|---|---:|---|---|")
for w in thr:
    for lm in (10,20,40,100,200):
        a=fixed.get((w,lm)); b=ad.get((w,lm))
        f=lambda r:"%.2f, %d, %d, %.1f %%"%(r['wall_s'],r['rss_kb']//1024,r['gc'].get('cycles',0),100*r['gc'].get('headroom_wait',0)/1e6/(r['wall_s']*thr[w]))
        print("| %s | %d | %s | %s |"%(w,lm,f(a),f(b)))
