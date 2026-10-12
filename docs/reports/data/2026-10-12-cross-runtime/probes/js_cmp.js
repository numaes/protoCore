let s = 0;
for (let i = 0; i < 2000000; i++) { s += (i < 1000000 ? 1 : 0); }
console.log("CHECK", s);
