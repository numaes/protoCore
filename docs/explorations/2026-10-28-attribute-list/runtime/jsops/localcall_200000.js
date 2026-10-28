function work(v) { return (v + 1) | 0; }
function runOne() { let s = 0; for (let i = 0; i < 200000; i++) { s = work(s); } }
runOne();
