// Fig: mutability concentrated in one place, as a printer's type-case cabinet on a workbench.
// node build.mjs            writes mutability-anatomy.html (dark)
// node build.mjs --light    writes mutability-anatomy-light.html
// node build.mjs --audit    also proves the geometry (kit/audit.mjs) and exits non-zero on a fault
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import * as k from "./kit/iso-kit.mjs";
import * as A from "./kit/audit.mjs";
import * as M from "./model.mjs";
import { makeMovers } from "./movers.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const THEME = process.argv.includes("--light") ? "light" : "dark";
const strip = (file) => readFileSync(join(HERE, file), "utf8").replace(/^export /gm, "").replace(/^import .*$/gm, "");

const W = 600;
const H = 440;
const AZIMUTH = 44;
const ELEVATION = 35;

// World layout, in units of about one pixel; x and y on the bench, z up.
const BENCH = { x: -170, y: -80, w: 340, d: 152, r: 10 };
const BENCH_H = 12;
const FOOT_H = 5;
const POST_X = 160;
const RAIL_Y = 64;
const RAIL_Z = 84;
const RAIL_TOP = 88;
const LOWER_Z = 68;
const ARM_Z = 90;
const ARM_TIP = 49;

const CAB = { x: -136, y: -52, w: 58, d: 104, r: 1.5 };
const CAB_TOP = 66;
const CASE_Z = 58;
const CASE_FLOOR = 61;
const CELL_W = 13.5;
const CELL_D = 12.5;
const CASE_X0 = -134;
const CASE_Y0 = -50;
const HOLE = { col: 2, row: 5 };
const holeCentre = (c, r) => [CASE_X0 + CELL_W * (c + 0.5), CASE_Y0 + CELL_D * (r + 0.5)];
const CARD_TOP = 70;

const STONE = { x: -60, y: -48, w: 168, d: 96, r: 2 };
const STONE_Z = 38;
const STONE_TOP = 44;
const PLATE_H = 3.5;
const PLATE_TOP = STONE_TOP + PLATE_H;
const CX = 24;
const LEVEL_Y = [33, 12, -9, -31];
const OFFSET = [[-3.8, -3.8], [3.8, -3.8], [-3.8, 3.8], [3.8, 3.8]];

function nodeXY(n) {
  const L = M.levelOf(n);
  const count = 2 ** L;
  const i = n - (count - 1);
  const span = M.LEAVES / count;
  return [CX + ((i + 0.5) * span - M.LEAVES / 2) * 20, LEVEL_Y[L]];
}
const siteXY = (s) => {
  const [x, y] = nodeXY(M.nodeOfSite(s));
  const [ox, oy] = OFFSET[s % M.SITES];
  return [x + ox, y + oy];
};
const SITE_COUNT = M.NODES * M.SITES;

const YC = 26;
const COLLECT = { y0: YC - 8.5, y1: YC + 5.5, x0: -54, x1: 150, home: -38, unload: 132, outFrom: 112 };
const HOPPER = { x: -8, y: YC - 8, w: 16, d: 16, floor: 7, top: 17, step: 0.8, max: 12 };
const BIN = { x: 114, y: -26, w: 38, d: 28, r: 1.2 };
const BIN_COLS = 6;
const BIN_ROWS = 4;
const binXY = (i) => [BIN.x + 4.6 + (i % BIN_COLS) * 5.8, BIN.y + 4.6 + Math.floor(i / BIN_COLS) * 6.3];
const STOCK = BIN_COLS * BIN_ROWS;

const READER = { y0: 45.4, d: 6.6, z0: 28.6, h: 7.8, post: 48.7, arm0: 55, armStep: 3.4, rail: { x: -58, w: 164 } };
const BRIDGE = { spindle: 8.5, nozzle: 72, w1Park: 134, w2Park: -143, gap: 18 };

const P = k.fitProjection(
  [...k.boxCorners(BENCH, -BENCH_H - FOOT_H, 0), [-POST_X - 4, -RAIL_Y - 4, ARM_Z + 9], [POST_X + 4, -RAIL_Y - 4, ARM_Z + 9], [-150, ARM_TIP + 2, ARM_Z + 9], [150, ARM_TIP + 2, ARM_Z + 9]],
  W,
  H,
  { pad: 22, azimuth: AZIMUTH, elevation: ELEVATION },
);
const at = (p) => k.iso(p, P);
const S = (paths, style) => k.solidSvg(paths, style);
const Ln = (d, style) => k.lineSvg(d, style);
const Dots = (points, style) => k.dotsSvg(points, style);
const path3 = (points, closed = false) => k.pathOf(points.map(at), closed);
const byDepth = (points) => [...points].sort((a, b) => k.depthOf([a[0], a[1], 0], P) - k.depthOf([b[0], b[1], 0], P));
const f2 = (v) => Math.round(v * 100) / 100;

const R = A.recorder(P);
let solids = 0;
function put(name, ref, svg, shapes, { bias = 0, attrs = "", cls = "" } = {}) {
  solids += 1;
  return R.put({ name: R.nameFor(name), at: ref, bias, svg, shapes, piece: "fig", attrs, cls });
}
function slab(name, plan, z, h, style = {}, { steps = 3, bevel = 0, extra = "", inner = "", bias = 0, attrs = "", ref } = {}) {
  const shape = R.solid(A.slab(plan, z, h, { name }, steps));
  return put(name, ref ?? [plan.x + plan.w / 2, plan.y + plan.d / 2, z + h / 2], S(k.slabOf(plan, z, h, P, steps, bevel), { tone: "mid", ...style, inner }) + extra, [shape], { bias, attrs });
}
function post(name, cx, cy, r, z, h, style = {}, { steps = 24, bevel = 0, extra = "", inner = "", bias = 0 } = {}) {
  const shape = R.solid(A.cylinder(cx, cy, r, z, h, { name }));
  return put(name, [cx, cy, z + h / 2], S(k.cylinder(cx, cy, r, z, h, P, steps, bevel), { tone: "mid", ...style, inner }) + extra, [shape], { bias });
}

// ---------------------------------------------------------------- bench (back layer)
const back = [];
back.push(`<path class="iso-halo" d="${k.haloOf(BENCH, -BENCH_H - FOOT_H, BENCH_H, P)}"/>`);
for (const [x, y] of byDepth(k.corners(BENCH, 24))) {
  back.push(S(k.cylinder(x, y, 8, -BENCH_H - FOOT_H, 1.6, P, 28), { tone: "lo" }));
  back.push(S(k.cylinder(x, y, 6.2, -BENCH_H - FOOT_H + 1.6, FOOT_H - 1.6, P, 28), { tone: "mid" }));
}
back.push(S(k.slabOf(BENCH, -BENCH_H, BENCH_H, P, 8, 1.6), { tone: "mid", inner: Ln(k.sideSeam(BENCH, -4.5, P, 8), { tone: "faint" }) }));
R.solid(A.slab(BENCH, -BENCH_H, BENCH_H, { name: "stand.bench", ground: true }, 8));
back.push(Ln(k.planOutline(k.insetPlan(BENCH, 5), 0, P), { tone: "lo" }));
const benchScrews = k.corners(BENCH, 10);
back.push(Ln(benchScrews.map(([x, y]) => k.ring(x, y, 2.2, 0, P, 16)).join(""), { tone: "lo" }));
back.push(Dots(benchScrews.map(([x, y]) => at([x, y, 0])), { size: 0.5 }));
const STRIP = { x: -120, y: BENCH.y + BENCH.d - 13, w: 220, d: 6, r: 1 };
back.push(S(k.slabOf(STRIP, 0, 0.6, P, 3), { tone: "lo", crease: "none" }));
const ticks = k.topTicks(STRIP.x + 4, STRIP.x + STRIP.w - 4, 4, 5, STRIP.y, 0.6, [1.8, 3.4], P);
back.push(Ln(ticks.minor, { tone: "lo" }), Ln(ticks.major, { tone: "mid" }));
back.push(Dots([at([STRIP.x + 2, STRIP.y + 3, 0.6]), at([STRIP.x + STRIP.w - 2, STRIP.y + 3, 0.6])], { size: 0.45 }));
const LABEL = { x: 118, y: BENCH.y + BENCH.d - 32, w: 34, d: 14, r: 1.2 };
back.push(S(k.slabOf(LABEL, 0, 0.8, P, 3), { tone: "lo", crease: "none" }));
back.push(Ln(k.planOutline(k.insetPlan(LABEL, 1.4), 0.8, P, 3), { tone: "faint" }));
back.push(Ln([22, 15, 18].map((w, i) => k.lineOnTop([LABEL.x + 4, LABEL.y + 3.6 + i * 3.2], [LABEL.x + 4 + w, LABEL.y + 3.6 + i * 3.2], 0.8, P)).join(""), { tone: "lo", free: true }));
back.push(Dots([at([LABEL.x + LABEL.w - 2.6, LABEL.y + 2.4, 0.8]), at([LABEL.x + LABEL.w - 2.6, LABEL.y + LABEL.d - 2.4, 0.8])], { size: 0.45 }));

// ---------------------------------------------------------------- gantry: two back posts and two rails
for (const x of [-POST_X, POST_X]) {
  const y = -RAIL_Y;
  const foot = { x: x - 6, y: y - 6, w: 12, d: 12, r: 1.5 };
  const col = { x: x - 3, y: y - 3, w: 6, d: 6, r: 0.8 };
  slab(`post.foot`, foot, 0, 2, { tone: "lo" }, { extra: Dots(k.corners(foot, 2.2).map(([a, b]) => at([a, b, 2])), { size: 0.4 }) });
  slab(`post.column`, col, 2, RAIL_Z - 2, { tone: "mid", inner: Ln(k.sideSeam(col, 40, P, 3), { tone: "faint" }) }, { bevel: 0.4 });
}
{
  const y = -RAIL_Y;
  const upper = { x: -POST_X - 4, y: y - 2.5, w: POST_X * 2 + 8, d: 5, r: 0.8 };
  slab(`rail.upper`, upper, RAIL_Z, RAIL_TOP - RAIL_Z, { tone: "mid" }, {
    bevel: 0.4,
    extra: Ln(k.lineOnTop([upper.x, y], [upper.x + upper.w, y], RAIL_TOP, P), { tone: "faint" }) + Dots([-POST_X, POST_X].map((x) => at([x, y + 1.6, RAIL_TOP])), { size: 0.45 }),
  });
  const lower = { x: -POST_X + 3, y: y - 2.5, w: POST_X * 2 - 6, d: 5, r: 0.6 };
  slab(`rail.lower`, lower, LOWER_Z, 3.6, { tone: "mid" }, { bevel: 0.3 });
  for (const x of [-POST_X + 4, POST_X - 7]) slab(`rail.stop`, { x, y: y - 3.5, w: 3, d: 7, r: 0.6 }, RAIL_TOP, 4, { tone: "lo" }, { extra: Dots([at([x + 1.5, y, RAIL_TOP + 4])], { size: 0.4 }) });
}

// ---------------------------------------------------------------- type-case cabinet (the mutable table)
slab("cab.plinth", k.insetPlan(CAB, 2), 0, 4, { tone: "lo" });
{
  const body = slab("cab.body", CAB, 4, CASE_Z - 4, { tone: "mid", inner: Ln(k.sideSeam(CAB, 36, P, 3), { tone: "faint" }) }, { bevel: 0.6 });
  const side = CAB.x + CAB.w;
  body.svg += Ln(path3([[side, CAB.y + 6, 10], [side, CAB.y + CAB.d - 6, 10], [side, CAB.y + CAB.d - 6, 52], [side, CAB.y + 6, 52]], true), { tone: "faint" });
  body.svg += Dots([[CAB.y + 9, 13], [CAB.y + CAB.d - 9, 13], [CAB.y + 9, 49], [CAB.y + CAB.d - 9, 49]].map(([y, z]) => at([side, y, z])), { size: 0.45, tone: "mid" });
}
// A lifting handle on the side that faces the stone.
{
  const side = CAB.x + CAB.w;
  for (const y of [-11, 11]) slab("cab.handle.post", { x: side, y: y - 0.8, w: 1.8, d: 1.6, r: 0.4 }, 44, 1.8, { tone: "mid" }, { steps: 2 });
  slab("cab.handle", { x: side + 1.8, y: -13, w: 1.2, d: 26, r: 0.5 }, 43.8, 2.2, { tone: "hi" }, { steps: 2, bevel: 0.2 });
}
// Seven closed cases under the open one: shards 32-255.
const DRAWER_Z0 = 5.6;
const DRAWER_H = 6.6;
const DRAWER_GAP = 0.75;
for (let i = 0; i < M.DRAWERS; i++) {
  const z = DRAWER_Z0 + i * (DRAWER_H + DRAWER_GAP);
  const facePlan = { x: CAB.x + 2, y: CAB.y + CAB.d, w: CAB.w - 4, d: 1.4, r: 0.5 };
  const yf = facePlan.y + facePlan.d;
  const cx = CAB.x + CAB.w / 2;
  const holder = [[cx - 18, yf, z + 1.6], [cx - 9, yf, z + 1.6], [cx - 9, yf, z + 5], [cx - 18, yf, z + 5]];
  slab("cab.drawer", facePlan, z, DRAWER_H, { tone: "mid" }, {
    steps: 2,
    bevel: 0.3,
    extra: Ln(path3(holder, true), { tone: "lo" }) + Dots([at([cx - 19.4, yf, z + 3.3]), at([cx - 7.6, yf, z + 3.3])], { size: 0.4 }),
  });
  for (const x of [cx + 6, cx + 16]) slab("cab.pull.post", { x: x - 0.8, y: yf, w: 1.6, d: 1.8, r: 0.4 }, z + 2.6, 1.6, { tone: "mid" }, { steps: 2 });
  slab("cab.pull", { x: cx + 4.6, y: yf + 1.8, w: 12.8, d: 1.2, r: 0.5 }, z + 2.4, 2, { tone: "hi" }, { steps: 2 });
}
// The open case on top: 32 pigeonholes, each holding one card, the root of its shard.
const holes = [];
for (let r = 0; r < M.CASE_ROWS; r++) for (let c = 0; c < M.CASE_COLS; c++) holes.push([c, r]);
const CASE_PLAN = { x: CAB.x, y: CAB.y, w: CAB.w, d: CAB.d, r: CAB.r };
const caseItem = (() => {
  const shapes = [R.solid(A.slab(CASE_PLAN, CASE_Z, CASE_FLOOR - CASE_Z, { name: "cab.case" }, 3))];
  for (const wall of [
    { x: CAB.x, y: CAB.y, w: CAB.w, d: 1.6 },
    { x: CAB.x, y: CAB.y + CAB.d - 1.6, w: CAB.w, d: 1.6 },
    { x: CAB.x, y: CAB.y + 1.6, w: 1.6, d: CAB.d - 3.2 },
    { x: CAB.x + CAB.w - 1.6, y: CAB.y + 1.6, w: 1.6, d: CAB.d - 3.2 },
  ]) shapes.push(R.solid(A.slab({ ...wall, r: 0.2 }, CASE_FLOOR, CAB_TOP - CASE_FLOOR, { name: "cab.case.rim", owner: "cab.case" }, 2)));
  for (let c = 1; c < M.CASE_COLS; c++) shapes.push(R.solid(A.slab({ x: CASE_X0 + CELL_W * c - 1, y: CAB.y + 1.6, w: 2, d: CAB.d - 3.2, r: 0.1 }, CASE_FLOOR, CAB_TOP - CASE_FLOOR, { name: "cab.case.divider", owner: "cab.case" }, 2)));
  for (let r = 1; r < M.CASE_ROWS; r++) shapes.push(R.solid(A.slab({ x: CAB.x + 1.6, y: CASE_Y0 + CELL_D * r - 1, w: CAB.w - 3.2, d: 2, r: 0.1 }, CASE_FLOOR, CAB_TOP - CASE_FLOOR, { name: "cab.case.divider", owner: "cab.case" }, 2)));
  const parts = [S(k.slabOf(CASE_PLAN, CASE_Z, CAB_TOP - CASE_Z, P, 3, 0.6), { tone: "mid", inner: Ln(k.sideSeam(CASE_PLAN, CASE_FLOOR, P, 3), { tone: "faint" }) })];
  const open = [];
  for (const [c, r] of holes) {
    const [x, y] = holeCentre(c, r);
    const o = { x: x - CELL_W / 2 + 1, y: y - CELL_D / 2 + 1, w: CELL_W - 2, d: CELL_D - 2 };
    const A0 = [o.x, o.y];
    const B0 = [o.x + o.w, o.y];
    const C0 = [o.x, o.y + o.d];
    const D0 = [o.x + o.w, o.y + o.d];
    const q = (pts) => k.pathOf(pts.map(at), true);
    parts.push(`<path class="iso-shade" data-shade="0" d="${q([[...A0, CASE_FLOOR], [...B0, CASE_FLOOR], [...D0, CASE_FLOOR], [...C0, CASE_FLOOR]])}"/>`);
    parts.push(`<path class="iso-shade" data-shade="2" d="${q([[...A0, CAB_TOP], [...B0, CAB_TOP], [...B0, CASE_FLOOR], [...A0, CASE_FLOOR]])}"/>`);
    parts.push(`<path class="iso-shade" data-shade="1" d="${q([[...A0, CAB_TOP], [...C0, CAB_TOP], [...C0, CASE_FLOOR], [...A0, CASE_FLOOR]])}"/>`);
    open.push(q([[...A0, CAB_TOP], [...B0, CAB_TOP], [...D0, CAB_TOP], [...C0, CAB_TOP]]));
  }
  parts.push(Ln(open.join(""), { tone: "lo" }));
  const textAt = k.sideMatrix([CAB.x + 4, CAB.y + CAB.d], CAB_TOP - 5.6, P, "left");
  parts.push(k.faceTextSvg(textAt, "SHARDS 0-31 OF 256", { size: 2.6, tone: "lo" }));
  return put("cab.case", [CAB.x + CAB.w / 2, 0, (CASE_Z + CAB_TOP) / 2], parts.join(""), shapes);
})();
// The cards. Each is clipped to its pigeonhole's opening and the space above it.
const swept = (plan, z0, z1) => {
  const pts = [];
  for (const [x, y] of [[plan.x, plan.y], [plan.x + plan.w, plan.y], [plan.x + plan.w, plan.y + plan.d], [plan.x, plan.y + plan.d]]) pts.push(at([x, y, z0]), at([x, y, z1]));
  return k.pathOf(hull(pts), true);
};
function hull(points) {
  const pts = [...points].sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const cross = (o, a, b) => (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
  const lower = [];
  for (const p of pts) { while (lower.length >= 2 && cross(lower[lower.length - 2], lower[lower.length - 1], p) <= 0) lower.pop(); lower.push(p); }
  const upper = [];
  for (const p of pts.reverse()) { while (upper.length >= 2 && cross(upper[upper.length - 2], upper[upper.length - 1], p) <= 0) upper.pop(); upper.push(p); }
  return lower.slice(0, -1).concat(upper.slice(0, -1));
}
const cardPlan = (x, y) => ({ x: x - 3.6, y: y - 0.5, w: 7.2, d: 1, r: 0.3 });
function cardSvg(x, y, style) {
  return S(k.slabOf(cardPlan(x, y), CASE_FLOOR, CARD_TOP - CASE_FLOOR, P, 2, 0.25), style) +
    Ln(k.pathOf([at([x - 2.4, y + 0.5, CARD_TOP - 2.2]), at([x + 1.2, y + 0.5, CARD_TOP - 2.2])]), { tone: "lo", free: true });
}
for (const [c, r] of holes) {
  const [x, y] = holeCentre(c, r);
  const o = { x: x - CELL_W / 2 + 1, y: y - CELL_D / 2 + 1, w: CELL_W - 2, d: CELL_D - 2 };
  const id = `mp-hole-${c}-${r}`;
  const ours = c === HOLE.col && r === HOLE.row;
  const shape = R.solid(A.slab(cardPlan(x, y), CASE_FLOOR, CARD_TOP - CASE_FLOOR, { name: "card" }, 2));
  const inner = ours
    ? `<g data-part="card-old">${cardSvg(x, y, { tone: "hi", lit: true })}</g><g data-part="card-new" style="display:none">${cardSvg(x, y, { tone: "hi", lit: true })}</g>`
    : cardSvg(x, y, { tone: "mid" });
  put(ours ? "card.ours" : "card", [x, y, CARD_TOP + 30], `<clipPath id="${id}"><path d="${swept(o, CASE_FLOOR - 0.01, CARD_TOP + 14)}"/></clipPath><g clip-path="url(#${id})">${inner}</g>`, [shape], { bias: 60 });
}
// The latch on our pigeonhole: a bolt between two guides on the rim in front of it. It shoots
// toward the card when a swap holds and jolts back when the token has changed meanwhile.
const [HX, HY] = holeCentre(HOLE.col, HOLE.row);
const RIM_Y = HY + CELL_D / 2 - 1;
for (const x of [HX - 4.4, HX + 3.2]) slab("cab.latch.guide", { x, y: RIM_Y + 0.2, w: 1.2, d: 1.6, r: 0.3 }, CAB_TOP, 1.6, { tone: "lo" }, { steps: 2 });
const latchBolt = S(k.slabOf({ x: HX - 3, y: RIM_Y + 0.3, w: 6, d: 1.4, r: 0.4 }, CAB_TOP, 1.1, P, 2, 0.2), { tone: "hi" }) +
  Dots([at([HX + 2.2, RIM_Y + 1, CAB_TOP + 1.1])], { size: 0.45, tone: "hi" });

// ---------------------------------------------------------------- composing stone (the pages)
const LEG = 7;
const legPlans = [
  { x: STONE.x + 3, y: STONE.y + 3, w: LEG, d: LEG, r: 0.8, back: true },
  { x: STONE.x + STONE.w - 3 - LEG, y: STONE.y + 3, w: LEG, d: LEG, r: 0.8, back: true },
  { x: STONE.x + 3, y: STONE.y + STONE.d - 3 - LEG, w: LEG, d: LEG, r: 0.8 },
  { x: STONE.x + STONE.w - 3 - LEG, y: STONE.y + STONE.d - 3 - LEG, w: LEG, d: LEG, r: 0.8 },
];
for (const plan of legPlans) {
  slab(plan.back ? "stone.leg.back" : "stone.leg.front", plan, 0, STONE_Z, { tone: "mid", inner: Ln(k.sideSeam(plan, 6, P, 3), { tone: "faint" }) }, { bevel: 0.3, bias: plan.back ? 0 : 40 });
}
const AP_Z = 30;
const apronX0 = STONE.x + 3 + LEG;
const apronX1 = STONE.x + STONE.w - 3 - LEG;
const apronY0 = STONE.y + 3 + LEG;
const apronY1 = STONE.y + STONE.d - 3 - LEG;
slab("stone.apron.back", { x: apronX0, y: STONE.y + 4, w: apronX1 - apronX0, d: 3, r: 0.4 }, AP_Z, STONE_Z - AP_Z, { tone: "lo" }, { steps: 2 });
slab("stone.apron.left", { x: STONE.x + 4, y: apronY0, w: 3, d: apronY1 - apronY0, r: 0.4 }, AP_Z, STONE_Z - AP_Z, { tone: "lo" }, { steps: 2 });
slab("stone.apron.right", { x: STONE.x + STONE.w - 7, y: apronY0, w: 3, d: apronY1 - apronY0, r: 0.4 }, AP_Z, STONE_Z - AP_Z, { tone: "mid" }, { steps: 2 });
const APRON_FRONT = { x: apronX0, y: STONE.y + STONE.d - 7, w: apronX1 - apronX0, d: 3, r: 0.4 };
slab("stone.apron.front", APRON_FRONT, AP_Z, STONE_Z - AP_Z, { tone: "mid" }, { steps: 2, extra: Dots([apronX0 + 6, apronX1 - 6].map((x) => at([x, APRON_FRONT.y + 3, 34])), { size: 0.45 }) });
// The readers' rail, bolted to the front apron on two brackets.
const RRAIL = { x: READER.rail.x, y: APRON_FRONT.y + 3 + 2.4, w: READER.rail.w, d: 2.6, r: 0.5 };
for (const x of [apronX0 + 14, apronX1 - 14]) slab("stone.rail.bracket", { x: x - 2, y: APRON_FRONT.y + 3, w: 4, d: 2.4, r: 0.3 }, 31, 4, { tone: "lo" }, { steps: 2 });
slab("stone.rail", RRAIL, 31.4, 3.2, { tone: "mid" }, { steps: 2, bevel: 0.3, extra: Ln(k.lineOnTop([RRAIL.x, RRAIL.y + 1.3], [RRAIL.x + RRAIL.w, RRAIL.y + 1.3], 34.6, P), { tone: "faint" }) });
for (const x of [RRAIL.x - 2.4, RRAIL.x + RRAIL.w]) slab("stone.rail.stop", { x, y: RRAIL.y - 0.6, w: 2.4, d: 3.8, r: 0.4 }, 30.6, 5, { tone: "lo" }, { steps: 2 });
READER.y0 = RRAIL.y - 0.8;
READER.post = RRAIL.y + 1.3;
// The stone slab, its border groove and the seats the plates stand in.
const seats = [];
for (let s = 0; s < SITE_COUNT; s++) {
  const [x, y] = siteXY(s);
  seats.push(k.planOutline({ x: x - 3.6, y: y - 3.6, w: 7.2, d: 7.2, r: 0.6 }, STONE_TOP, P, 2));
}
const stoneScrews = k.corners(STONE, 4.5);
const stoneItem = slab("stone.slab", STONE, STONE_Z, STONE_TOP - STONE_Z, { tone: "mid", inner: Ln(k.sideSeam(STONE, 40, P, 3), { tone: "faint" }) }, {
  bevel: 0.8,
  extra: Ln(k.planOutline(k.insetPlan(STONE, 7), STONE_TOP, P, 3), { tone: "lo" }) + Ln(seats.join(""), { tone: "faint" }) +
    Ln(stoneScrews.map(([x, y]) => k.ring(x, y, 1.6, STONE_TOP, P, 12)).join(""), { tone: "lo" }) + Dots(stoneScrews.map(([x, y]) => at([x, y, STONE_TOP])), { size: 0.45 }),
});

// Quoins: the wedges that lock a forme in its chase, one on each short edge of the stone.
for (const x of [STONE.x + 2.2, STONE.x + STONE.w - 5.2]) {
  const q = { x, y: -7, w: 3, d: 14, r: 0.5 };
  slab("stone.quoin", q, STONE_TOP, 2, { tone: "lo" }, { steps: 2, bevel: 0.2, extra: Dots([at([x + 1.5, -4, STONE_TOP + 2]), at([x + 1.5, 4, STONE_TOP + 2])], { size: 0.4 }) });
}

// The plates. One per seat, shown when the model has a plate there.
const plateAt = (x, y) => ({ x: x - 3, y: y - 3, w: 6, d: 6, r: 0.5 });
function plateSvg(x, y) {
  const plan = plateAt(x, y);
  return S(k.slabOf(plan, STONE_TOP, PLATE_H, P, 2, 0.3), { tone: "hi", crease: "lo", inner: Ln(k.sideSeam(plan, STONE_TOP + 1.2, P, 2), { tone: "faint" }) }) +
    Ln(k.planOutline(k.insetPlan(plan, 1.2), PLATE_TOP, P, 2), { tone: "lo" });
}
for (let s = 0; s < SITE_COUNT; s++) {
  const [x, y] = siteXY(s);
  const shape = R.solid(A.slab(plateAt(x, y), STONE_TOP, PLATE_H, { name: "plate" }, 2));
  const clip = swept({ x: x - 3.6, y: y - 3.6, w: 7.2, d: 7.2 }, STONE_TOP, STONE_TOP + 30);
  const mark = Dots([at([x, y, PLATE_TOP])], { size: 0.85, tone: "hi" });
  put("plate", [x, y, STONE_TOP + PLATE_H / 2],
    `<clipPath id="mp-seat-${s}"><path d="${clip}"/></clipPath><g clip-path="url(#mp-seat-${s})"><g data-sink>${plateSvg(x, y)}<g class="mp-mark">${mark}</g></g></g>`,
    [shape], { attrs: ` data-site="${s}"`, cls: "mp-plate" });
}

// ---------------------------------------------------------------- collector rail and free stock under the bench top
for (const y of [COLLECT.y0, COLLECT.y1]) {
  const rail = { x: COLLECT.x0, y, w: COLLECT.x1 - COLLECT.x0, d: 3, r: 0.6 };
  slab("collector.rail", rail, 0, 2.4, { tone: "lo" }, { steps: 2, extra: Dots([COLLECT.x0 + 4, (COLLECT.x0 + COLLECT.x1) / 2, COLLECT.x1 - 4].map((x) => at([x, y + 1.5, 2.4])), { size: 0.4 }) });
}
for (const x of [COLLECT.x0 - 3, COLLECT.x1]) slab("collector.stop", { x, y: COLLECT.y0 - 0.5, w: 3, d: COLLECT.y1 - COLLECT.y0 + 4, r: 0.6 }, 0, 5, { tone: "mid" }, { steps: 2 });
slab("bin.floor", BIN, 0, 1.6, { tone: "lo" });
for (const wall of [
  { x: BIN.x, y: BIN.y, w: BIN.w, d: 1.4 },
  { x: BIN.x, y: BIN.y + 1.4, w: 1.4, d: BIN.d - 2.8 },
  { x: BIN.x + BIN.w - 1.4, y: BIN.y + 1.4, w: 1.4, d: BIN.d - 2.8 },
  { x: BIN.x, y: BIN.y + BIN.d - 1.4, w: BIN.w, d: 1.4 },
]) slab("bin.wall", { ...wall, r: 0.3 }, 1.6, 4, { tone: "mid" }, { steps: 2 });
for (let i = 0; i < STOCK; i++) {
  const [x, y] = binXY(i);
  const plan = { x: x - 2.4, y: y - 2.4, w: 4.8, d: 4.8, r: 0.5 };
  const shape = R.solid(A.slab(plan, 1.6, 3, { name: "stock" }, 2));
  put("stock", [x, y, 3], S(k.slabOf(plan, 1.6, 3, P, 2, 0.3), { tone: "mid" }), [shape], { attrs: ` data-stock="${i}"`, cls: "mp-stock" });
}

// ---------------------------------------------------------------- moving parts (built at rest, moved by the page)
const HOP = { ...HOPPER };
function collectorSvg() {
  const out = [`<path class="iso-halo" d="${k.haloOf({ x: -9, y: YC - 10, w: 18, d: 20, r: 2 }, 2, 0.1, P)}" style="filter:blur(3px)"/>`];
  for (const [x, y] of byDepth([[-6, COLLECT.y0 + 1.5], [6, COLLECT.y0 + 1.5], [-6, COLLECT.y1 + 1.5], [6, COLLECT.y1 + 1.5]])) out.push(S(k.slabOf({ x: x - 2.4, y: y - 2.2, w: 4.8, d: 4.4, r: 0.6 }, 2.4, 2.4, P, 2), { tone: "lo" }));
  const base = { x: -9, y: YC - 10, w: 18, d: 20, r: 1 };
  out.push(S(k.slabOf(base, 4.8, 2.2, P, 3, 0.3), { tone: "mid" }));
  out.push(Dots(k.corners(base, 1.8).map(([x, y]) => at([x, y, 7])), { size: 0.4 }));
  const hop = { x: HOP.x, y: HOP.y, w: HOP.w, d: HOP.d, r: 0.6 };
  out.push(S(k.slabOf(hop, 7, HOP.top - 7, P, 2, 0.4), { tone: "mid", inner: Ln(k.sideSeam(hop, 12, P, 2), { tone: "faint" }) }));
  out.push(`<g data-part="pile">${MOVERS.pile(0)}</g>`);
  out.push(S(k.slabOf({ x: 9, y: YC - 2, w: 2.4, d: 4, r: 0.4 }, 7.4, 4, P, 2), { tone: "mid" }));
  out.push(S(k.cylinder(12.6, YC, 1.6, 8, 3.4, P, 16), { tone: "hi" }));
  out.push(`<g data-part="sweep-lamp">${S(k.cylinder(12.6, YC, 1.6, 8, 3.4, P, 16), { tone: "hi", lit: true })}</g>`);
  return out.join("");
}
function bridgeSvg(id, lit) {
  const out = [];
  const car = { x: -7, y: -RAIL_Y + 2.5, w: 14, d: 7, r: 1 };
  out.push(S(k.slabOf(car, LOWER_Z - 3, ARM_Z - LOWER_Z + 3, P, 3, 0.4), { tone: "mid", inner: Ln(k.sideSeam(car, LOWER_Z + 8, P, 3), { tone: "faint" }) }));
  out.push(Dots([[-4.5, LOWER_Z + 1.8], [4.5, LOWER_Z + 1.8], [-4.5, RAIL_Z + 2], [4.5, RAIL_Z + 2]].map(([x, z]) => at([x, car.y + car.d, z])), { size: 0.45, tone: "mid" }));
  out.push(S(k.slabOf({ x: 7, y: car.y + 1, w: 1.6, d: 5, r: 0.3 }, RAIL_Z - 8, 7, P, 2), { tone: "lo" }));
  out.push(S(k.cylinder(11.2, car.y + 3.5, 2.6, RAIL_Z - 8.5, 8, P, 20), { tone: "mid", inner: Ln(k.knurl(11.2, car.y + 3.5, 2.6, RAIL_Z - 4, RAIL_Z - 0.5, 16, P), { tone: "lo" }) }));
  out.push(S(k.slabOf({ x: -2.6, y: car.y + car.d, w: 5.2, d: 12, r: 0.5 }, ARM_Z - 6, 6, P, 2, 0.3), { tone: "mid" }));
  const arm = { x: -4, y: car.y + 0.5, w: 8, d: ARM_TIP - car.y - 0.5, r: 0.8 };
  out.push(S(k.slabOf(arm, ARM_Z, 4, P, 3, 0.4), { tone: "mid", inner: Ln(k.sideSeam(arm, ARM_Z + 2, P, 3), { tone: "faint" }) }));
  out.push(S(k.slabOf({ x: -4.6, y: ARM_TIP, w: 9.2, d: 2.2, r: 0.6 }, ARM_Z - 0.4, 4.8, P, 2, 0.3), { tone: "lo" }));
  const head = [];
  const top = ARM_Z + 4;
  head.push(S(k.slabOf({ x: -5, y: -5.5, w: 10, d: 11, r: 1 }, top, 4.6, P, 3, 0.4), { tone: "hi", crease: "lo" }));
  head.push(Dots([at([-3, -3.5, top + 4.6]), at([3, 3.5, top + 4.6])], { size: 0.4 }));
  head.push(S(k.slabOf({ x: 4, y: -4, w: 2, d: 8, r: 0.4 }, BRIDGE.nozzle + 12, top + 4.6 - BRIDGE.nozzle - 12, P, 2, 0.2), { tone: "mid" }));
  for (const z of [BRIDGE.nozzle + 13, top - 1]) head.push(S(k.slabOf({ x: 6, y: -2, w: 3.6, d: 4, r: 0.4 }, z, 2.6, P, 2), { tone: "mid" }));
  head.push(S(k.cylinder(BRIDGE.spindle, 0, 1.5, BRIDGE.nozzle + 5, top + 1.6 - BRIDGE.nozzle - 5, P, 16), { tone: "mid", crease: "none", inner: Ln(k.sideArc(BRIDGE.spindle, 0, 1.5, BRIDGE.nozzle + 9, P, 12), { tone: "faint" }) }));
  head.push(S(k.cylinder(BRIDGE.spindle, 0, 2.9, BRIDGE.nozzle, 5, P, 24, 0.3), { tone: "hi", lit }));
  head.push(Ln(k.ring(BRIDGE.spindle, 0, 1.4, BRIDGE.nozzle + 5, P, 16), { tone: lit ? "lit" : "mid" }));
  out.push(`<g data-part="head">${head.join("")}</g>`);
  return `<g class="mp-bridge" data-bridge="${id}">${out.join("")}</g>`;
}

const DATA = {
  P,
  sites: Array.from({ length: SITE_COUNT }, (_, s) => siteXY(s).map(f2)),
  stoneTop: STONE_TOP,
  plateH: PLATE_H,
  hole: [HX, HY],
  cardTop: CARD_TOP,
  nozzle: BRIDGE.nozzle,
  spindle: BRIDGE.spindle,
  park: { w1: BRIDGE.w1Park, w2: BRIDGE.w2Park },
  gap: BRIDGE.gap,
  reader: READER,
  readerRange: [RRAIL.x + 6, RRAIL.x + RRAIL.w - 6],
  hopper: HOP,
  collect: COLLECT,
  bin: Array.from({ length: STOCK }, (_, i) => binXY(i).map(f2)),
  stock: STOCK,
  leaves: Array.from({ length: M.LEAVES }, (_, j) => nodeXY(M.leafNode(j))[0]),
};
const MOVERS = makeMovers(k, P, DATA);

// ---------------------------------------------------------------- order
A.settle(R, P);
R.items.sort((a, b) => a.key - b.key);
const items = R.items.filter((item) => !item.front);
const frontItems = R.items.filter((item) => item.front);
const idx = (test) => items.findIndex(test);
const lastIdx = (test) => items.reduce((best, item, i) => (test(item) ? i : best), -1);
// The front legs stand in front of everything under the stone, so they are drawn after the
// collector's rails, whatever key the solver left them.
{
  const after = Math.max(lastIdx((item) => item.name.startsWith("collector.rail")), lastIdx((item) => item.name.startsWith("stone.leg.back")));
  const legs = items.filter((item, i) => i < after && item.name.startsWith("stone.leg.front"));
  for (const leg of legs) items.splice(items.indexOf(leg), 1);
  items.splice(items.indexOf(items.filter((item) => item.name.startsWith("collector.rail") || item.name.startsWith("stone.leg.back")).pop()) + 1, 0, ...legs);
}
const slabAt = idx((item) => item.name === "stone.slab");
const lastPlate = lastIdx((item) => item.name.startsWith("plate"));
const underFrom = Math.min(...["stone.slab", "stone.leg.front", "stone.apron.front", "stone.rail"].map((n) => idx((item) => item.name === n || item.name.startsWith(n + "#") || (n !== "stone.rail" && item.name.startsWith(n)))).filter((i) => i >= 0));
const underAfter = Math.max(lastIdx((item) => item.name.startsWith("collector.rail")), lastIdx((item) => item.name.startsWith("stone.leg.back")));
const outAfter = lastIdx((item) => item.name.startsWith("bin") || item.name.startsWith("stock") || item.name.startsWith("collector"));
if (underAfter >= underFrom) console.warn("order: the collector slot sits after a part in front of it", underAfter, underFrom, items[underAfter].name, items[underFrom].name);
const slots = new Map();
const addSlot = (i, svg) => slots.set(i, (slots.get(i) ?? "") + svg);
addSlot(underAfter, `<g data-slot="under"><g data-part="collector" transform="${k.translateAlong(P, [COLLECT.home, 0, 0])}">${collectorSvg()}</g></g>`);
addSlot(outAfter, `<g data-slot="out"></g>`);
addSlot(slabAt, `<g data-part="links"><path class="iso-line" data-tone="faint" data-part="links-old" d=""/><path class="iso-line" data-tone="mid" data-part="links-now" d=""/></g>`);
addSlot(lastPlate, `<g data-part="falling"></g>`);
const caseIndex = items.indexOf(caseItem);
addSlot(caseIndex, "");
const group = (item) => `<g class="it${item.cls ? " " + item.cls : ""}" data-name="${item.name}"${item.attrs ?? ""}>${item.svg}</g>`;
const body = [];
items.forEach((item, i) => {
  body.push(group(item));
  if (slots.has(i)) body.push(slots.get(i));
});
const latchSvg = `<g data-part="latch">${latchBolt}</g>`;
const svg = k.figureSvg({
  width: W,
  height: H,
  label:
    "A printer's type-case cabinet on a workbench. Behind, an open case of 32 pigeonholes holds one card each, with seven closed cases below; beside it a composing stone carries plates set in a tree, each page version sharing most of its plates with the one before. Two writer gantries cast new plates and swap a card through a latch; reader heads on the front rail read pages; a collector carriage under the stone sinks plates no card or reader reaches into its hopper and returns them to a stock bin.",
  body: [
    ...back,
    `<g class="mp-scene">${body.join("")}</g>`,
    latchSvg,
    `<g data-part="readers"></g>`,
    ...frontItems.map(group),
    `<g data-part="w2" transform="${k.translateAlong(P, [BRIDGE.w2Park, 0, 0])}">${bridgeSvg("w2", false)}</g>`,
    `<g data-part="w1" transform="${k.translateAlong(P, [BRIDGE.w1Park, 0, 0])}">${bridgeSvg("w1", true)}</g>`,
    `<g data-part="carry"></g>`,
  ],
});

// The falling plate, built once at the origin seat and moved.
DATA.fallSvg = plateSvg(0, 0);
DATA.cardSvg = cardSvg(0, 0, { tone: "hi", lit: true });

const CSS = `
.mp-plate:not([data-on]){display:none}
.mp-stock:not([data-on]){display:none}
.mp-mark{opacity:0;transition:opacity .25s var(--anatomy-ease)}
.mp-plate[data-mark] .mp-mark{opacity:1}
.mp-plate[data-state="old"] .iso-edge{stroke:var(--anatomy-lo)}
.mp-plate[data-state="old"] .iso-top{fill:var(--anatomy-shade-1)}
.mp-plate[data-state="held"] .iso-edge{stroke:var(--anatomy-mid)}
.mp-plate[data-state="new"] .iso-edge{stroke:var(--anatomy-lit)}
.mp-plate[data-state="new"] .iso-top{fill:var(--anatomy-lit-top)}
.mp-void{fill:var(--anatomy-shade-0)}
[data-part="sweep-lamp"]{opacity:0;transition:opacity .3s var(--anatomy-ease)}
[data-part="collector"][data-on] [data-part="sweep-lamp"]{opacity:1}
.mp-scan{fill:var(--anatomy-lit);opacity:0}
.iso-stage{touch-action:pan-y;cursor:pointer}
.iso-stage:focus{outline:none}
.iso-stage:focus-visible{outline:1px solid var(--anatomy-muted);outline-offset:6px}
.mp-sr{position:absolute;width:1px;height:1px;overflow:hidden;clip-path:inset(50%);white-space:nowrap}
`;

const LIVE = readFileSync(join(HERE, "live.js"), "utf8").replace("__DATA__", JSON.stringify(DATA));
const script = readFileSync(join(HERE, "kit", "iso-kit.mjs"), "utf8").replace(/^export /gm, "") + "\n" + strip("model.mjs") + "\n" + strip("movers.mjs") + "\n" + LIVE;

const page = k.pageHtml({
  title: "Mutability anatomy",
  theme: THEME,
  width: 680,
  script,
  body:
    `<style>${CSS}</style>` +
    k.plateHtml({
      fig: "Fig 1",
      title: "One place to mutate",
      hint: "W write · C race · G collect · ±",
      readout: "v2 · 2 readers · 73% shared · 4 plates pending",
      keys: [
        { mark: "lit", label: "The token for one object, its latch and the writer that swaps it" },
        { mark: "raised", label: "Plates: immutable cells, shared between page versions" },
        { mark: "dim", label: "Plates no token or reader reaches: garbage for the collector" },
        { mark: "flat", label: "Readers, reading whatever version they took, with no lock" },
      ],
      caption:
        "protoCore keeps mutability in one place. Every value is built from immutable 64-byte cells (one cache line each), drawn here as type plates; a mutable object's state is one version of such a tree. A write never touches a plate: it casts new plates for the path from the changed field to the root and shares every other plate with the previous version, then publishes by a compare-and-swap on the object's entry in a table of 256 shards, the pigeonholes (the open case shows shards 0-31). Each shard root is an immutable sparse list, so a single write copies roughly ten cells in all, the object's path and the shard's path; setAttributes publishes a run of changes as one version (five fields of a new object: 16 cells instead of 52). If another writer published first, the latch refuses and the writer re-applies its change on the newer version. Readers take whatever version the token names and read it with no lock, because it never changes under them. The collector pauses every thread only to capture their roots and the 256 shard roots (at most 0.23 ms per cycle measured), then marks with no write barrier, walking immutable cells while threads keep running, and the sweep returns unreachable cells to free stock: the memory stays with the process, not with the OS.",
      body: `<div class="iso-stage" tabindex="0" role="group" aria-label="Type-case model of protoCore mutability. W writes one change, Shift W writes three changes as one version, C starts two writers that race, plus and minus add or remove readers, G runs the collector.">${svg}<span class="mp-sr" role="status" aria-live="polite" data-status></span></div>`,
    }),
});
const name = THEME === "light" ? "mutability-anatomy-light.html" : "mutability-anatomy.html";
writeFileSync(join(HERE, name), page);
console.log("wrote", name, `${(page.length / 1024).toFixed(0)} KB`, `${R.items.length} recorded items`, `${solids} solids`, `scale ${k.screenRise ? "" : ""}`);
// Each card is clipped to its own pigeonhole opening and the space above it, so the case walls in
// front of a card never need to be painted over it: the clip already hides what they would cover.
A.auditOrExit(R, P, { allow: [["card*", "cab.case"]] });
