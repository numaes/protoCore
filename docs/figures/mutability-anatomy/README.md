# Mutability anatomy

An interactive isometric figure that explains how protoCore concentrates mutability in one
place, drawn as a printer's type-case cabinet on a workbench.

- **Plates on the composing stone** are immutable cells. A page version is a tree of 15 plates;
  a write casts only the plates on the path from the changed leaf to the root and shares the rest.
- **The type case** is the mutable-shard table: 32 pigeonholes (shards 0-31) open on top, seven
  closed cases below for the other 224 of the 256 shards. Each card stands for a shard root.
- **Two writer arms** cast plates and swap the card through a latch (compare-and-swap). When the
  other writer published first, the latch refuses and the writer re-applies its change.
- **Reader heads** read whatever version they took from the card, with no lock.
- **The collector carriage** under the stone runs after a short pause that snapshots the roots:
  it marks reachable plates, sinks the rest into its hopper and returns them to the stock bin.

The readout (version, readers, share of plates reused, plates pending) comes from `model.mjs`,
the same pure model that drives the motion. Numbers in the caption come from
`docs/GarbageCollector.md`, `docs/MemoryModel.md`, `docs/GLOBAL_MUTABLE_TABLE.md` and the
2.11.0 `setAttributes` entry in `CHANGELOG.md`.

## Build

Node 18 or later, no dependencies.

```sh
node build.mjs            # writes mutability-anatomy.html (dark theme)
node build.mjs --light    # writes mutability-anatomy-light.html
node build.mjs --audit    # also proves the geometry; must end "audit passed"
```

Each page is standalone: open it in a browser. Keys: `W` write one change, `Shift+W` write three
changes as one version, `C` two writers race, `G` collect, `+`/`-` (or arrows) add or remove
readers. A click writes; an idle tour runs when nobody interacts.

## Files

| File | Role |
|---|---|
| `model.mjs` | The truth: tree, path copy, compare-and-swap, reachability. Inlined into the page. |
| `build.mjs` | Geometry, painter's order (settled by `kit/audit.mjs`), card, caption, page. |
| `movers.mjs` | Parts rebuilt per frame (reader arms, hopper load), shared by build and page. |
| `live.js` | Writers, readers, collector, tour and input. |
| `kit/` | The isometric kit from the anatomy skill (MIT, see `kit/LICENSE`). |
| `shots/` | Captures used to review the figure. |

## Verification

The figure was checked with the anatomy skill's tools: `node build.mjs --audit` (audit passed;
the case walls in front of each card are excused because every card is clipped to its own
pigeonhole), `scripts/lines.mjs` at rest and during a write, a sweep and a race in both themes
(0 floating line ends), and desktop, 390 px phone and 4x captures in both themes.

Author: Gustavo Marino.
