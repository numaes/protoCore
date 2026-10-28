import subprocess, sys, os, glob
bins = sys.argv[1:]
d = os.path.dirname(os.path.abspath(__file__))
names = sorted({os.path.basename(f).rsplit('_', 1)[0] for f in glob.glob(d + '/*_200000.js')})
def ins(b, f):
    p = subprocess.run(['perf', 'stat', '-x,', '-e', 'instructions:u,cycles:u', '-r', '3', b, f], capture_output=True, text=True)
    if p.returncode: raise SystemExit(f'FAIL {b} {f} {p.stderr[-300:]}')
    r = {}
    for l in p.stderr.splitlines():
        x = l.split(',')
        if len(x) > 2: r[x[2].split(':')[0]] = float(x[0])
    return r
print('%-12s' % 'op' + ''.join('%22s' % os.path.basename(b) for b in bins))
for n in names:
    row = []
    for b in bins:
        a = ins(b, f'{d}/{n}_200000.js'); c = ins(b, f'{d}/{n}_400000.js')
        row.append('%9.0f i %7.0f c' % ((c['instructions'] - a['instructions']) / 2e5, (c['cycles'] - a['cycles']) / 2e5))
    print('%-12s' % n + ''.join('%22s' % x for x in row), flush=True)
