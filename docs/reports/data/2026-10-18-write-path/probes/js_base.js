const b = { x: 0, y: 1 };
let s = 0;
for (let i = 0; i < 2000000; i++) { s += b.y; }
console.log("CHECK", s + b.x);
