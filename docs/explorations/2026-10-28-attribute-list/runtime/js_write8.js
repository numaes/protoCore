const b = { a: 0, c: 2, d: 3, e: 4, f: 5, g: 6, x: 0, y: 1 };
let s = 0;
for (let i = 0; i < 2000000; i++) { b.x = i; s += b.y; }
console.log("CHECK", s + b.x);
