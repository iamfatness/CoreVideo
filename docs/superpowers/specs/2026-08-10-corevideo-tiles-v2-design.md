# CoreVideo Tiles v2 — GPU compositor, background, borders, per-slot crop

**Date:** 2026-08-10
**Status:** Approved, ready for implementation planning (Phase A first)
**Builds on:** `2026-08-09-corevideo-tiles-design.md` (the wall) and
`2026-08-10-corevideo-tiles-participant-picker-design.md` (casting)

## Problem

Tiles renders a grid of live participants and nothing else: no background, no
styling, no per-feed framing control. The operator asked for three things — a
background (video, image, or colour), tile borders with a colour and a
square/rounded shape, and a left/right crop per feed.

Two of those cannot be built well on the current architecture. The source is
`OBS_SOURCE_ASYNC_VIDEO`: a worker thread blits I420 on the CPU and pushes
finished frames through `obs_source_output_video`. On that path:

- An async source cannot sample other OBS sources, so **background video** would
  mean writing a decoder, a playback clock, seek and loop handling inside the
  plugin — reimplementing what OBS's Media source already does.
- I420 stores chroma at half resolution, so **rounded corners** get colour
  fringing along every diagonal edge. Against vMix and Ecamm that reads as
  amateur.

The original Tiles spec already flagged the underlying issue: *"Compositing N
feeds into one surface at 4K must stay on the GPU path. A CPU fallback at high
tile counts would be a latency regression against the north star."* This feature
request is the forcing function for that work.

## Decisions

| Question | Decision |
|---|---|
| CPU or GPU compositor? | **Convert to GPU first**, then build features on it. |
| Background source | **Colour picker + a dropdown of existing OBS sources.** No in-plugin decoding. |
| Crop scope | **Per tile slot** — persists across meetings; see the Auto-mode caveat below. |
| Border shape | **Square / Rounded** dropdown, radius slider revealed for Rounded. |
| Phasing | **Two separate deliveries.** Phase A reaches visual parity with today; Phase B adds the features. |

**Why phasing is not optional.** If the GPU conversion and the new features land
together and the wall looks wrong, a rendering regression is indistinguishable
from a border bug. Phase A must be verified as *the same wall, drawn
differently* before anything is stacked on it.

**Accepted cost of per-slot crop.** A crop belongs to a position, not a person.
In Manual mode that is exactly right — the operator cast that slot deliberately.
In Auto mode the wall reflows whenever somebody joins, leaves, or kills their
camera, so a slot's crop lands on whoever shifts into that position. The
operator sets a crop for one face and may see it applied to another. This was
chosen over per-participant crop because Zoom regenerates user IDs every
meeting, which would make per-person crops session-scoped and force the operator
to re-set them every show.

**Border rule override.** The Phase 0/1 plan states "No borders on tiles",
citing the project's program/preview no-borders rule. That rule governs
program/preview outlines in the application UI; decorative borders inside a
composited wall are a different thing. The owner has explicitly asked for
borders here, and this spec supersedes that constraint for the Tiles source
only. The no-borders rule for program and preview is unchanged.

---

# Phase A — GPU compositor at parity

**Deliverable: the wall looks exactly as it does today, drawn on the GPU.** No
new operator-visible features.

## Architecture

The source's `output_flags` change from
`OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE` to
`OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE`, and
the source implements `video_render` instead of pushing frames.

**The front half is unchanged.** The engine reader thread still copies I420 out
of shared memory into each feed's staging buffer under `feed->mtx`, exactly as
today. `tile_take_snapshot`'s existing O(1) buffer swap is already the right
handoff — it hands the reader a buffer back to refill so neither side allocates
after warm-up — and it is reused as-is.

**The back half changes.** The compositor worker thread and its canvas buffer go
away. `video_render` (graphics thread) instead:

1. For each feed, takes the snapshot (existing O(1) swap, brief lock).
2. Uploads the Y, U and V planes to per-feed GPU textures **only when that
   feed's generation changed** — an idle wall re-uploads nothing.
3. Solves the grid with the existing `solve_tile_grid` (unchanged) and draws one
   quad per tile through a YUV→RGB effect.

Uploads happen outside `feed->mtx`; the lock covers the buffer swap only, never
a graphics call.

## Parity requirements

These are the things that will silently look wrong if got wrong, so Phase A is
not done until each is confirmed:

- **Colour handling.** The current path sets range and space to match
  `zoom-source.cpp:416-423`. The shader conversion must produce the same result;
  getting it wrong shows as washed-out or crushed faces, which is easy to miss
  on a webcam image and obvious on a broadcast.
- **Geometry.** `solve_tile_grid`, `solve_cover_crop`, the even-snapping, and
  the short-row centering are unchanged and stay under their existing tests.
  Phase A changes how tiles are drawn, not where they land.
- **Neutral tile.** A slot with no current frame still renders the same neutral
  fill.
- **Lifetime.** The worker thread's removal must not leave the teardown path
  (`tiles_source_destroy`, the gate, `execute_feed_plan`) racing; textures are
  GPU resources and must be destroyed on the graphics thread.

## Verification

Screenshot the same wall — same participants, same canvas, same tile count —
before and after, and compare side by side. Record both in
`docs/design-reference/`. A build that compiles and renders *something* is not
parity.

---

# Phase B — background, borders, crop

## Background

Two properties: a colour, and a dropdown listing sources in the current scene
collection. The colour fills the canvas; if a source is selected it is rendered
over the colour, beneath the tiles. Selecting nothing leaves the colour alone,
which also preserves today's plain-background behaviour.

Rendering another OBS source is `obs_source_video_render` on a weak reference,
so the Tiles source never keeps a deleted background alive.

**Two hazards this design must handle explicitly:**

1. **Recursion.** Selecting the Tiles source itself — or a scene that contains
   it — is an infinite render cycle. OBS's active-child registration reports
   this; the selection must be refused and logged, not attempted. An unguarded
   cycle is a stack overflow, i.e. OBS disappears mid-show.
2. **Inactive media.** A Media or Browser source that is not in any active scene
   does not play. Rendering it without marking it active yields a frozen first
   frame, which presents to the operator as "my background video is broken".
   The background source must be held active/showing for as long as Tiles
   references it, and released when the reference changes or the source is
   destroyed.

## Borders

Global for the wall, not per tile: width, colour, and a Shape dropdown of Square
or Rounded, with a corner-radius slider shown only for Rounded.

Width and radius are both in **pixels of the configured canvas, and do not
auto-scale with it** — a 6 px border is 6 px whether the canvas is 1080p or 4K.
Note this deliberately differs from the gutter, which scales as
`canvas_height / 135`. The gutter scales because it is structural spacing that
should stay proportional; a border is a drawn line whose weight the operator is
choosing directly, and a border that silently thickened when the canvas changed
would be the more surprising behaviour.

The border insets **inside** the tile rect, so grid geometry and the tested
solver are untouched — a bordered wall has tiles in exactly the same places as
an unbordered one.

Rounded corners mask the **video**, not just the stroke: the background shows
through the corner. Rounding only the stroke would leave square video peeking
out behind a rounded outline. Anti-aliasing comes from a distance-field mask in
the shader, which is the specific reason this feature belongs on the GPU rather
than in an I420 blit.

## Per-slot crop

Each tile slot gets a left crop and a right crop — two independent values, each
a percentage of source width, so they stay correct regardless of what resolution
that participant is sending. Composition order is fixed and matters:

1. Apply the slot's left/right crop to the source rectangle.
2. Apply the existing `solve_cover_crop` to what remains, to fill the tile
   aspect without letterboxing.

So cropping narrows the usable source, and the cover-crop then centres within
it. This ordering is pure geometry and belongs in the tested header alongside
`solve_cover_crop`, because it is the part most likely to be subtly wrong and
the least likely to look obviously wrong on a rig.

Crops of 0 must be exactly today's behaviour, so an operator who never touches
the controls sees no change.

## Error handling

- Background source deleted while selected → weak reference fails to resolve →
  fall back to the colour. Not an error state; log once.
- Background source selection that would recurse → refused at selection time
  with a clear log line naming the source.
- Crop values that leave zero usable width → clamp so at least a minimal strip
  remains, rather than producing a zero-width sample rect.
- Border width larger than half the tile → clamp, so a tile never inverts.

## Testing

Pure and unit-tested: the crop composition order, crop clamping, and the border
inset geometry, all in the existing plain-`main()` pattern.

Not unit-testable, and honest about it: the shader itself, colour conversion
fidelity, and rounded-corner anti-aliasing. These need the rig plus screenshot
comparison.

## Out of scope

Drop shadows, per-tile border overrides, background scaling or fit modes beyond
stretch-to-canvas, name-tile text, top/bottom crop, and Phase 2 lip sync.

## Risk

This work does not touch the concurrent-stream cap. On 2026-08-09 only 1 of 6
subscribed feeds survived. A prettier wall that can still sustain only one live
feed is still blocked on that, and it remains the real gate on shipping Tiles.
