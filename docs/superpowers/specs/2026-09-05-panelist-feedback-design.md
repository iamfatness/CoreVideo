# Panelist Feedback System — Design

**Date:** 2026-09-05
**Status:** Draft — integration points pending codebase research
**Branch:** `feat/panelist-feedback`

## Goal

Give a producer running a Zoom panel two things Zoom does not provide:

1. **Loudness consistency** — see, at a glance, whether every panelist is arriving at a comparable loudness, in real broadcast units.
2. **Framing self-correction** — return a feed into the meeting that tells the person currently on air that they are badly framed, so they fix it themselves.

Plus an operator-side option to auto-frame Tiles.

## Operating context: this is a PRESHOW tool

It runs before the show is live — green room, mic check, rehearsal. Every
consequence below follows from that and should not be re-derived later.

- **No audience exposure.** The only people in the meeting are the
  production team and the panelists. A return feed everyone can see is the
  *intended* delivery, not a leak. No spotlight, no private-video
  workaround, no webinar-attendee risk.
- **No on-air risk.** Nothing here touches a live program shot, so
  framing changes cannot damage a broadcast in progress.
- **Loudness is measured per check, not per session.** A panelist speaks
  for roughly 20–60 s during a mic check. Integrated loudness must be
  scoped to *that panelist's check window* and be resettable, or the
  number is polluted by whoever spoke before them. Short-term (3 s) is the
  live readout while they talk; integrated is the verdict once they stop.
  At a 100 ms block hop, 20 s of speech yields ~200 gated blocks, which is
  ample for the relative gate to be meaningful.
- **The meter is a readiness board.** One row per panelist: loudness
  deviation from the panel reference, framing status, pass/fail. That is
  the preshow-shaped presentation of the same measurements, and is the
  primary UI for Subsystem 1.

## Non-goals (YAGNI)

- No automatic reframing of the on-air program shot. Directors do not want a shot that moves by itself. Auto-framing applies to Tiles only, as an opt-in, and to nothing else.
- No gaze, pose, emotion, or identity recognition. A face bounding box plus five landmarks is the entire perception budget for v1.
- No per-person private messaging. Zoom has no private video; the return feed is visible to everyone in the meeting. This is accepted, not worked around.
- No loudness *correction*. We measure and display. Auto-gain on a live panel is a separate, riskier product.

## Why this is feasible now

The idea was paused on 2026-09-02 because a framing tool cannot tolerate the 200–400 ms Zoom return path. That objection does not apply here:

- **Framing advice is human-paced.** The panelist takes seconds to react. 400 ms of transport disappears into their reaction time.
- **Tiles auto-framing has no return path at all.** It is a local crop on video we already receive.
- **The return feed is an OBS scene on the OBS Virtual Camera.** It does not use `setExternalVideoSource`, so the unproven raw-video-send entitlement is not on the critical path. No new Zoom SDK surface at all.

## Architecture

Two independent engines, three consumers.

**The plugin does not link the Zoom SDK.** Media crosses a process boundary
first — this is the real topology and every task must respect it:

```
Zoom SDK ─> ZoomObsEngine.exe ─> SHM ring ─> obs-zoom-plugin
            (engine/src/engine-audio.cpp)   (src/engine-ipc.h)
```

Within the plugin:

```
per-participant PCM ──> Loudness engine (BS.1770-4) ──> [1] Meter source (operator)
                                                    └─> [3] Return overlay (loudness half)

per-participant video ──> Subject detector (box +   ──> [2] Tiles auto-frame (operator)
                          5 landmarks, 2–5 fps)     └─> [3] Return overlay (framing half)
```

The engines share nothing. Either can ship without the other. The detector has two consumers and must therefore expose a stable, small result type.

Note: `src/zoom-audio-delegate.*` and `src/zoom-audio-router.*` are orphaned
dead code — not in any CMake target. Do not build against them.

---

### Subsystem 1 — Loudness engine + meter source

Implements ITU-R BS.1770-4 (the measurement) and EBU R128 (the practice).

**Filter chain, per channel:**

1. Stage 1 — high-shelf "head" filter, roughly +4 dB above 1 kHz.
2. Stage 2 — RLB high-pass, roughly 38 Hz.

**Sample rate must be treated as a runtime variable, not 48 kHz.** BS.1770-4
publishes its biquad coefficients *only* for 48 kHz, but this codebase does
not receive a guaranteed rate: the engine reads `data->GetSampleRate()` per
buffer and stamps it into the ring header (`engine/src/engine-audio.cpp`),
and no constant anywhere asserts a rate for the receive path. Zoom commonly
delivers 32 kHz. Therefore the loudness engine **must** read
`ShmAudioHeader::sample_rate` at runtime and **derive** the filter
coefficients for that rate — hardcoding the published 48 kHz constants would
silently mis-weight every measurement. This is the single most likely way to
ship a meter that reads plausibly and is wrong.

Input format is **16-bit signed, interleaved**; channel count is likewise
runtime-discovered.
3. Mean square per channel, then weighted sum:
   `L = -0.691 + 10 * log10( Σ G_i * z_i )`
   with `G = 1.0` for L/R/C, `1.41` for surrounds, `0` for LFE. Zoom participant audio is mono or stereo, so only `G = 1.0` terms apply.

**Three measurements, all required:**

| Measure | Window | Gating | Use here |
|---|---|---|---|
| Momentary (M) | 400 ms | none | live bar movement |
| Short-term (S) | 3 s | none | the number an operator reads |
| Integrated (I) | whole session | absolute −70 LUFS, then relative −10 LU | per-panelist consistency |

**The gate is what makes this work.** A panelist is silent roughly 80% of a panel. An ungated integrated reading over that would be meaningless. The BS.1770 absolute gate at −70 LUFS discards silence for free — it is precisely the mechanism this use case needs, so we implement gating properly rather than shortcutting to a running average. Integration uses 400 ms blocks with 75% overlap (a new block every 100 ms).

**The product insight: consistency is relative, not absolute.**

The operator does not primarily care that a panelist hits −23 LUFS. They care that panelist A is not 6 LU louder than panelist B. The meter must therefore surface, per panelist:

- short-term LUFS (absolute), and
- **deviation from the panel reference**, in LU,

where the panel reference is the **median** integrated loudness across panelists who have cleared the absolute gate. Median, not mean, so one very loud or very quiet participant does not drag the reference.

Reference targets offered as presets: EBU R128 `−23 LUFS`, ATSC A/85 `−24 LKFS`, streaming `−16 LUFS`. Default to *panel median* rather than a fixed target, because matching each other is the actual goal.

**True peak** (dBTP, ≥4× oversampled) is deferred to v2 — it guards against clipping on distribution, which is not what this feature is for.

---

### Subsystem 2 — Subject detector

**Cadence is the core design decision.** Detection runs at **2–5 fps per monitored participant, on a downscaled frame (long edge ~320 px)** — not per frame, not at full resolution. Justification:

- Framing advice is consumed by a human who reacts in seconds.
- Tiles auto-framing *must* be slow, or tiles visibly twitch.

**We receive 1080p per participant**, so downscaling is a pure efficiency choice, not a quality compromise — a face in a 1080p source remains well-resolved at a 320 px long edge. Detection accuracy is not a constraint here; cost is.

**Model requirement: the detector must return five landmarks** (both eyes, nose, two mouth corners), not just a box. Eyeline is the basis of every real framing rule, and approximating it from a box is a heuristic we do not need to accept.

#### Library decision (spike complete, 2026-09-05)

**Use libfacedetection (ShiqiYu), vendored as source.**

- **License: 3-clause BSD**, verified from the upstream LICENSE file. No non-commercial clause, no field-of-use restriction. Obligation is a copyright notice in our attributions — nothing more.
- **It *is* YuNet.** Upstream converted the OpenCV Zoo YuNet ONNX model into static C arrays. So we get the leading model without the runtime that usually carries it.
- **Zero dependencies, and no external model asset.** Four files in `src/` (~500 KB, of which ~446 KB is weights compiled in as C arrays), plus a one-line export header we write. Nothing to install, path-resolve, or sign at runtime; no new DLL in the plugin folder. Upstream explicitly sanctions copying the sources into a host project.
- **Five landmarks including mouth corners** — the exact set we need.

**Rejected, with reasons worth recording:**

| Candidate | Verdict |
|---|---|
| YuNet via `cv::FaceDetectorYN` | Same model, but requires opencv_core + dnn + imgproc. Keep as documented fallback only. |
| BlazeFace / MediaPipe | No mouth corners (6 keypoints incl. ear tragions); no supported C++ desktop path (Bazel, TFLite); poor MSVC history. |
| **SCRFD / RetinaFace (InsightFace)** | **License blocker** — pretrained weights are non-commercial-research only. |
| **dlib 5/68-point** | **License blocker** — iBUG 300-W excludes commercial use. |
| Ultra-Light-Fast-Generic-1MB | MIT and tiny, but bbox only, no landmarks. |

**Build notes that will otherwise cost a day:** input must be **BGR 3-channel** (not RGB, not YUV — a colour convert is required from whatever the video path hands us); verify `/arch:AVX2` actually reaches the compile line, since MSVC can silently accept AVX2 intrinsics under SSE2 and the resulting slowdown looks like a library fault; do not enable `/openmp` — we thread per-participant ourselves, which is what upstream recommends.

**Keep the detector behind a narrow interface** (`frame → box + 5 points`) from day one, so the OpenCV-DNN fallback stays a contained swap if local benchmarking disappoints.

#### Scheduling: cost is O(1) in panelist count, not O(n)

Published cost is 13.09 ms for 320×240 on a 2017 i7-7820X; ~10 ms at our 320×180 is an **extrapolation, not a measurement**. Naively running 10 panelists at 5 fps would be ~0.5 of a core.

We do not need that. **One detector worker thread services all participants on a round-robin schedule**, with a priority boost for the active speaker (who is the one being checked). One detection per ~100 ms cycled across the roster costs roughly a tenth of a core *regardless of how many panelists there are*, and every consumer here is slow enough not to notice: framing advice is human-paced, and per-tile crops are hysteretic by design.

This also means panelist count never threatens the preshow tool's footprint — the cost ceiling is fixed by the schedule, not the roster.

**Local benchmarking is still required** before finalizing the tick rate. Treat every figure above as a planning estimate.

**Output type (stable, shared by both consumers):**

```cpp
struct SubjectFrame {
    bool     found;
    float    box_x, box_y, box_w, box_h;          // normalized 0..1 of source
    float    eye_l_x, eye_l_y, eye_r_x, eye_r_y;  // normalized 0..1
    float    confidence;
    uint64_t detected_ns;                          // source frame timestamp
};
```

**Temporal smoothing is mandatory, not optional.** Raw per-detection output jitters. Both consumers require a hysteretic, rate-limited signal:

- **deadband** — ignore movement below a threshold,
- **rate limit** — cap crop travel per second,
- **dropout hold** — keep the last good result for N seconds before declaring "no subject", so a head turn does not read as "left frame."

---

### Subsystem 3 — Consumers

**3a. Tiles auto-frame (opt-in).** Produces a per-tile crop rect placing the eyeline on the upper-third line and centering the subject horizontally, clamped so the crop never exceeds source bounds and never zooms past a configured maximum. Off by default.

**3b. Return scene + overlay.** An OBS scene showing the active speaker with framing advice, routed out via the OBS Virtual Camera and joined to the meeting by a separate Zoom client.

Conditions detectable from a box plus landmarks, and the advice each maps to:

| Condition | Advice |
|---|---|
| No face for longer than the hold period | "Step into frame" |
| Box touches a frame edge | "You're cut off — move right/left/down/up" |
| Eyeline well above the upper-third line | "Lower your camera" |
| Eyeline well below the upper-third line | "Raise your camera" |
| Box height below minimum fraction | "Move closer" |
| Box height above maximum fraction | "Move back" |
| Horizontal center off by more than tolerance | "Shift left/right" |

**Overlay legibility is a hard constraint — and spotlight is not the fix.** The limit is not Zoom's re-encode; it is that a gallery tile occupies roughly 640×360 of *physical screen space* on a panelist's display however clean the encode is. Spotlighting the return would not solve this and is explicitly not required. Instead, design the overlay to read at gallery-tile size: chunky segments, oversized numerals, hard contrast, no hairlines, no fine text. The overlay must be designed and reviewed at 640×360, not at 1080p.

---

## Integration facts (researched 2026-09-05 — build against these, not assumptions)

### Audio

- **Tap point:** `src/zoom-participant-audio-source.cpp:698-714`, where fully decoded per-participant int16 PCM sits immediately before publish.
- **Thread:** the dedicated **audio lane** thread (`m_audio_lane` in `ZoomEngineClient`). Never the IPC reader thread — it head-of-line-blocks every source; never the OBS audio-mixer thread, which is budget-critical.
- Media events are **coalescing prompts, not payloads** — integrate over the whole drain loop, not per wakeup.
- `roster()` deep-copies strings under a hot mutex. **Cache names; never call it per frame.** Use `add_roster_callback` for changes.
- Cross-thread readout should mirror the existing `g_sources_mtx` + `std::vector<...*>` registry in the same file. Lock order: `g_sources_mtx` before any `ctx->mtx`, never the reverse.

### Video

- **Format: I420 planar, BT.709, full range** (the engine normalises limited→full every frame). Not NV12, not BGRA.
- **CPU access is free** — the plugin already holds a plain I420 buffer (`feed->frame`).
- **libfacedetection requires BGR 3-channel.** The Y plane alone is *not* sufficient for this CNN, unlike a Haar/HOG detector. A cheap **I420 → BGR convert at the downscaled size** (~320×180) is required and must be budgeted. Do not feed replicated greyscale; U and V are right there.
- **Thread:** the frame callback runs on the **shared engine-IPC reader thread that serves every source in the plugin**. Detection must never run there. Tap a copy under `feed->mtx`, or the graphics-thread `TileScratch`, and hand it to the detector worker.
- `src/zoom-video-delegate.cpp` is **dead code** (no CMake target). Do not build on it.

### Tiles crop — the single insertion point

`src/zoom-supersource.cpp:1957`, the `CropRect crop = solve_slot_crop(...)` inside the `draw_tile` lambda. It is the only place the sampled rectangle is decided, and `crop_uv` plus the border/glow registration derive from it. Four constraints:

1. Result aspect must be exactly `params.tile_aspect` or the tile letterboxes.
2. Must stay within `[0, tex_w] × [0, tex_h]`; zero width/height falls back to the placeholder.
3. `crop_uv` must be computed from the **truncated integers**, not the doubles, or borders misregister.
4. Framing state must live where `render_slot_crop` lives: authoritative copy under `ctx->mutex`, snapshotted once per frame at `:1286` so a framing pass lands as a unit.

### Active speaker

- Use `ZoomEngineClient::active_speaker_id()` — the *directed* id, post hold/dwell. Not the raw one.
- `SpeakerDirector::snapshot()` carries everything an overlay wants (candidate, hold remaining, manual override, excludes).
- **It is poll-only** — no observer list. Poll it; do not expect events.
- A dedicated **`corevideo_active_speaker_source` already exists** (`src/zoom-source.cpp:2875`) with hidden-preview handover. That is the natural host for the framing overlay.

### Testing conventions (non-negotiable in this repo)

- **No test framework.** Plain `int main()` with a local `check()`. Do not introduce gtest or Catch.
- Target `CoreVideo<Thing>Test`, ctest name `CoreVideo<Thing>`, registered by hand inside `if(BUILD_TESTING)` in the root `CMakeLists.txt`. No `tests/CMakeLists.txt`.
- **No headless GPU harness exists and one has been ruled against** (an offscreen Qt harness "certified it three times and was wrong three times"). The sanctioned approach: extract the decision into a pure header and unit-test that. `tests/tile-shape-test.cpp:189-240` reproduces the shader's crop arithmetic in plain C++ — **auto-framing crop math must be pinned exactly that way.**
- Build/run: `cmake --build build --config Release --parallel 8` then `ctest -C Release --output-on-failure`, N/N green.
- New pure logic goes in a **header-only file under `src/`** with a "why this exists" comment. This repo has never vendored a library; libfacedetection will be the first, and that is a deliberate exception justified by the license/dependency analysis above.

## Deployment constraints

- **Self-tile exclusion is mandatory, and is real work — no such code exists.** Confirmed by exhaustive search: `ParticipantInfo` has no self/me field, the engine builds the roster from `GetParticipantsList()` with zero filtering, and the bot therefore appears in every roster, picker, tile candidate set, and speaker candidate set. The only existing defence is a handful of operator-chosen exclude combo boxes keyed by a **meeting-scoped `user_id` that does not survive a rejoin**. Worse: **talkback deliberately unmutes the bot**, so during a talkback key the bot is a fully eligible active-speaker candidate. Adding a vcam return feed on top of this makes a real feedback loop likely, not hypothetical. This needs a durable self/return identity flag, not another combo box.

### OPEN QUESTION — tile source resolution vs. auto-framing quality

Tiles subscribes at **P360 by default** (`tile_feed_subscribe`, `zoom-supersource.cpp:475-480`), not 1080p. The rationale is recorded in place: on 2026-08-17 a 720p wall oversubscribed Zoom's raw-data envelope and throttled the entire meeting to 0.3–0.45× real time. The engine's policy is upgrade-only, so a participant already carried at 720p/1080p for a program output is shared at that higher resolution — but the default wall is 360p.

Consequences, which differ per subsystem:

- **Detection is unaffected.** We downscale to ~320 px long edge anyway; a 640×360 source is ample.
- **Auto-framed tile *quality* is affected.** Cropping into a 360p source and magnifying the result will look soft. Getting crisp auto-framed tiles means raising the subscription resolution — which walks straight back into the envelope that caused the 2026-08-17 throttle.

This gates Subsystem 3a only, and 3a is last in the build order, so it does not block anything before it. Options: accept softness at 360p; raise resolution only for the small number of tiles actually being auto-framed; or restrict auto-framing to preshow, where a throttled meeting is survivable.
- **The vcam needs its own seat.** Some Zoom client must select OBS Virtual Camera as its webcam; that is a normal client, not our SDK identity. Prior findings warn that same-account joins collide, so this should be a separate account.
- Broadcast sample rate is 48 kHz. Resample, or recompute filter coefficients, if participant audio arrives at another rate.

## Risks

| Risk | Mitigation |
|---|---|
| Detector library license / size / perf unknown | **Spike gates the choice.** Measure before committing. |
| Detection cost scales with panelist count | Low cadence + downscale + monitor only participants actually shown |
| Return tile feedback loop | Explicit self-exclusion, with a test |
| Overlay unreadable after Zoom re-encode | Design and review at 640×360 |
| Integrated loudness meaningless for silent panelists | BS.1770 absolute gate handles this by design |
| Tiles auto-frame twitches | Deadband + rate limit + dropout hold |

## Build order

1. **Loudness engine + meter source** — no detector, no vcam, no spike. Ships alone and is independently valuable.
2. **Detector spike, then detector engine** — gates on library validation.
3. **Consumers** — Tiles auto-frame, then return scene + overlay.

Each is a separate implementation plan producing working software on its own.
