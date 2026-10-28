
function runOne() { let s = 0; const a = [1,2,3,4,5,6,7,8]; for (let i = 0; i < 400000; i++) { s = a[i & 7]; } }
runOne();
