let state = 1; function work(v) { return (v + 1) | 0; }
function runOne() {  for (let i = 0; i < 400000; i++) { state = work(state); } }
runOne();
