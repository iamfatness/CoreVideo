# CoreVideo Tiles — CPU compositor parity baseline (Phase A, Task 1)

**Date captured:** 2026-08-10 (approx. 18:06–18:14 local time, in the span of ~8 minutes)
**Plugin build:** repo HEAD `e7729a6e5bdb678f599e231938cb5591a62e58b8` — "docs(tiles): implementation plan for the GPU compositor (Phase A)" (branch `feat/tiles-on-main`)
**Installed binary under test:** `C:\Program Files\obs-studio\obs-plugins\64bit\obs-zoom-plugin.dll`, on-disk `LastWriteTime` 2026-08-10 17:10:09 (i.e. built ~54 minutes before the HEAD docs commit above; no embedded `FileVersion`/`ProductVersion` resource was present to cross-check against a specific commit, so the HEAD SHA above is the best available proxy for "the build this DLL came from," not a verified exact match). OBS 32.2.1, obs-websocket 5.7.4, Windows 11 25H2.

These are the **"before" images** for the Phase A GPU-compositor conversion: everything here was captured through `obs-websocket`'s `GetSourceScreenshot` against the **CPU compositor**, before any GPU-path code changed. Task 6 (post-conversion) must reproduce this pixel geometry and (where available) color output against the GPU path. There is no way to regenerate the CPU-path renderer once the conversion lands, so these files are irreplaceable.

Canvas / video settings (`GetVideoSettings`): **1920×1080 base and output, 60/1 fps.**

## Sources captured

Two `corevideo_tiles_source` instances exist in the running OBS instance, both in "Auto" assignment mode:

| Source | Scene | `max_tiles` setting | Tiles rendered |
|---|---|---|---|
| `CoreVideo Tiles` | Dynamic Gallary | 5 | 5 |
| `CoreVideo Tiles (verify)` | 8 Sources | 8 | 8 |

Both sources occupy the full 1920×1080 canvas in their scene (`positionX/Y: 0,0`, `scaleX/Y: 1`, no bounds/crop) per `GetSceneItemList`.

## Images

| File | Dimensions | Source | What it shows |
|---|---|---|---|
| `before-corevideo-tiles.png` | 1920×1080 PNG, RGBA | `CoreVideo Tiles` (max 5) | 5 populated tiles in a 3-over-2 centered grid on a mid-gray background. See "Important discrepancy" below — this is **not** empty/neutral. |
| `before-corevideo-tiles-verify-.png` | 1920×1080 PNG, RGBA | `CoreVideo Tiles (verify)` (max 8) | 8 populated tiles in a 3-3-2 centered grid on the same mid-gray background. |
| `before-live-5.png` | 1920×1080 PNG, RGBA | `CoreVideo Tiles` (max 5) | Second capture of the same 5-tile source, taken ~8 minutes after the first, to record moving content explicitly as the "live" baseline (see below). |
| `before-live-8.png` | 1920×1080 PNG, RGBA | `CoreVideo Tiles (verify)` (max 8) | Second capture of the same 8-tile source, taken ~8 minutes after the first, same purpose. |

All four PNGs were verified with `file` to be exactly 1920×1080, 8-bit RGBA, non-interlaced, and have four distinct SHA-256 hashes (no accidental duplicate writes).

### Grid geometry observed

- 5-tile wall: row 1 has 3 tiles, row 2 has 2 tiles, horizontally centered under row 1 — an even gray gutter separates tiles and surrounds the whole block, which itself is vertically centered in the 1080-px canvas.
- 8-tile wall: row 1 and row 2 each have 3 tiles, row 3 has 2 tiles, again centered. Same gutter width as the 5-tile wall by eye.
- Each tile appears to be an independent center-crop/fill of its source frame (portrait subjects are cropped tighter or looser depending on tile aspect — see per-tile `cropLeft/cropRight` values captured live for the individually-assigned `CoreVideo Participant N` sources in scene "8 Sources", which range from 156px to 326px of asymmetric horizontal crop across the 8 tiles).
- This geometry (tile count → row/column layout, centering, gutter) is exactly what Task 6 needs to reproduce pixel-for-pixel on the GPU path.

## Neutral placeholder reference — no `before-neutral.png` exists

The plan's file list and Task 3 both reference `docs/design-reference/tiles-gpu-parity/before-neutral.png` by that literal name, as the reference for the *neutral placeholder* colour — the grey drawn where a tile has no current frame. **That file does not exist and was never captured.** A live meeting was active for the entire session (see "IMPORTANT DISCREPANCY" below), so every tile in every one of the four delivered captures was populated; there was no way to force an all-neutral wall without tearing down the owner's live meeting, which this task was explicitly told not to do.

Nothing needs recapturing to close this gap, though — the neutral fill is already present in every delivered image, just not covering the whole canvas. Confirmed directly against source: `src/zoom-supersource.cpp:28-30` defines `kNeutralY = 0x80` / `kNeutralUV = 0x80` as "Neutral fill for the background and for tiles with no frame yet," and `:641-644` shows the `clear_all` path `memset`ing the *entire* I420 canvas (Y plane and interleaved U/V) to those constants before any tile is drawn. So the same constant backs both the full-canvas clear and any empty-tile slot — one value, two use sites.

**Use the gutter/margin region of any of the four delivered images as the neutral reference — sample there, not a tile.** All four (`before-corevideo-tiles.png`, `before-corevideo-tiles-verify-.png`, `before-live-5.png`, `before-live-8.png`) show the same flat gray fill around and between tiles (canvas margins, and the strips separating adjacent tiles). The source-side expected value at those pixels is:

| Plane | Expected value |
|---|---|
| Y | `0x80` (128) |
| U | `0x80` (128) |
| V | `0x80` (128) |

**Maps directly to Task 6 check #6** ("the neutral placeholder is the same grey"): sample a gutter/margin pixel from the GPU-path output at the same canvas position, and confirm it still reads `0x80`/`0x80`/`0x80` on the source I420 planes. Caveat: the four PNGs here are RGBA, re-encoded by OBS's screenshot path from the underlying I420 buffer — they're a reasonable *visual* sanity check (the gutter should look like a flat, neutral mid-gray with no colour cast, and it does in all four) but not a guaranteed bit-exact stand-in for the raw `0x80/0x80/0x80` I420 values. For an exact numeric check, read the source I420 buffer (or an equivalent raw capture) rather than trusting the PNG's re-encoded RGB value to round-trip precisely.

## IMPORTANT DISCREPANCY: the "no live meeting" premise was contradicted by direct observation

I was told, as verified ground truth going into this task: *"There is currently no live Zoom meeting, so the walls will render neutral/empty. That is expected and is exactly the geometry baseline we want."* I was also told Step 2 (the live-feed baseline) could not be completed today and to record it as **MISSING**.

Both of those statements turned out to be wrong when I actually queried the running system. I want to be precise about what I found rather than silently following the stale premise or silently overriding it:

1. **The screenshots are not neutral/empty.** Both `CoreVideo Tiles` and `CoreVideo Tiles (verify)` rendered fully populated grids of participant video the moment I took the first screenshot (`before-corevideo-tiles.png` / `before-corevideo-tiles-verify-.png`), not a gray/blank canvas.
2. **`obs-websocket` reports an active meeting.** All 8 `zoom_participant_source` inputs used in the "8 Sources" scene report `status_label: "In meeting"` with a `live_status` string showing `1920x1080` video at a per-source fps (e.g. 60.0, 47.5, 25.6, 44.9, 43.5, 48.5, 46.5, 47.6 — see `roster.json` capture below for the full dump), assigned to named participants: CJ Covell, Anika Patel, Tom Ferguson, Michelle Zhang, Elena Kovač, John Kinsborough, Sarah Muller, Susan Cho.
3. **The content is genuinely moving, not a frozen cached frame.** I took three screenshots of `CoreVideo Tiles (verify)` three seconds apart and hashed them: all three had different byte counts and different SHA-256 hashes (`9f97d8a1...`, `6b487649...`, `068e884f...`). A frozen last-good-frame fallback (which this plugin is known to have, per project history around "still-media frames") would hash identically between polls; this did not.
4. **Zoom processes are running.** `Get-Process` showed two `Zoom.exe` instances, `ZoomObsEngine`, `zcscpthost`, and `CptHost` all active at the time of capture.

Taken together (status metadata + changing pixel hashes + running processes), this is strong evidence that a live Zoom meeting with at least 8 participants sending video was active on this machine while I worked, directly contradicting what I was told had been "verified minutes ago."

**What I did about it:** rather than discard this — the brief itself stresses this capture is one-shot and irreplaceable once the compositor changes — I captured it. `before-live-5.png` and `before-live-8.png` are a second round of screenshots of the same two sources, taken ~8 minutes after the first round, explicitly intended to serve as the Step 2 "live-feed" baseline the brief asked for.

**What I did *not* do:** confirm that this is the owner's own meeting with real human participants, as Step 2 of the brief specifically asks for ("Requires the owner and a Zoom meeting"). I have circumstantial reasons to suspect this may instead be an automated/synthetic test rig rather than an organic human meeting:
- All 8 faces have a uniform, glossy, stock-photo/AI-generated aesthetic (studio-lit headshots against generically "corporate" backgrounds — library, café, open-plan office, server room, park, showroom apartment) rather than the visual variety of real home/office webcams.
- One tile (`CoreVideo Participant 8` / "Susan Cho," a shared workspace scene) carries a visible on-screen overlay reading **"Time until Meeting Reset:"** next to a QR code and **"powered by mimoLive"** branding — mimoLive is a competitor product to CoreVideo Pro. This strongly suggests that tile's "webcam" is actually a looping recorded demo/marketing video being fed in as a stand-in participant feed, not an organic human camera.
- `CoreVideo Participant 8` is also the only one in `active_speaker_mode: true` / `assignment_mode: 1` with `participant_id: 0`, distinct from the other 7's static numeric-ID assignment — consistent with a test-harness slot rather than a manually-assigned participant.

**Net effect on verification:**
- **Geometry parity** (tile count → grid layout, centering, gutters, per-tile crop) is fully covered by all four images regardless of whether the content is a real meeting or a test rig — the compositor doesn't know or care where the pixels came from.
- **Colour/decode parity** (the part Step 2 specifically exists for) *is* covered by `before-live-5.png` / `before-live-8.png`. BT.709 conversion correctness depends only on real YUV bytes flowing through the real Zoom decode → tile compositor path — and that's established independently of what's actually in front of the camera, by the live `zoom_participant_source` status and the changing screenshot hashes recorded above. Whether the faces are organic webcams or a synthetic/looping test rig doesn't touch the shader/conversion maths, so it does not undermine the colour check.
- **Reproducibility** is the real thing the synthetic-feed observation raises, not colour correctness: if Task 6 wants to compare its GPU-path output against literally the same source frames as this baseline, it needs the same feeds still live and unchanged at that point. That can't be guaranteed for feeds that may be a looping/test asset with its own reset cycle (see the "Time until Meeting Reset" overlay on one tile) rather than a stable, owner-controlled meeting. Recommend Task 6 recapture against whatever feeds are live at the time, close to when the GPU-path comparison actually runs, rather than assuming these exact frames are still being served later.

Raw evidence (full participant roster, `GetInputSettings` dump) was saved during this session to a scratch file, not committed to the repo:
`C:\Users\walla\AppData\Local\Temp\claude\C--Users-walla\ee90dd79-2091-49db-b5b0-b49223466efa\scratchpad\roster.json`

## Commands run

All commands were run from the repo root (`C:\Users\walla\CoreVideo\cv-tiles2-wt`) with Node.js v24 on PATH, against `obs-websocket` at `127.0.0.1:4455` (`auth_required: false`).

```
mkdir -p "docs/design-reference/tiles-gpu-parity"
node <scratch>/capture-baseline.js      # Step 1: GetInputList + GetSourceScreenshot for both tiles sources -> before-corevideo-tiles*.png
node <scratch>/gather-info.js           # GetVersion, GetVideoSettings, GetSceneList, GetSceneItemList, GetCurrentProgramScene
node <scratch>/investigate-sources.js   # GetInputList(kind=zoom_participant_source) + GetInputSettings per participant
node <scratch>/check-live-stability.js  # 3x GetInputSettings poll, 2s apart, to check for change
node <scratch>/diff-check.js            # 3x GetSourceScreenshot poll, 3s apart, SHA-256 hash comparison
node <scratch>/capture-live.js          # Step 2: second GetSourceScreenshot round -> before-live-5.png / before-live-8.png, + roster.json dump
Get-Process | Where-Object { $_.ProcessName -match 'zoom|CptHost|obs64' }   # confirm Zoom/OBS processes running
Get-Item '...\obs-zoom-plugin.dll' | .VersionInfo                          # check for embedded version stamp (none found)
```

All scratch scripts were written under the session scratchpad directory (`C:\Users\walla\AppData\Local\Temp\claude\C--Users-walla\ee90dd79-2091-49db-b5b0-b49223466efa\scratchpad\`), never inside this repo.

## Self-review

- Confirmed all 4 PNGs are 1920×1080 RGBA via `file` (not just trusting the write succeeded).
- Confirmed the 4 PNGs have distinct SHA-256 hashes (no duplicate/copy-paste capture).
- Did not restart, rebuild, or reinstall OBS or the plugin, per the constraint given.
- Did not fabricate or substitute a still image for the live baseline — both `before-live-*.png` are genuine `GetSourceScreenshot` captures of the live running sources.
- Actively investigated and disclosed a contradiction between the task's stated ground truth and direct observation, rather than silently trusting either the stale premise or the surprising live data.
- Left open, and explicitly flagged, the one thing I could not verify from the tools available to me: whether the live content is an organic owner-run human meeting (as Step 2 of the brief specifies) or a synthetic/automated test rig. This is a judgment call for the owner, not something an agent without rig/meeting access can resolve conclusively.

---

## Phase A parity verification (2026-08-10, GPU build `bdf3918`)

Performed by the controller directly, on the owner's machine, against the
mimoLive test room with 8 participants sending video.

**After images**
- `after-corevideo-tiles-2.png` — 8-tile Auto wall, source *showing* in the
  preview scene. This is the image to compare against `before-live-8.png`.
- `after-corevideo-tiles-verify-.png` — 10 KB, a flat neutral canvas. This is
  **not a failure**: that source is hidden (`showing=false`), and a hidden wall
  correctly renders the neutral grey canvas rather than transparency. It is
  evidence for check 6 below.

**Checks, with what was actually observed**

| # | Check | Result |
|---|---|---|
| 1 | Tile positions and sizes identical | Pass — no difference detectable comparing the two images at full resolution |
| 2 | Gutters and margins identical | Pass — visually identical, and separately measured at RGB `128,128,128,255` across the whole canvas |
| 3 | Short-row centering identical | Pass — both render 3-3-2 with the bottom row centered |
| 4 | Crop framing identical | Pass — each participant sits the same way within their tile |
| 5 | Skin tones and background colours match | Pass by visual comparison. See the caveat below |
| 6 | Neutral placeholder is the same grey | Pass — measured `128,128,128,255`, matching the CPU fill's `0x80` |

**Caveat on check 5, stated plainly.** This was a side-by-side visual
comparison at full resolution, not a numeric per-pixel difference. A systematic
sub-LSB colour error would not be visible this way. The specific risk that
motivated the check — a wrong chroma offset — was independently closed by
reading the shader against libobs's own matrices (`media-io/video-matrices.c`,
full-range `black_levels = {0, 128, 128}`), and the offset is `128.0/255.0`.
A numeric diff would still be stronger evidence and is not done.

**Also not done:** the render-time measurement at 6+ tiles (plan Task 6 step 3).
That number was to decide whether the GPU path was worth doing and to inform any
future 4K work; it is still owed.

**Known bounded difference to expect.** The GPU path divides the draw scale by
the truncated integer crop passed to `gs_draw_sprite_subregion`, and truncates
the crop to whole rather than even source pixels. This is a sub-canvas-pixel
difference from the CPU path by construction, not a defect.

### Correction: one real visible difference (found in review, after the checks above)

The GPU effect samples with `Filter = Linear`; the deleted CPU path scaled with
nearest-neighbour (`blit_plane_nearest`). Tiles are therefore **softer** on the
GPU path wherever a feed is scaled, which is a larger visible difference than
the sub-pixel crop truncation noted above, and it was missed by the side-by-side
comparison recorded here.

This is a quality improvement, not a regression — nearest-neighbour downscaling
of a 720p feed into a small tile aliases badly. But Phase A's stated contract
was "no operator-visible change", and this is one. Recorded rather than quietly
accepted: reverting to nearest to honour the letter of the contract is possible
(`Filter = Point`) and is the owner's call.
