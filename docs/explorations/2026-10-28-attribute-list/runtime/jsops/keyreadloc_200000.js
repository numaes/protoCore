
function runOne() { let s = 0; const o = {a: 1, b: 2, c: 3}; const k = 'b'; for (let i = 0; i < 200000; i++) { s = o[k]; } }
runOne();
