
function runOne() { const o = {a: 1, b: 2, c: 3}; for (let i = 0; i < 200000; i++) { o.b = i; } }
runOne();
