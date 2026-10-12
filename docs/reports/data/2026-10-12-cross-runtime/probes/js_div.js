let s = 0;
for (let i = 0; i < 2000000; i++) { s += Math.trunc(i / 3); }
console.log("CHECK", s);
