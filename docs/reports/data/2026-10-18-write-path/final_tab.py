#!/usr/bin/env python3
"""Summary table of final_table.sh: protoCore (median over the three library
copies) against the JVM structures of ImmutableBench, in ns per operation.

usage: final_tab.py <outdir>
"""
import glob, os, re, statistics as st, sys, collections

OUT = sys.argv[1]
LINE = re.compile(r"^(.*?)\s+(\d+)\s+([\d.,]+) ns/op\s+\(ops=\d+, checksum (\w+)\)")


def parse(path, nobj_default=None):
    nobj = nobj_default
    for line in open(path):
        m = re.match(r"-- nobj=(\d+)", line)
        if m:
            nobj = int(m.group(1))
            continue
        m = LINE.match(line)
        if m:
            if m.group(4) != "ok":
                sys.exit("checksum failed: %s: %s" % (path, line.strip()))
            yield nobj, m.group(1).strip(), int(m.group(2)), float(m.group(3).replace(",", "."))


proto = collections.defaultdict(list)
for kind in ("sweep", "upd"):
    for p in glob.glob(os.path.join(OUT, "proto_%s_*.txt" % kind)):
        for nobj, sc, par, ns in parse(p):
            proto[(sc, nobj, par)].append(ns)
for p in glob.glob(os.path.join(OUT, "proto_own_*.txt")):
    for _, sc, par, ns in parse(p):
        if sc.startswith("own/immutable/hot"):
            proto[("own/immutable", 1, par)].append(ns)
        elif sc.startswith("NATIVE own/hot"):
            proto[("NATIVE own", 1, par)].append(ns)
for n in (4, 16):
    reps = []
    for p in glob.glob(os.path.join(OUT, "proto_hotcopy_%d_*.txt" % n)):
        reps += [float(m.group(1)) for m in re.finditer(r"rep [1-9]\d*: ([\d.]+) ns/op", open(p).read())]
    if reps:
        proto[("hotcopy", 1, n)] = reps

jvm = {(sc, nobj, par): ns for nobj, sc, par, ns in parse(os.path.join(OUT, "jvm.txt"))}


def med(d, k):
    v = d.get(k)
    if v is None:
        return None
    return st.median(v) if isinstance(v, list) else v


def fmt(x):
    return "-" if x is None else ("%.1f" % x)


print("| Objects | Attrs | C++ in place read | JVM mutable read | Java record read | Clojure map read | Scala HashMap read | protoCore read |")
print("|---:|---:|---:|---:|---:|---:|---:|---:|")
for nobj in (1, 4096, 16384, 65536):
    for par in (4, 16):
        print("| %d | %d | %s | %s | %s | %s | %s | %s |" % (
            nobj, par, fmt(med(proto, ("NATIVE own", nobj, par)) or med(proto, ("NATIVE own nattr", nobj, par))),
            fmt(med(jvm, ("mutable/read nattr", nobj, par))), fmt(med(jvm, ("record/read nattr", nobj, par))),
            fmt(med(jvm, ("clj/read nattr", nobj, par))), fmt(med(jvm, ("scala/read nattr", nobj, par))),
            fmt(med(proto, ("own/immutable", nobj, par)) or med(proto, ("own/immutable nattr", nobj, par)))))
print()
print("| Objects | Attrs | C++ in place | JVM mutable in place | Java record copy | Clojure assoc | Scala updated | protoCore copy (default heap) | protoCore copy (adaptive heap, recycling) |")
print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
for nobj in (1, 4096, 16384, 65536):
    for par in (4, 16):
        print("| %d | %d | %s | %s | %s | %s | %s | %s | %s |" % (
            nobj, par, fmt(med(proto, ("NATIVE update in place nattr", nobj, par))),
            fmt(med(jvm, ("mutable/update copy nattr", nobj, par))), fmt(med(jvm, ("record/update copy nattr", nobj, par))),
            fmt(med(jvm, ("clj/update copy nattr", nobj, par))), fmt(med(jvm, ("scala/update copy nattr", nobj, par))),
            fmt(med(proto, ("update/immutable copy nattr", nobj, par))),
            fmt(med(proto, ("hotcopy", 1, par))) if nobj == 1 else "-"))
