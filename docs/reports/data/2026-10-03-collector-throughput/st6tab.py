import sys,glob,os
d=sys.argv[1]
print("| Benchmark | instructions:u A -> B | cycles:u A -> B | task-clock ms A -> B | stddev cycles A / B |")
print("|---|---:|---:|---:|---:|")
for b in ["microbenchmark_final","mutable_access_benchmark","cache_timing_benchmark","hash_quality_benchmark","object_access_benchmark","immutable_sharing_benchmark"]:
    v={}
    for s in "AB":
        for line in open(f"{d}/perf_{b}_{s}.csv"):
            p=line.strip().split(",")
            if len(p)>3 and p[0] and p[0][0].isdigit():
                v[(s,p[2])]=(float(p[0]),p[3])
    def r(k): a=v[("A",k)][0]; bb=v[("B",k)][0]; return a,bb,100*(bb-a)/a
    i=r("instructions:u"); c=r("cycles:u"); t=r("task-clock" if ("A","task-clock") in v else "msec")
    print("| %s | %.3gG -> %.3gG (%+.1f%%) | %.3gG -> %.3gG (%+.1f%%) | %.0f -> %.0f (%+.1f%%) | %s / %s |"%(b,i[0]/1e9,i[1]/1e9,i[2],c[0]/1e9,c[1]/1e9,c[2],t[0],t[1],t[2],v[("A","cycles:u")][1],v[("B","cycles:u")][1]))
