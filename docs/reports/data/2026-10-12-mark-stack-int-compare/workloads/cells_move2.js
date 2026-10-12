// Cells per call: a method that updates two fields of its receiver.
function V(x, y) { this.x = x; this.y = y; }
V.prototype.move = function (dx, dy) { this.x = this.x + dx; this.y = this.y + dy; };
const v = new V(0, 0);
const N = @N@;
for (let i = 0; i < N; i++) v.move(1, 1);
console.log("done " + v.x);
