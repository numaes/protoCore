#!/usr/bin/env python3
"""Aggregates sweep/<cfg>-<round>-time.txt: per (n, op) the median over rounds
of instructions, cycles and ns; cells from any round (deterministic).

usage: tabulate.py <sweep dir> <metric: in|cy|ns|cells> [ops...]
Prints a markdown table per op: rows n, columns configurations (master in
absolute units, the others as ratio to master)."""
import glob, os, re, statistics, sys

d, metric = sys.argv[1], sys.argv[2]
ops_wanted = sys.argv[3:]
data = {}
for f in glob.glob(os.path.join(d, "*-time.txt")):
    cfg, rnd = os.path.basename(f).split("-")[:2]
    for line in open(f):
        m = re.match(r"\s*(\d+) (\S+)\s+([\d.]+) ns\s+([\d.]+) in\s+([\d.]+) cy\s+([-\d.]+) cells", line)
        if not m:
            continue
        n, op = int(m.group(1)), m.group(2)
        v = {"ns": float(m.group(3)), "in": float(m.group(4)), "cy": float(m.group(5)), "cells": float(m.group(6))}
        data.setdefault((cfg, n, op), []).append(v[metric])
cfgs = ["master", "masterB", "br0", "br4", "br8", "br12", "br16", "br24", "br32", "br8np", "br16np"]
cfgs = [c for c in cfgs if any(k[0] == c for k in data)]
ns = sorted({k[1] for k in data})
ops = ops_wanted or sorted({k[2] for k in data})
for op in ops:
    print("\n**%s** (%s; master absolute, others ratio to master)\n" % (op, metric))
    print("| n | " + " | ".join(cfgs) + " |")
    print("|---" * (len(cfgs) + 1) + "|")
    for n in ns:
        row = []
        base = data.get(("master", n, op))
        bm = statistics.median(base) if base else None
        for c in cfgs:
            v = data.get((c, n, op))
            if not v:
                row.append("-"); continue
            med = statistics.median(v)
            if c == "master" or metric == "cells":
                row.append("%.0f" % med if metric != "cells" else "%.2f" % med)
            else:
                row.append("%.2f" % (med / bm) if bm else "-")
        print("| %d | " % n + " | ".join(row) + " |")
