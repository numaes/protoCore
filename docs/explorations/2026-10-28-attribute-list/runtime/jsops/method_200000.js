
function runOne() { let s = 0; const o = {m(v) { return (v + 1) | 0; }}; for (let i = 0; i < 200000; i++) { s = o.m(s); } }
runOne();
