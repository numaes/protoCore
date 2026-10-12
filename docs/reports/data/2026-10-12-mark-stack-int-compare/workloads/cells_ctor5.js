// Cells per construction: a constructor with five this.x = ... writes.
function P(a, b, c, d, e) { this.a = a; this.b = b; this.c = c; this.d = d; this.e = e; }
const N = @N@;
let last = null;
for (let i = 0; i < N; i++) { last = new P(i, 1, 2, 3, 4); }
console.log("done " + (last.a + 1));
