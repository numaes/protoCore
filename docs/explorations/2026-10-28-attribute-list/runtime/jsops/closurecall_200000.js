
function runOne() { let s = 0; const f = function (v) { return (v + 1) | 0; }; for (let i = 0; i < 200000; i++) { s = f(s); } }
runOne();
