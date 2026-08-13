# CoreVideo Tiles — layout animation design

Status: approved design, not yet planned or implemented.
Date: 2026-08-12.

## Goal

When a participant joins or leaves, the Tiles wall currently changes layout on a
single frame: every tile jumps to a new position and size. This design makes
that change a continuous motion, to a standard that holds up on a live program.

The bar is not "it animates". The bar is that an operator who turns it on cannot
later point at a stutter, a colour shift, a stale face, or a wall that is behind
reality and blame this feature.

## What was decided, and why

Five decisions were settled before any design work. They are recorded with their
reasoning because each one closed off alternatives that look reasonable in
isolation.

### 1. Full reflow — every tile animates, not just the one that changed

A departure is not one tile vanishing. Five tiles to four is a 3x2 grid becoming
2x2, so every remaining tile moves and resizes regardless. Animating only the
arriving or leaving tile would leave the most visible part of the change — the
other tiles jumping — as a hard cut, which is precisely what the eye follows.

### 2. Pixel-exact at rest, sub-pixel only while moving

`snap_tile_grid_even()` places every tile edge on an even pixel because I420
chroma is subsampled 2x2, so a blit edge on an odd pixel has no valid chroma
sample. Animating through that constraint directly gives 2-pixel quantised
motion, which judders worst on exactly the slow, gentle reflows this feature
exists to produce.

Therefore: at rest, tiles draw exactly as they do today — same snapped blit, same
shader, byte-identical output. While moving, a tile is composited through an
intermediate texture of even dimensions, which can then be drawn at any
fractional position with linear filtering. Chroma reconstruction stays in tile
space, so sub-pixel placement is safe.

The still image, which is what is on air almost all of the time, is unchanged by
construction. The additional cost exists only during a transition.

### 3. A leaving tile holds its last frame, bounded to genuine departures

The natural look, and what comparable products do. It requires a bounded
exception to the rule `src/zoom-tile-slot.h` exists to enforce — that a stored
frame stops being shown the instant a slot is repointed, written after the wrong
face reached air. The exception is scoped by four invariants in "Exit
invariants" below.

### 4. Retarget in flight, behind a settle window

Changes that arrive mid-transition re-aim the tiles from wherever they currently
are; they never queue and never restart. Before any transition begins, a roster
change must hold for a settle window, which absorbs blips — the roster is known
to flicker, which is why `SpeakerDirector` carries a 60-second absence grace.
Without this, a participant who drops out and back for 120 ms would produce a
full exit animation followed by a full entry, which is more visible on air than
today's single-frame pop.

### 5. Off by default, with a duration control

An on/off toggle plus a duration in milliseconds. It defaults to off, so
upgrading changes nothing about any existing show and every scene file built
before this feature behaves exactly as it did. Because of decision 2, "off" is
not a cosmetic setting: with the toggle off the animator never runs and the
render path is today's path.

## Architecture

Three units, each with one responsibility.

### `src/zoom-tile-animator.h` (new, pure)

Owns all animation state and decisions. No dependency on OBS, the graphics API,
or the Zoom engine — the same idiom as `zoom-tile-grid.h`, `zoom-tile-border.h`
and `speaker-settings-merge.h`, and for the same reason: every hard part of this
feature is timing logic, and timing logic that cannot be tested is timing logic
that fails on air.

State is a map keyed by **participant id** — not by index. This is the
foundational change. Today's layout is purely positional: tiles are placed by
their offset in the feed list, so "the same participant moved from here to
there" is not currently expressible. Position by index is also why a departure
shifts everyone after it.

Per tile it holds: current rect, current velocity, target rect, phase, and phase
start time.

Entry point:

```
advance(now_ns, desired_layout, settings) -> vector<AnimatedTile>
```

where `desired_layout` is the solver's ideal geometry keyed by participant, and
each `AnimatedTile` carries a rect in doubles, an alpha, and whether it is at
rest. "At rest" is what the renderer uses to choose between the snapped path and
the texture path.

### `src/zoom-tile-grid.h` (unchanged)

Still solves the ideal layout from a count. The animator consumes its output as
a target rather than as the thing drawn. The parity-verified geometry and the
even-snapping rule are untouched.

### `tiles_source_render()` in `src/zoom-supersource.cpp` (thin change)

Where it currently solves and draws in one step, it will solve the target, hand
it to the animator, and draw what comes back: the existing snapped blit for
tiles at rest, the intermediate-texture path for tiles in motion.

Unaffected and explicitly out of scope: the audio-group and ISO planning (which
key off the roster, not off animation), per-tile crop, the background source,
and the glow pass ordering. The glow quad follows the animated rect.

## Motion model

Elapsed-nanosecond based, from `os_gettime_ns()`. Never per-frame increments, so
motion is identical at 30 and 60 fps and unaffected by a dropped frame.

Each tile is driven by a critically damped spring over position, carrying
velocity as state, with the duration setting as its settle time.

The spring is chosen specifically to solve retargeting. When a change lands
mid-flight, the spring re-aims from the current position **at the current
velocity**, so motion stays continuous. Restarting a cubic ease from the current
position cannot do this: an ease begins at zero velocity, so a tile moving at
speed would stop dead and re-accelerate — a visible hitch precisely when the
wall is busiest.

## Lifecycle

Phases: `Entering` (alpha 0 to 1 at its final slot), `Present`, `Exiting` (alpha
1 to 0). Position springs run in every phase, so an entering tile still
participates in reflow.

Entering tiles fade in at their final position rather than flying in, so tiles
never cross one another.

### Settle window

The settle window is **250 ms**, fixed in code and not exposed as a setting. It
is a correctness mechanism rather than a taste one: exposing it would let an
operator set it to zero and reintroduce blip-driven motion.

A roster change starts a timer rather than a transition. If the roster reverts
before it expires, the timer is cancelled and nothing moves. If it holds, the
transition begins against the roster as it stands at that moment — so a burst of
joins coalesces into one reflow rather than several.

Cost: the wall reacts to a real change one settle window later than it does
today. This is deliberate.

### Exit invariants

These four bound the exception to `zoom-tile-slot.h`'s rule, and should be
reviewed as safety-critical rather than presentational:

1. An exit animation may begin **only** on a genuine roster departure.
2. A slot reassignment cuts instantly — no hold, no fade — exactly as today.
3. Any repoint of a slot cancels a running exit immediately.
4. An exit can never outlive its configured duration.

## Settings

Two properties on the Tiles source:

| Property | Default | Behaviour |
| --- | --- | --- |
| Animate layout changes | **off** | When off, the animator is bypassed entirely and the render path is today's. |
| Duration | 350 ms | Spring settle time. Also bounds the exit animation. |

Both follow the existing property idiom in `zoom-supersource.cpp`, with strings
in `data/locale/en-US.ini`.

## Testing

Pure, no OBS and no GPU required — the same harness as the existing 40 tests:

- **Parity when disabled.** With the toggle off, the emitted rects equal
  `snap_tile_grid_even(solve_tile_grid(...))` exactly. This is the guarantee
  that existing shows are untouched, and it is proven rather than asserted.
- **Velocity continuity on retarget.** A tile retargeted mid-flight must not
  drop to zero velocity.
- **A blip never moves the wall.** A departure reverted inside the settle window
  produces no motion at all.
- **Burst coalescing.** Several changes inside one settle window produce one
  transition.
- **Exit time-box.** An exit never exceeds the duration.
- **Reassignment never fades.** Repointing a slot produces an instant change and
  cancels any running exit.
- **Frame-rate independence.** The same elapsed time produces the same geometry
  whether advanced in 16 ms or 33 ms steps.

Not unit-testable, and therefore rig work with explicit acceptance criteria:

- **Colour parity between the two render paths** (see Risks).
- Transition cost on the real GPU at eight 1080p feeds.

## Risks

1. **Colour parity between the still and moving paths.** A tile composited
   directly at rest and through an intermediate texture while moving must be
   colour-identical, or tiles will visibly shift as motion starts and stops —
   which would be worse than no animation at all. This must be measured, not
   eyeballed; the luma-probe idiom already in the codebase is the tool.
2. **Transition cost.** Up to eight intermediate 1080p render targets for a few
   hundred milliseconds. Must be measured on the real box.
3. **Proximity to the slot invariants.** The exit path is the only part of this
   feature that touches safety-critical code. It is bounded by four invariants
   above and should be reviewed as such.

## Out of scope

Animating speaker-change cuts, tile-shape or spacing changes, and any transition
on the Active Speaker source. This design covers participants joining and
leaving the wall, and the reflow that follows.
