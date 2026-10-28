const o = {a: 1, b: 2, c: 3};
function runOne() { let s = 0; for (let i = 0; i < 200000; i++) { s = o.b; } }
runOne();
