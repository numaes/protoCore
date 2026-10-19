#!/usr/bin/env python3
"""Table of ab.py: per case, median cycles and instructions per arm over the
rounds, the B/A change, and the A/A spread (|AA - A| / A, the noise floor).

usage: ab_tab.py <results.jsonl>
"""
import json, statistics as st, sys, collections

rows = [json.loads(l) for l in open(sys.argv[1])]
bad = [r for r in rows if not r["ok"]]
for r in bad:
    print("FAILED RUN", r["case"], r["round"], r["arm"])
d = collections.OrderedDict()
for r in rows:
    if r["ok"]:
        d.setdefault(r["case"], collections.defaultdict(list))[r["arm"]].append(r["perf"])
print("| Case | A Gcycles | B Gcycles | B/A cycles | A/A cycles | B/A instructions |")
print("|---|---:|---:|---:|---:|---:|")
for case, arms in d.items():
    m = {a: st.median(p["cycles"] for p in arms[a]) for a in ("A", "B", "AA") if arms[a]}
    i = {a: st.median(p["instructions"] for p in arms[a]) for a in ("A", "B") if arms[a]}
    print("| %s | %.3f | %.3f | %+.1f %% | %+.1f %% | %+.1f %% |" % (
        case, m["A"] / 1e9, m["B"] / 1e9, (m["B"] / m["A"] - 1) * 100,
        (m["AA"] / m["A"] - 1) * 100, (i["B"] / i["A"] - 1) * 100))
