# CoreVideo Tiles — Gallery Styling

**Date:** 2026-08-11
**Status:** Designed from the owner's reference image; authorised to build under
"make a list of all known issues and features and work them while I am gone"
**Builds on:** `2026-08-10-corevideo-tiles-v2-design.md`

## Problem

The owner supplied a reference gallery wall and asked for "boxes like this". Three
things separate it from what Tiles renders today:

1. **Tile shape.** The reference measures ~1.27:1 — 5:4 or 4:3. Tiles is
   hard-coded to 16:9 (`kTileAspect`), so faces read as letterbox-wide beside it.
2. **Spacing.** The reference has generous gaps. Tiles uses
   `canvas_height / 135` — **8 px at 1080p** — which is not an operator control.
3. **Outer glow.** Each tile in the reference sits on a soft halo bleeding into
   the background. Tiles has a border stroke on the edge and nothing beyond it.

The first two are parameters the grid solver already accepts and that are simply
fed constants. The third is new shader work.

## Decisions

| Question | Decision |
|---|---|
| Tile shape | Preset dropdown (16:9, 4:3, 5:4, 1:1, 3:4, 9:16) **plus Custom** revealing a numeric ratio |
| Spacing | `Gutter` and `Margin`, each a percentage of canvas height, defaulting to today's exact value |
| Glow | Colour, size, intensity — drawn as a **separate pass before the tiles** |
| Glow overlap | **Allowed, not clamped.** See below. |

**Why the glow is a separate pass, not part of the tile draw.** Two reasons, both
load-bearing. A tile is drawn as a quad exactly its own size, so there is no
canvas outside it to bleed into — a glow needs a larger quad. And the tile pixel
shader carries the parity-verified BT.709 conversion and the `crop_uv` correction
that keeps borders registered against a cropped source; that path has been
verified twice and is not worth disturbing for an effect that can live beside it.

**Why glow overlap is not clamped.** A glow wider than half the gutter makes
neighbouring halos merge into a continuous wash, and one wider than the margin
clips at the canvas edge. Both are legitimate at small sizes and obviously wrong
at large ones. Clamping would silently contradict the number the operator typed;
this is a judgement better made by eye than by rule.

## Architecture

| File | Responsibility |
|---|---|
| `src/zoom-tile-shape.h` (new) | Pure: preset → aspect ratio; spacing percentages → pixels |
| `tests/tile-shape-test.cpp` (new) | Tests for both resolvers |
| `src/zoom-tile-glow.h` (new) | Pure: tile rect + glow size → the expanded quad to draw |
| `tests/tile-glow-test.cpp` (new) | Glow quad geometry, including canvas clamping |
| `data/effects/corevideo-tiles.effect` (modify) | A `Glow` technique |
| `src/zoom-tiles-effect.h` / `.cpp` (modify) | Resolve the glow technique and its uniforms |
| `src/zoom-supersource.cpp` (modify) | Properties, settings, draw order |
| `data/locale/en-US.ini` (modify) | Strings |

## Tile shape

`TileGridParams::tile_aspect` already exists and is under test. The hazard is that
the same constant is consumed in **three** places — the grid solve, the cover-crop
in the draw path, and `solve_slot_crop` for the per-slot crop. All three must take
the operator's value or the layout and the sampling disagree and every tile is
subtly mis-framed.

Consequence to state rather than hide: a 4:3 tile fed by a 16:9 camera crops more
off the sides, because the cover-crop must still fill the tile. That is inherent
to filling, not a defect.

## Spacing

`Gutter` and `Margin` become percentages of canvas height. The current hard-coded
value is `1/135 ≈ 0.741%`; both default to that, so an untouched scene renders
identically. Percentages rather than pixels because spacing is structural and
should scale with the canvas — deliberately unlike border width and corner
radius, which are absolute pixels because they are a drawn line whose weight the
operator is choosing directly.

## Outer glow

Drawn per tile, before the tiles, on a quad expanded by the glow size on all four
sides and clamped to the canvas. In that quad the shader evaluates the existing
`rounded_rect_sd` against the *inner* tile rect: distance ≤ 0 is inside the tile,
and from 0 outward the alpha falls off over the glow size. Colour and intensity
are operator controls.

Draw order becomes: background colour → background source → **glow** → tiles.

The glow quad's geometry is pure and belongs in a tested unit — in particular the
clamping, because a tile against the canvas edge produces an expanded rect that
would otherwise run off it.

## Error handling

- Glow size 0 → the pass is skipped entirely; zero cost and byte-identical output.
- Glow quad clamped to the canvas; a degenerate quad (zero width or height after
  clamping) is skipped rather than drawn.
- Custom aspect ≤ 0 → fall back to 16:9 rather than dividing by zero.
- Gutter or margin large enough to leave no room for tiles → the solver already
  returns an empty layout, which renders the background alone. Not an error.

## Testing

Pure and tested: preset → ratio, spacing percentage → pixels, glow quad expansion
and clamping. Not unit-testable and honest about it: the shader falloff and the
visual result, which need the headless GPU probe approach that worked for borders
and crop, plus a rig pass.

## Out of scope

Inner glow or shadow, per-tile glow overrides, glow on the background source,
drop shadows, and non-uniform gutters. Tile aspect remains uniform across the
wall — per-tile aspect is not in this design.
