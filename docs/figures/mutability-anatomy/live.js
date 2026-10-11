const D = __DATA__;
const P = D.P;
const MV = makeMovers({ iso, pathOf, slabOf, cylinder, extrude, solidSvg, lineSvg, dotsSvg, ring }, P, D);
const stage = document.querySelector(".iso-stage");
const svg = stage.querySelector("svg");
const $ = (sel) => svg.querySelector(sel);
const readout = document.querySelector("[data-readout]");
const status = document.querySelector("[data-status]");
const still = matchMedia("(prefers-reduced-motion: reduce)");
const ease = (t) => (t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t));
const rnd = (n) => Math.floor(Math.random() * n);

const world = seedWorld(22);
const plateEl = [];
for (const el of svg.querySelectorAll(".mp-plate")) plateEl[Number(el.dataset.site)] = el;
const sinkEl = plateEl.map((el) => el && el.querySelector("[data-sink]"));
const stockEl = [];
for (const el of svg.querySelectorAll(".mp-stock")) stockEl[Number(el.dataset.stock)] = el;
const linksNow = $('[data-part="links-now"]');
const linksOld = $('[data-part="links-old"]');
const fallingEl = $('[data-part="falling"]');
const readersEl = $('[data-part="readers"]');
const collectorEl = $('[data-part="collector"]');
const pileEl = $('[data-part="pile"]');
const latchEl = $('[data-part="latch"]');
const cardEl = $('[data-part="card-old"]');
const scene = $(".mp-scene");
const slotUnder = $('[data-slot="under"]');
const slotOut = $('[data-slot="out"]');

let clock = 0;
let dirty = true;
let paused = 0; // seconds left in the collector's stop-the-world pause
const born = {}; // site -> clock when its plate landed
const landing = {}; // site -> true while its plate is still falling
const falling = [];
const sinking = {}; // site -> clock when it started to sink
let lastSay = "";

function say(text) {
  if (text === lastSay) return;
  lastSay = text;
  status.textContent = text;
  // Phase log for scripted checks (drive.mjs waits on it); kept short.
  const log = (window.__mpLog ||= []);
  log.push(clock.toFixed(1) + " " + text);
  if (log.length > 60) log.shift();
}

// ---------------------------------------------------------------- writers (the two gantries)
function makeWriter(id, park) {
  const el = $(`[data-part="${id}"]`);
  return { id, el, head: el.querySelector('[data-part="head"]'), park, x: park, y: 0, tx: park, ty: 0, queue: [], wait: 0, base: -1, plan: null, cast: [], busy: false };
}
const W1 = makeWriter("w1", D.park.w1);
const W2 = makeWriter("w2", D.park.w2);
const writers = [W2, W1];
const holeX = D.hole[0] - D.spindle;

function goto(w, x, y) {
  w.queue.push({ to: [x - D.spindle, y] });
}
function writeJob(w, leaves, { slow = 0 } = {}) {
  w.busy = true;
  goto(w, D.hole[0], D.hole[1]);
  w.queue.push({ fn: () => { w.base = world.token; say(`${w.id === "w1" ? "Writer" : "Second writer"} reads the token: v${world.version}`); bump(-2.5); return 0.35; } });
  composeSteps(w, leaves, slow);
}
function composeSteps(w, leaves, slow) {
  w.queue.push({
    fn: () => {
      const plan = compose(world, w.base, leaves);
      if (!plan) {
        say("No free plates: the writer waits for the collector");
        startCollect();
        return false;
      }
      w.plan = plan;
      w.cast = [];
      const steps = [];
      for (const step of plan.steps) {
        const [sx, sy] = D.sites[step.site];
        steps.push({ to: [sx - D.spindle, sy] });
        steps.push({ fn: () => { dropPlate(w, step); return 0.32 + slow; } });
      }
      steps.push({ to: [holeX, D.hole[1]] });
      steps.push({ fn: () => tryswap(w, leaves, slow) });
      w.queue.unshift(...steps);
      say(`Composing a new page: ${plan.steps.length} new plates, the rest shared`);
      return 0;
    },
  });
}
function dropPlate(w, step) {
  cast(world, step);
  w.cast.push(step.site);
  landing[step.site] = true;
  falling.push({ site: step.site, t0: clock });
  dirty = true;
}
let latch = { t0: -9, ok: true };
let card = { t0: -9 };
function tryswap(w, leaves, slow) {
  const plan = w.plan;
  const ok = swap(world, w.base, plan.root);
  latch = { t0: clock, ok };
  dirty = true;
  if (ok) {
    card = { t0: clock };
    w.plan = null;
    w.cast = [];
    w.base = -1;
    say(`Swap held: the token now names v${world.version}`);
    w.queue.unshift({ wait: 0.5 }, { to: [w.park, 0] }, { fn: () => { w.busy = false; return 0; } });
  } else {
    say(`Swap refused: the token moved to v${world.version}; re-applying the change on it`);
    w.queue.unshift({ wait: 0.7 }, { fn: () => { w.base = world.token; w.plan = null; w.cast = []; dirty = true; return 0.3; } });
    composeSteps(w, leaves, slow);
  }
  return 0.35;
}
function bump(dz) {
  // a short dip of the nozzle onto the card, as the head reads it
  card.read = clock;
}

function runWriter(w, dt) {
  if (w.wait > 0) {
    w.wait -= dt;
    return;
  }
  const step = w.queue[0];
  if (!step) return;
  if (step.to) {
    w.tx = step.to[0];
    w.ty = step.to[1];
    if (Math.abs(w.x - w.tx) < 0.3 && Math.abs(w.y - w.ty) < 0.3) {
      w.x = w.tx;
      w.y = w.ty;
      w.queue.shift();
    }
  } else if (step.wait !== undefined) {
    w.wait = step.wait;
    w.queue.shift();
  } else if (step.fn) {
    const r = step.fn();
    if (r === false) {
      w.wait = 0.6;
      return;
    }
    if (w.queue[0] === step) w.queue.shift();
    else w.queue.splice(w.queue.indexOf(step), 1);
    w.wait = r || 0;
  }
}
function moveWriters(dt, calm) {
  for (const w of writers) {
    const k = calm ? 1 : 1 - Math.exp(-dt / 0.16);
    let nx = w.x + (w.tx - w.x) * k;
    const sp = 320 * dt;
    if (!calm) nx = w.x + Math.max(-sp, Math.min(sp, nx - w.x));
    w.x = nx;
    const ny = w.y + (w.ty - w.y) * (calm ? 1 : 1 - Math.exp(-dt / 0.25));
    w.y = calm ? ny : w.y + Math.max(-sp, Math.min(sp, ny - w.y));
  }
  // The two bridges share the rails and never pass each other.
  if (W1.x < W2.x + D.gap) {
    if (Math.abs(W1.tx - W1.x) > Math.abs(W2.tx - W2.x)) W1.x = W2.x + D.gap;
    else W2.x = W1.x - D.gap;
  }
  for (const w of writers) {
    w.el.setAttribute("transform", translateAlong(P, [w.x, 0, 0]));
    w.head.setAttribute("transform", translateAlong(P, [0, w.y, 0]));
  }
}

// ---------------------------------------------------------------- readers
const readers = Array.from({ length: 4 }, (_, i) => ({ i, x: D.readerRange[0] + 12 + i * 22, lx: 0, ly: 0, tx: 0, ty: 0, root: -1, path: [], step: 0, dwell: 0, rest: 0.3 + i * 0.5 }));
let readerCount = 2;
for (const r of readers) {
  r.lx = r.x;
  r.ly = D.sites[0][1] + 8;
  r.tx = r.lx;
  r.ty = r.ly;
}
function readPath(root) {
  const leaf = rnd(LEAVES);
  const nodes = [];
  for (let n = leafNode(leaf); ; n = parentOf(n)) {
    nodes.unshift(n);
    if (n === 0) break;
  }
  return nodes.map((n) => siteOfNode(world, root, n));
}
function runReaders(dt, calm) {
  const RG = D.readerRange;
  readers.forEach((r, i) => {
    const active = i < readerCount;
    if (!active) {
      if (r.root >= 0) { r.root = -1; dirty = true; }
      r.tx = RG[1] - (3 - i) * 11;
      r.ty = D.sites[0][1] + 7;
    } else if (r.root < 0) {
      r.rest -= dt;
      if (r.rest <= 0) {
        r.root = world.token;
        r.version = world.version;
        r.path = readPath(r.root);
        r.step = 0;
        r.dwell = 0.9;
        dirty = true;
      }
    } else {
      const site = r.path[r.step];
      [r.tx, r.ty] = D.sites[site];
      if (Math.hypot(r.lx - r.tx, r.ly - r.ty) < 0.4) {
        r.dwell -= dt;
        if (r.dwell <= 0) {
          r.step += 1;
          r.dwell = 0.8;
          if (r.step >= r.path.length) {
            r.root = -1;
            r.rest = 0.6 + Math.random() * 0.8;
            r.ty = D.sites[0][1] + 7;
            dirty = true;
          }
        }
      }
    }
    const k = calm ? 1 : 1 - Math.exp(-dt / 0.28);
    r.lx += (r.tx - r.lx) * k;
    r.ly += (r.ty - r.ly) * k;
  });
  // Carriages follow their lenses along the rail and keep their order.
  readers.forEach((r, i) => {
    const lo = RG[0] + i * 11;
    const hi = RG[1] - (3 - i) * 11;
    const want = Math.min(hi, Math.max(lo, r.lx));
    r.x += (want - r.x) * (calm ? 1 : 1 - Math.exp(-dt / 0.35));
  });
  for (let i = 1; i < 4; i++) readers[i].x = Math.max(readers[i].x, readers[i - 1].x + 11);
  for (let i = 2; i >= 0; i--) readers[i].x = Math.min(readers[i].x, readers[i + 1].x - 11);
  readersEl.innerHTML = readers.map((r, i) => MV.reader(i, r.x, r.lx, r.ly, i < readerCount && r.root >= 0)).join("");
}

// ---------------------------------------------------------------- collector
const gc = { phase: "idle", t: 0, marked: new Set(), order: [], snapBorn: 0, x: D.collect.home, hopper: 0, freed: 0, queued: false };
function rootsNow() {
  const roots = [world.token];
  for (const r of readers) if (r.root >= 0) roots.push(r.root);
  for (const w of writers) {
    if (w.base >= 0) roots.push(w.base);
    roots.push(...w.cast);
  }
  return roots;
}
function startCollect() {
  if (gc.phase !== "idle") {
    return;
  }
  gc.phase = "pause";
  gc.t = 0;
  paused = still.matches ? 0 : 0.45;
  gc.snapBorn = world.born;
  gc.order = [...reach(world, rootsNow())];
  gc.marked = new Set();
  scene.setAttribute("data-snap", "");
  say("Pause: every thread stops while the collector copies its roots and the 256 shard roots");
}
function runCollector(dt, calm) {
  const C = D.collect;
  if (gc.phase === "pause") {
    if (paused <= 0) {
      scene.removeAttribute("data-snap");
      gc.phase = "mark";
      gc.t = 0;
      say("Marking: walking immutable plates from the snapshot while threads keep running");
    }
  } else if (gc.phase === "mark") {
    gc.t += dt;
    const n = calm ? gc.order.length : Math.min(gc.order.length, Math.floor(gc.t / 0.045));
    for (let i = gc.marked.size; i < n; i++) gc.marked.add(gc.order[i]);
    if (gc.marked.size >= gc.order.length) {
      gc.phase = "sweep";
      collectorEl.setAttribute("data-on", "");
      say("Sweeping: plates nothing reached go back to free stock");
    }
    dirty = true;
  } else if (gc.phase === "sweep") {
    const end = 108;
    gc.x = calm ? end : Math.min(end, gc.x + 34 * dt);
    for (const s of platesOf(world)) {
      if (sinking[s] !== undefined || gc.marked.has(s) || world.plates[s].born > gc.snapBorn) continue;
      if (D.sites[s][0] <= gc.x + 2) {
        sinking[s] = clock;
        dirty = true;
      }
    }
    if (gc.x >= end && !Object.keys(sinking).length) {
      gc.phase = "unload";
      gc.marked = new Set();
      dirty = true;
    }
  } else if (gc.phase === "unload") {
    gc.x = calm ? C.unload : Math.min(C.unload, gc.x + 50 * dt);
    if (gc.x >= C.unload - 0.1) {
      world.free += gc.hopper;
      say(`${gc.hopper} plates returned to free stock; the memory stays with the process`);
      gc.freed = gc.hopper;
      gc.hopper = 0;
      pileEl.innerHTML = MV.pile(0);
      gc.phase = "return";
      collectorEl.removeAttribute("data-on");
      dirty = true;
    }
  } else if (gc.phase === "return") {
    gc.x = calm ? C.home : Math.max(C.home, gc.x - 60 * dt);
    if (gc.x <= C.home + 0.1) {
      gc.phase = "idle";
    }
  }
  collectorEl.setAttribute("transform", translateAlong(P, [gc.x, 0, 0]));
  const slot = gc.x > D.collect.outFrom ? slotOut : slotUnder;
  if (collectorEl.parentNode !== slot) slot.appendChild(collectorEl);
  for (const key of Object.keys(sinking)) {
    const s = Number(key);
    const t = calm ? 1 : (clock - sinking[s]) / 0.5;
    sinkEl[s].setAttribute("transform", translateAlong(P, [0, 0, -D.plateH * ease(t)]));
    if (t >= 1) {
      delete sinking[s];
      delete world.plates[s];
      sinkEl[s].removeAttribute("transform");
      gc.hopper += 1;
      pileEl.innerHTML = MV.pile(gc.hopper);
      dirty = true;
    }
  }
}

// ---------------------------------------------------------------- painting the model
function paint() {
  const now = reach(world, [world.token]);
  const live = reach(world, rootsNow());
  let dNow = "";
  let dOld = "";
  for (let s = 0; s < plateEl.length; s++) {
    const el = plateEl[s];
    const p = world.plates[s];
    const show = !!p && !landing[s];
    el.toggleAttribute("data-on", show);
    if (!p) continue;
    const fresh = born[s] !== undefined && clock - born[s] < 0.8;
    el.dataset.state = fresh ? "new" : now.has(s) ? "cur" : live.has(s) ? "held" : "old";
    el.toggleAttribute("data-mark", gc.marked.has(s));
    if (p.kids && show) for (const c of p.kids) {
      if (!world.plates[c] || landing[c]) continue;
      const a = iso([D.sites[s][0], D.sites[s][1], D.stoneTop], P);
      const b = iso([D.sites[c][0], D.sites[c][1], D.stoneTop], P);
      const seg = `M${a[0].toFixed(1)} ${a[1].toFixed(1)}L${b[0].toFixed(1)} ${b[1].toFixed(1)}`;
      if (now.has(s) && now.has(c)) dNow += seg;
      else dOld += seg;
    }
  }
  linksNow.setAttribute("d", dNow);
  linksOld.setAttribute("d", dOld);
  const free = Math.min(world.free, D.stock);
  stockEl.forEach((el, i) => el.toggleAttribute("data-on", i < free));
  const pendingCount = pending(world, rootsNow()).filter((s) => sinking[s] === undefined).length;
  const active = readers.filter((r, i) => i < readerCount).length;
  const text = `v${world.version} · ${active} reader${active === 1 ? "" : "s"} · ${Math.round(world.history[world.history.length - 1].shared * 100)}% shared · ${pendingCount} plate${pendingCount === 1 ? "" : "s"} pending`;
  if (readout.textContent !== text) readout.textContent = text;
  stage.setAttribute("aria-valuetext", text);
}

function drawFalls(calm) {
  const T = 0.42;
  const h0 = D.nozzle - D.stoneTop - D.plateH - 0.6;
  let out = "";
  for (let i = falling.length - 1; i >= 0; i--) {
    const f = falling[i];
    const t = calm ? 1 : (clock - f.t0) / T;
    if (t >= 1) {
      falling.splice(i, 1);
      delete landing[f.site];
      born[f.site] = clock;
      dirty = true;
      continue;
    }
    const [x, y] = D.sites[f.site];
    out += `<g transform="${translateAlong(P, [x, y, h0 * (1 - t * t)])}">${D.fallSvg}</g>`;
  }
  fallingEl.innerHTML = out;
}
function drawLatch(calm) {
  const t = calm ? 1 : (clock - latch.t0) / 0.6;
  let dy = 0;
  if (t < 1) dy = latch.ok ? -2.2 * ease(t / 0.4) : -0.9 * Math.sin(Math.PI * Math.min(1, t / 0.35));
  else dy = latch.ok && clock - latch.t0 < 1.4 ? -2.2 * (1 - ease((clock - latch.t0 - 0.9) / 0.5)) : 0;
  latchEl.setAttribute("transform", translateAlong(P, [0, dy, 0]));
  latchEl.toggleAttribute("data-on", t < 1.6 && latch.ok);
  const c = calm ? 1 : (clock - card.t0) / 0.7;
  const lift = c < 1 ? 7 * Math.sin(Math.PI * c) : 0;
  cardEl.setAttribute("transform", lift ? translateAlong(P, [0, 0, lift]) : "");
}

// ---------------------------------------------------------------- tour, input, loop
const TOUR = ["write", 2, "collect", 2, "contend", 2.5, "collect", 2, "multi", 2, "readers+", 1.5, "write", 2, "collect", 2.5, "readers-", 1];
let touring = !still.matches;
let tourAt = 0;
let tourWait = 1.4;
let idleTimer = 0;
const idle = () => !W1.busy && !W2.busy && W1.queue.length === 0 && W2.queue.length === 0;
function act(name) {
  if (name === "write") { if (!W1.busy) writeJob(W1, [rnd(LEAVES)]); }
  else if (name === "multi") { if (!W1.busy) { const a = rnd(LEAVES); writeJob(W1, [a, (a + 3) % LEAVES, (a + 5) % LEAVES]); say("One version for three changes: setAttributes"); } }
  else if (name === "contend") {
    if (W1.busy || W2.busy) return;
    writeJob(W1, [4 + rnd(2), 6 + rnd(2)], { slow: 0.25 });
    W2.busy = true;
    W2.queue.push({ fn: () => (W1.base >= 0 && W1.x > holeX + 40 ? 0 : false) });
    writeJob(W2, [rnd(4)]);
  } else if (name === "collect") startCollect();
  else if (name === "readers+") readerCount = Math.min(4, readerCount + 1);
  else if (name === "readers-") readerCount = Math.max(1, readerCount - 1);
  dirty = true;
}
function runTour(dt) {
  if (!touring) return;
  if (tourWait > 0) { tourWait -= dt; return; }
  if (!idle() || gc.phase !== "idle") return;
  const item = TOUR[tourAt % TOUR.length];
  tourAt += 1;
  if (typeof item === "number") tourWait = item;
  else act(item);
}
function takeOver() {
  touring = false;
  clearTimeout(idleTimer);
  idleTimer = setTimeout(() => { touring = !still.matches; tourWait = 0.5; run(); }, 9000);
}
stage.addEventListener("keydown", (event) => {
  const key = event.key;
  let name = null;
  if (key === "w" || key === "W" || key === "Enter" || key === " ") name = event.shiftKey ? "multi" : "write";
  else if (key === "c" || key === "C") name = "contend";
  else if (key === "g" || key === "G") name = "collect";
  else if (key === "+" || key === "=" || key === "ArrowUp" || key === "ArrowRight") name = "readers+";
  else if (key === "-" || key === "_" || key === "ArrowDown" || key === "ArrowLeft") name = "readers-";
  if (!name) return;
  event.preventDefault();
  takeOver();
  act(name);
  run();
});
svg.addEventListener("click", (event) => {
  takeOver();
  stage.focus({ preventScroll: true });
  if (event.target.closest('[data-part="readers"]')) act("readers+");
  else if (event.target.closest('[data-part="collector"]') || event.target.closest('[data-name^="bin"]') || event.target.closest('[data-name^="stock"]')) act("collect");
  else act("write");
  run();
});

let frame = 0;
let last = 0;
let visible = false;
function tick(now) {
  const real = last ? Math.min((now - last) / 1000, 1 / 30) : 1 / 60;
  last = now;
  const calm = still.matches;
  clock += real;
  let dt = real;
  if (paused > 0) {
    paused -= real;
    dt = 0;
  }
  runTour(real);
  if (dt > 0) {
    for (const w of writers) runWriter(w, dt);
    moveWriters(dt, calm);
    runReaders(calm ? 0 : dt, calm);
  }
  runCollector(dt, calm);
  drawFalls(calm);
  drawLatch(calm);
  if (dirty || Object.keys(born).some((s) => clock - born[s] < 1)) {
    dirty = false;
    paint();
  }
  frame = visible ? requestAnimationFrame(tick) : 0;
}
function run() {
  if (!frame && visible) {
    last = 0;
    frame = requestAnimationFrame(tick);
  }
}
new IntersectionObserver((entries) => {
  visible = entries[entries.length - 1].isIntersecting;
  if (visible) run();
}, { rootMargin: "120px 0px" }).observe(stage);
runReaders(0, true);
paint();
