// The truth that drives the figure: a pure model of one mutable object in protoCore.
//
// A version of the object's state is an immutable tree of cells (the "page"), drawn as
// plates on a composing stone. A write never touches a plate: it casts new plates for the
// path from the changed leaf up to the root (path copy) and shares every other plate with
// the previous version. The current version is named by one token in one pigeonhole of the
// type case (the mutable-shard table), and a write publishes by compare-and-swap on it.
// The collector frees the plates that no root reaches.
//
// This file is imported by build.mjs and inlined (exports stripped) into the page.

// Numbers from protoCore (docs/GarbageCollector.md, docs/MemoryModel.md, CHANGELOG 2.11.0).
export const TRUTH = {
  cellBytes: 64, // one Cell is one 64-byte cache line
  shards: 256, // MUTABLE_ROOT_SHARDS
  snapshotBytes: 2048, // 256 shard-root pointers captured at the pause
  pauseMaxMs: 0.23, // measured stop-the-world window per cycle (2026-10-03)
  fieldByField: 52, // a fresh 5-field mutable object written field by field, in cells
  oneGroup: 16, // the same five fields published with one setAttributes, in cells
};

// The figure's page: a balanced binary tree of 15 plates, 4 levels deep.
export const LEVELS = 4;
export const NODES = 15;
export const LEAVES = 8;
export const SITES = 4; // seats on the stone for each node of the tree
export const CASE_COLS = 4;
export const CASE_ROWS = 8; // 32 pigeonholes visible: shards 0-31
export const DRAWERS = 7; // seven closed cases below hold shards 32-255

export const levelOf = (n) => Math.floor(Math.log2(n + 1));
export const parentOf = (n) => (n - 1) >> 1;
export const kidsOf = (n) => (n < 7 ? [2 * n + 1, 2 * n + 2] : null);
export const leafNode = (j) => 7 + j;
export const nodeOfSite = (s) => Math.floor(s / SITES);

export function createWorld(free) {
  return { plates: {}, token: -1, version: 0, history: [], free, born: 0, reserved: {} };
}

export function siteOfNode(world, root, node) {
  const chain = [];
  for (let n = node; n > 0; n = parentOf(n)) chain.push(n);
  let s = root;
  for (let i = chain.length - 1; i >= 0; i--) s = world.plates[s].kids[(chain[i] - 1) % 2];
  return s;
}

// The nodes a change to these leaves must copy, deepest first (a path copy is built bottom up).
export function pathNodes(leaves) {
  const nodes = new Set();
  for (const j of leaves) for (let n = leafNode(j); ; n = parentOf(n)) {
    nodes.add(n);
    if (n === 0) break;
  }
  return [...nodes].sort((a, b) => b - a);
}

// Plan a new version on top of `base`: one new plate per node on the changed paths, every
// other child shared with `base`. Returns null when a node has no free seat (the writer waits).
export function compose(world, base, leaves) {
  const nodes = pathNodes(leaves);
  const fresh = {};
  for (const n of nodes) {
    let pick = -1;
    for (let k = 0; k < SITES; k++) {
      const s = n * SITES + k;
      if (!world.plates[s] && !world.reserved[s]) { pick = s; break; }
    }
    if (pick < 0) return null;
    fresh[n] = pick;
  }
  if (world.free < nodes.length) return null;
  for (const n of nodes) world.reserved[fresh[n]] = true;
  const steps = nodes.map((n) => ({
    node: n,
    site: fresh[n],
    kids: kidsOf(n) ? kidsOf(n).map((c) => (fresh[c] !== undefined ? fresh[c] : siteOfNode(world, base, c))) : null,
  }));
  return { base, leaves, steps, root: fresh[0] };
}

export function cast(world, step) {
  world.plates[step.site] = { site: step.site, node: step.node, kids: step.kids, born: ++world.born };
  delete world.reserved[step.site];
  world.free -= 1;
}

// Compare-and-swap on the token: it only takes the new root if nobody published meanwhile.
export function swap(world, expected, root) {
  if (world.token !== expected) return false;
  const before = reach(world, [expected]);
  let both = 0;
  for (const s of reach(world, [root])) if (before.has(s)) both++;
  world.token = root;
  world.version += 1;
  world.history.push({ version: world.version, root, shared: both / NODES });
  return true;
}

export function reach(world, roots) {
  const seen = new Set();
  const stack = roots.filter((s) => s >= 0 && world.plates[s]);
  while (stack.length) {
    const s = stack.pop();
    if (seen.has(s) || !world.plates[s]) continue;
    seen.add(s);
    const kids = world.plates[s].kids;
    if (kids) stack.push(...kids);
  }
  return seen;
}

export const platesOf = (world) => Object.keys(world.plates).map(Number);

// Plates no token, reader or writer reaches: what the next collection will free.
export function pending(world, roots) {
  const live = reach(world, roots);
  return platesOf(world).filter((s) => !live.has(s));
}

// The starting state: version 1 cast whole, version 2 with one change, so the stone opens
// on a page that already shares 11 of its 15 plates.
export function seedWorld(free) {
  const world = createWorld(free + NODES + LEVELS);
  const first = [];
  for (let n = NODES - 1; n >= 0; n--) first.push({ node: n, site: n * SITES, kids: kidsOf(n) ? kidsOf(n).map((c) => c * SITES) : null });
  for (const step of first) cast(world, step);
  world.token = 0;
  world.version = 1;
  world.history.push({ version: 1, root: 0, shared: 0 });
  const plan = compose(world, 0, [5]);
  for (const step of plan.steps) cast(world, step);
  swap(world, 0, plan.root);
  return world;
}
