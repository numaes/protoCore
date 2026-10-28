#!/usr/bin/env python3
"""Per configuration and n: geometric mean over a set of operations of the
ratio branch/master (median over rounds), for instructions and cycles.

usage: summary.py <sweep dir> <metric in|cy|ns> <group>
groups: reads, writes_imm, writes_mut, all"""
import glob, math, os, re, statistics, sys

d, metric, group = sys.argv[1], sys.argv[2], sys.argv[3]
GROUPS = {
    "reads": ["read_hot_new", "read_hot_mid", "read_hot_old", "read_cold_new", "read_cold_mid",
              "read_cold_old", "miss_chain_cold", "miss_chain_hot"],
    "reads_cold": ["read_cold_new", "read_cold_mid", "read_cold_old", "miss_chain_cold"],
    "writes_imm": ["imm_write_new", "imm_over_new", "imm_over_mid", "imm_over_old"],
    "writes_mut": ["mut_write_new", "mut_over_new", "mut_over_mid", "mut_over_old",
                   "mut_write_cycle", "mut_write_samekey"],
}
GROUPS["all"] = sum(GROUPS.values(), [])
ops = GROUPS[group]
data = {}
for f in glob.glob(os.path.join(d, "*-time.txt")):
    cfg = os.path.basename(f).split("-")[0]
    for line in open(f):
        m = re.match(r"\s*(\d+) (\S+)\s+([\d.]+) ns\s+([\d.]+) in\s+([\d.]+) cy", line)
        if m:
            v = {"ns": float(m.group(3)), "in": float(m.group(4)), "cy": float(m.group(5))}[metric]
            data.setdefault((cfg, int(m.group(1)), m.group(2)), []).append(v)
cfgs = [c for c in ["masterB", "br0", "br4", "br8", "br12", "br16", "br24", "br32", "br8np", "br16np"]
        if any(k[0] == c for k in data)]
ns = sorted({k[1] for k in data})
print("geomean ratio to master, %s, group %s\n" % (metric, group))
print("| n | " + " | ".join(cfgs) + " |")
print("|---" * (len(cfgs) + 1) + "|")
for n in ns:
    row = []
    for c in cfgs:
        logs = []
        for op in ops:
            a, b = data.get((c, n, op)), data.get(("master", n, op))
            if a and b:
                logs.append(math.log(statistics.median(a) / statistics.median(b)))
        row.append("%.2f" % math.exp(sum(logs) / len(logs)) if logs else "-")
    print("| %d | " % n + " | ".join(row) + " |")
