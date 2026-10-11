// Parts whose shape changes as they move: the reader heads (their arms reach diagonally from
// the rail to the plate they read) and the collector's hopper load. Shared by the build and the
// page, so the first frame and every later frame come from the same code.
export function makeMovers(k, P, D) {
  const S = (paths, style) => k.solidSvg(paths, style);
  const R = D.reader;

  // One reader head: a carriage on the front rail, a post, an arm at its own height and a lens
  // over the plate it reads. `cx` is the carriage, `[lx, ly]` the lens, `lit` while reading.
  function reader(index, cx, lx, ly, lit) {
    const zArm = R.arm0 + R.armStep * index;
    const out = [];
    out.push(S(k.slabOf({ x: cx - 4.5, y: R.y0, w: 9, d: R.d, r: 1 }, R.z0, R.h, P, 3, 0.4), { tone: "mid" }));
    out.push(k.dotsSvg([k.iso([cx - 2.6, R.y0 + R.d - 1.4, R.z0 + R.h], P), k.iso([cx + 2.6, R.y0 + R.d - 1.4, R.z0 + R.h], P)], { size: 0.4, tone: "lo" }));
    const px = cx;
    const py = R.post;
    out.push(S(k.cylinder(px, py, 1.3, R.z0 + R.h, zArm - R.z0 - R.h, P, 16), { tone: "mid", crease: "none" }));
    out.push(S(k.cylinder(px, py, 2, zArm, 2.4, P, 20), { tone: "mid" }));
    const dx = lx - px;
    const dy = ly - py;
    const len = Math.hypot(dx, dy) || 1;
    const nx = (-dy / len) * 1.1;
    const ny = (dx / len) * 1.1;
    const ring = [[px + nx, py + ny], [lx + nx, ly + ny], [lx - nx, ly - ny], [px - nx, py - ny]];
    if (len > 2.5) out.push(S(k.extrude(ring, zArm + 0.4, 1.6, P), { tone: "mid", crease: "none" }));
    out.push(S(k.cylinder(lx, ly, 2.5, zArm - 3.4, 5.2, P, 24, 0.3), { tone: "hi", lit }));
    out.push(k.lineSvg(k.ring(lx, ly, 1.3, zArm + 1.8, P, 16), { tone: lit ? "lit" : "lo" }));
    return out.join("");
  }

  // The pile of swept plates inside the hopper, seen through its open top.
  function pile(count) {
    const H = D.hopper;
    const z = H.floor + Math.min(count, H.max) * H.step;
    const open = { x: H.x + 1.4, y: H.y + 1.4, w: H.w - 2.8, d: H.d - 2.8, r: 0.6 };
    const a = [open.x, open.y];
    const b = [open.x + open.w, open.y];
    const c = [open.x, open.y + open.d];
    const at = (x, y, zz) => k.iso([x, y, zz], P);
    const quad = (pts) => k.pathOf(pts, true);
    const back = quad([at(a[0], a[1], H.top), at(b[0], b[1], H.top), at(b[0], b[1], z), at(a[0], a[1], z)]);
    const left = quad([at(a[0], a[1], H.top), at(c[0], c[1], H.top), at(c[0], c[1], z), at(a[0], a[1], z)]);
    const floor = quad([at(open.x, open.y, z), at(open.x + open.w, open.y, z), at(open.x + open.w, open.y + open.d, z), at(open.x, open.y + open.d, z)]);
    return `<path class="iso-shade" data-shade="1" d="${back}"/><path class="iso-shade" data-shade="0" d="${left}"/><path class="${count ? "iso-top" : "mp-void"}" d="${floor}"/><path class="iso-line" data-tone="lo" d="${quad([at(open.x, open.y, H.top), at(open.x + open.w, open.y, H.top), at(open.x + open.w, open.y + open.d, H.top), at(open.x, open.y + open.d, H.top)])}"/>`;
  }

  return { reader, pile };
}
