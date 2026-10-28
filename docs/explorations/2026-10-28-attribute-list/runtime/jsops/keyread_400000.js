const o = {a: 1, b: 2, c: 3}; const k = 'b';
function runOne() { let s = 0; for (let i = 0; i < 400000; i++) { s = o[k]; } }
runOne();
