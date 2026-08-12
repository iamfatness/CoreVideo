# CoreVideo Tiles — Design

**Date:** 2026-08-09
**Status:** Approved, pending implementation plan

## Summary

A new OBS source type, `CoreVideo Tiles`, that renders N assigned Zoom participants
as identical, evenly-spaced, crop-to-fill tiles on a shared presentation clock.
The operator drops one source into a scene and transforms it like any other; the
tile wall reflows internally as the assignment list changes.

The layout behaves like Zoom's gallery tiles. The synchronization does not — and
that difference is the point of the feature.

## Motivation

Two problems, one feature.

**Layout.** Arranging Zoom participants into an evenly-sized, evenly-spaced wall
is manual work in OBS today. The plugin has no scene-arrangement code at all; the
only layout machinery in the repo is `sidecar/src/layout-template.{h,cpp}`, which
lives in the optional sidecar (`COREVIDEO_BUILD_SIDECAR`), speaks to OBS over
obs-websocket, and uses edge-to-edge slots with no concept of a gutter —
`sidecar/data/templates/4-up-grid.json` is four 0.5×0.5 rects with zero spacing.

**Sync.** Gallery-style tile walls drift. Each feed free-runs at its own latency,
so tiles disagree with each other and with the mixed audio. CoreVideo's north star
is low-latency, high-quality A/V; a tile wall that breaks lip sync would undercut
the product's central claim.

The current code cannot sync a tile wall. `zoom-source.cpp:1089` stamps video
`frame.timestamp = os_gettime_ns()` and `:1231` stamps audio the same way —
wall-clock at arrival, not media time. With a single source the error is small,
because video and audio traverse comparable paths. With N tiles each feed carries
its own arrival latency and there is no common clock to align against.

### The unlock

`third_party/zoom-sdk/h/rawdata_def.h:27` and `:40` expose
`YUVRawDataI420::GetTimeStamp()` and `AudioRawData::GetTimeStamp()` — a real
per-participant media timestamp on both video and audio.

**Where that boundary actually is (corrected 2026-08-09 during Phase 0):** the
SDK raw-data callbacks live in the separate `ZoomObsEngine` process —
`engine/src/engine-video.cpp:175 onRawDataFrameReceived()` and
`engine/src/engine-audio.cpp` — not in the plugin. (`src/zoom-video-delegate.cpp`
and `src/zoom-audio-delegate.cpp` also contain SDK callbacks, but they are
orphaned dead code: no CMake target compiles them, left behind by the engine/SHM
split.) The engine discards `GetTimeStamp()` today, and the SHM headers that
carry frames to the plugin (`src/engine-ipc.h` `ShmFrameHeader`/`ShmAudioHeader`)
have no timestamp field — the media PTS currently dies at the engine boundary.

Recovering those timestamps makes per-tile sync achievable rather than
aspirational. It requires two things: reading `GetTimeStamp()` in the engine
(Phase 0 instruments exactly this), and — for Phase 2 — extending the SHM
frame/audio headers to carry the PTS across to the plugin, where the
presentation clock lives.

## Architecture

### Components

| Unit | Responsibility | Depends on |
|---|---|---|
| `zoom-tile-grid.{h,cpp}` | Pure solver: `(N, canvasAspect, tileAspect, gutter, margin) → QVector<Rect>` | nothing |
| `zoom-tile-sync.{h,cpp}` | Presentation clock; per-feed media-PTS → local-monotonic offset; adaptive `L` | nothing (takes samples) |
| `zoom-tile-buffer.{h,cpp}` | Bounded per-feed frame ring; `frameAt(T)`; hold-last + starvation flag | sync |
| `zoom-supersource.{h,cpp}` | OBS source registration, assignment list, output cadence, emits composite at `T` | all above |
| `hw-video-pipeline.cpp` (extend) | N-tile crop-to-fill blit into one surface on GPU | existing |
| `zoom-audio-router.cpp` (extend) | Per-participant delay line targeting the same `T` | sync |

The first two units are pure functions with no OBS or SDK dependency, which is
what makes the layout and timing logic testable without a meeting.

### Grid solver

For N tiles and a given canvas aspect, try every row count `r = 1..N` with
`cols = ceil(N / r)`. Compute the largest uniform tile that fits given gutter and
outer margin, and keep the `r` that maximizes tile area. A short last row is
centered.

This is what makes 5 participants render as 3-over-2 rather than 5-across, and it
is the property the operator actually asked for: identical rectangles at identical
spacing.

Tile aspect, gutter, and margin are parameters of the solver so they can be
exercised in tests, but v1 fixes them at 16:9, and gutter/margin at a fixed
fraction of canvas height. Exposing them as operator controls is out of scope
for v1.

### Presentation clock and data flow

```
Zoom SDK ──GetTimeStamp()──┐
  per-participant video ────┼─→ tile-buffer[i] ──┐
                            │                    ├─→ frameAt(T) ─→ GPU blit ─→ one frame @ T
  per-participant audio ────┴─→ delay line ──────┘                              ↓
                                    ↑                                      OBS source
                              tile-sync: T = now − L
```

The compositor runs on the OBS video tick, at the configured canvas frame rate —
it is not driven by feed arrival. For output PTS `T = now − L`, each tile takes
its newest frame with `pts ≤ T`. One coherent timestamp covers the whole wall, so
every tile carries identical latency instead of free-running.

Before a feed has delivered any frame with `pts ≤ T` — at startup, or when a
participant is newly assigned — its tile renders as a no-video name tile rather
than black, and joins the wall on its first qualifying frame.

Each participant's audio passes through a delay line targeting the same `T`.
Participant audio is already routed separately
(`zoom-participant-audio-source.cpp`), so each person's audio locks to their own
tile. This per-tile lip sync is precisely what gallery view cannot do.

**One `T` drives video selection and audio delay together.** That is the single
idea the design rests on.

### Sync budget

`L = clamp(p99 jitter across active tiles, 40ms, 250ms)`, adaptive.

A starved feed holds its last frame rather than stalling the wall, so one
participant's bad connection never adds latency to everyone else. A recovered feed
rejoins at the next `T` with no re-sync bump. Starvation is surfaced in the dock,
never on the program output.

### Tile fill

Tiles are a fixed aspect (16:9). Feeds are scaled to cover and center-cropped, so
every tile is an identical filled rectangle. This matches Zoom's default and is
what makes a wall read as one graphic rather than a collage. Off-aspect feeds
(portrait phone, screenshare) lose their edges; that is the accepted tradeoff.

### Slot filling

Participants are assigned explicitly, using the same model as output assignment
(`zoom-output-manager` / `zoom-output-dialog`). Order is list order, drag to
reorder. Reflow fires when the list changes or a participant drops video.

Explicit assignment keeps the subscription count visible, which matters directly
given the concurrent-stream cap (see Risks).

## Scope

**In:** grid solver; crop-to-fill; adaptive `L` with hold-last; assignment list
with reorder; no-video participants render as a name tile; subscription count
surfaced against the concurrent-stream cap.

**Out, deliberately:** pagination; speaker-driven feature slot; per-tile fit
override; name banners; borders. Borders are excluded because they violate the
existing program/preview no-borders rule. Reflow snaps; no animation.

## Testing

**Grid solver** — golden tests for N = 1..16 asserting the three properties
directly: all rects equal area, all gaps equal, short last row centered.

**Sync** — unit tests over synthetic PTS streams, including starvation, recovery,
and clock skew.

**Rig acceptance** — extend the A/V clap validator (built 2026-08-06) to report
*per-tile* offset rather than a single pair. That number is the evidence the
feature beats gallery view, and no claim of completion should be made without it.

## Phasing

**Phase 0 — spike, and it gates everything else.** Instrument `GetTimeStamp()`
across two or more simultaneous participants and determine whether the values
share a timebase. If they are receiver-normalized, this design holds as written.
If they are sender-clock, cross-tile alignment requires per-feed offset estimation
and Phase 2 roughly doubles in size. Half a day here beats discovering it after
the compositor exists.

**Phase 1** — grid solver plus rendering with naive latest-frame selection. Proves
layout and ships something visible.

**Phase 2** — sync: buffer, adaptive `L`, audio delay lines, clap validation.

**Phase 3** — UI polish and no-video tiles.

## Risks

**SDK timestamp semantics.** `third_party/zoom-sdk/h/rawdata_def.h` is a
*reconstructed* ABI header — its own comment notes the real definitions are "not
distributed publicly" and that these interfaces "match the SDK ABI."
`GetTimeStamp()` is present in the vtable, but its epoch, units, and cross-participant
timebase are unverified. This is what Phase 0 exists to answer.

**SHM header extension.** Phase 2 must add a PTS field to `ShmFrameHeader` and
`ShmAudioHeader` (`src/engine-ipc.h`) so media time survives the engine→plugin
hop. That is an IPC ABI change between the engine and plugin: both sides ship
together, and version/generation handling follows the existing SHM
compatibility rules (see the 2026-08-08 SHM resize incident).

**Concurrent-stream cap.** The feature's entire value is N simultaneous
subscriptions, and on 2026-08-09 only 1 of 6 feeds survived. The
1080p-uniform-subscribe plus compositor-scale work is not adjacent to this
feature — it is underneath it, and Tiles cannot ship without it.

**4K composite cost.** Compositing N feeds into one surface at 4K must stay on the
GPU path. A CPU fallback at high tile counts would be a latency regression against
the north star.
