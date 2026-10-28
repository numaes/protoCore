let s = 0;
for (let i = 0; i < 500000; i++) { const o = { x: i, y: 1, z: 2 }; s += o.y + o.z; }
console.log("CHECK", s);
