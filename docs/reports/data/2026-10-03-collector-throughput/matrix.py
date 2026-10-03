#!/usr/bin/env python3
"""The memory-as-only-variable experiment (spec section 9): each synthetic
workload under fixed limits and under the controller (H = the limit), same
library, same binaries.  usage: matrix.py <lib_dir> <label> <results.jsonl>
<policy: fixed|adaptive|helpers> <limitsM comma list> [workloads comma list]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runner as R

def main():
    lib, label, res, policy, limits = sys.argv[1:6]
    wls = sys.argv[6].split(",") if len(sys.argv) > 6 else ["core", "js_records", "js_wordfreq", "scala", "clj"]
    for lm in [int(x) for x in limits.split(",")]:
        cells = lm * 1000000
        cap = "%dG" % (int(cells * 64 * 1.4 / 2**30) + 2)
        extra = {"PROTOCORE_HEAP_LIMIT_CELLS": cells}
        if policy == "adaptive":
            extra["PROTOCORE_ADAPTIVE_HEAP"] = 1
        if policy == "helpers":
            extra["PROTOCORE_GC_SWEEP_THREADS"] = os.environ.get("K", "3")
        name = lambda w: "mx_%s_%dM_%s" % (w, lm, policy)
        for w in wls:
            env = dict(extra)
            if w == "core":
                mode = ["adaptive", "1000000"] if policy == "adaptive" else ["fixed", str(cells), "1000000"]
                R.run(lib, label, res, name(w), [lib + "/adaptive_heap_benchmark"] + mode, env, cap, R.HERE,
                      lambda o, e: "verified=yes" in o, 0)
            elif w.startswith("js_"):
                env.update({"WORKLOAD": w[3:], "MODE": "par", "N": 12, "REPS": 1, "SCALE": 1})
                R.run(lib, label, res, name(w), [R.BIN + "/protojs", "protojs_bench.js"], env, cap, R.JS, R.js_ok, 0)
            elif w == "scala":
                env.update({"T": 6, "BATCHES": 30})
                R.run(lib, label, res, name(w), [R.BIN + "/protoscala", R.WL + "/tree_alloc.scala"], env, cap, R.WL,
                      lambda o, e: "checksum=540451801" in o and o.strip().endswith("ok"), 0)
            elif w == "clj":
                Rn = 500
                expect = 6001000 * Rn + 1000 * Rn * (Rn - 1)
                R.run(lib, label, res, name(w), [R.BIN + "/protoclj", R.WL + "/coll_alloc_r500_t6.clj"], env, cap, R.WL,
                      lambda o, e: ("checksum %d" % expect) in o and o.strip().endswith("ok"), 0)

if __name__ == "__main__":
    main()
