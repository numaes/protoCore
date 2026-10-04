const N = 100000;
const keep = [];
for (let i = 0; i < N; i++) keep.push({ id: i, name: "n" + (i % 100), x: i * 0.5, y: i, tag: "t" });
let sumId = 0, sumX = 0, names = 0;
for (const o of keep) { sumId += o.id + o.y; sumX += o.x; if (o.name === "n7") names++; }
console.log("objects " + keep.length + " sumId " + sumId + " sumX " + sumX + " n7 " + names);
