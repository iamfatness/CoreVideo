# CLAUDE.md

Project notes for Claude Code sessions working in this repository: CoreVideo,
the OBS Studio plugin that pulls Zoom meeting video and audio into OBS as
native sources. Current release: **v0.1.44** (2026-08-22). Update this file in
the same change as any substantive work — docs-updated is part of done.

## Architecture in one paragraph

Two processes. The OBS plugin (`src/`, `obs-zoom-plugin.dll`) and a standalone
engine (`engine/src/`, `ZoomObsEngine.exe`) that links the Zoom Meeting SDK.
They talk over two named pipes (`ZoomObsPlugin_P2E` / `_E2P`, one JSON object
per line; wire format and all shared structs live in `src/engine-ipc.h`) and
move media through named shared-memory regions (`ZoomObsPlugin_<uuid>_video` /
`_audio` / `_share`, capped at `kMaxShmSources = 32` across all three kinds).
The plugin always launches the engine; the engine never outlives it on purpose
(see "process hygiene" below). Media **events are prompts, not payloads**: a
video event means "read the newest frame in the region", an audio event means
"drain everything pending in the ring" — this property is load-bearing for the
dispatch design and must survive refactors.

## Build, test, install

Full toolchain setup is in `README.md` (§Building). Day-to-day, an existing
configured worktree is just:

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure   # must be N/N green
```

Gotchas: CMake wants forward slashes in `-DCMAKE_MODULE_PATH` (dies on `\U`);
tests are plain executables with no framework, one `check()`-style file per
invariant cluster in `tests/`.

Installing to Program Files requires OBS closed, elevation (UAC), and **always
both binaries as a pair** — `obs-zoom-plugin.dll` AND
`zoom-runtime\ZoomObsEngine.exe`. Half the fixes in any release are
engine-side; a DLL-only copy silently half-applies. Back up the installed pair
first; verify with SHA256 after.

## Invariants that have each caused a live-show defect

Every one of these is documented at length where it lives; the list is the map.

- **Audio ring** (`ShmAudioHeader` in `src/engine-ipc.h`): 8 slots, per-slot
  seqlock, **free-running** uint32 write/read indices (modulo only for
  physical offsets — collapsing "caught up" and "lapped by exactly one ring"
  was a real defect). Readers drain fully on any wakeup and account for every
  lost slot (`audio_timeline_skip`).
- **Edge-triggered notify** (`notify` flag + helpers in `src/engine-ipc.h`):
  whoever consumes a wakeup owns the flag until `audio_ring_reader_done` sees
  the ring empty after clearing, or `audio_ring_reader_abandon` hands it back.
  Any return path that consumes a wakeup and leaves the flag set silences the
  source until the writer's ~2.5 s keepalive. The seq_cst fence-pair proof is
  on the struct; don't weaken the fences.
- **Master clock** (`src/audio-timeline.h`): timestamps derive from samples,
  never arrival. Reset only on re-subscribe / new engine / rate change.
  Asymmetric drift clamp (50 ms forward, +80 ms backward burst allowance tied
  to ring capacity by a test).
- **Media dispatch lanes** (`src/media-event-queue.h`, `MediaDispatchLane` in
  `zoom-engine-client.h`): the pipe reader thread must never run media
  callbacks inline — at real 1080p load that starved audio events by up to a
  second. Video lane is latest-wins, audio lane drains; control events stay
  ordered on the reader thread.
- **SHM generations** (`src/shm-generation.h`, `shm_region_name`): a Windows
  named section cannot grow while any process maps it, so every resize moves
  to a `_gN`-suffixed name. Generation counters are process-wide, not
  per-target.
- **Process hygiene** (`terminate_stale_engine_processes` in
  `src/zoom-engine-client.cpp`): every engine launch first kills any existing
  `ZoomObsEngine.exe`. An orphaned engine ghost-writes same-named regions and
  poisons the notify flag (~92% audio loss, no error anywhere) — root-caused
  live 2026-08-17. `start()` is serialized (`m_start_mtx`); a name collision
  is surfaced as `shm_name_collision` (non-fatal, no reconnect vote).
- **FFmpeg runtime preload** (`cv_ffmpeg_loader_init` in
  `src/cv-ffmpeg-loader.cpp`): the delay-loaded avfilter family must be
  resident before any media flows — pinned by CoreVideoFfmpegPreloadTest.
  Left to the delay-load hook, the load fired on the first video frame inside
  `on_engine_frame` under `m_mtx` (which `on_engine_audio` also takes): a cold
  ~1.06 s load vs. the ring's ~80 ms dropped ~1 s of audio on every audible
  source at the first join of each OBS run (2026-08-18 soak).
- **Director handover** (`src/director-handover.h`): on an Active Speaker cut
  the hidden preview covers air until the main subscription delivers the
  participant we cut TO; exactly one of the two slots publishes audio at any
  instant (gate polarity in `on_engine_audio` / `on_director_preview_audio`).
- **Silence-resume fade** (`src/audio-silence-fade.h`): Zoom's one-way audio
  callback fires on schedule but can carry true zero-valued PCM for hundreds
  of ms (a bot's virtual mic idling between synthesized utterances, not a
  dropped callback — distinct from audio-timeline.h's gap handling). The
  first buffer after such a run is ramped in over `kAudioResumeFadeMs`
  rather than jumping straight to full amplitude — live-diagnosed as
  clicks/pops on the active-speaker feed by probing the audio SHM ring
  directly (2026-08-18/19). Applied in both `zoom-source.cpp`
  (`output_audio_from_shared_memory`, shared by the main and director-preview
  slots) and `zoom-participant-audio-source.cpp`.
- **ISO recording timing** (`src/iso-video-pacer.h`, `src/iso-audio-gap-fill.h`):
  raw video has no per-frame timestamps and ffmpeg cannot be trusted to
  invent correct ones from a byte stream — `-use_wallclock_as_timestamps`
  is confirmed (via `ffprobe -show_frames`, 2026-08-21) to have **no
  effect** on this project's ffmpeg build's rawvideo demuxer, despite
  looking like the textbook fix. `record_video_frame()` is called 1:1 with
  Zoom's real, fluctuating (10-60fps) per-source delivery, so it must pace
  itself to a fixed cadence (duplicate to backfill a stall, drop to shed a
  burst) BEFORE the pipe — see `iso_video_frames_due()`. Audio has the
  mirror-image problem for a different reason: Zoom only calls back audio
  for someone currently talking, so `record_audio_frame()` must backfill
  silence across every gap (`iso_audio_silence_frames()`) or the WAV
  shrinks by every silent stretch. Both anchor to the same
  `os_gettime_ns()` clock so video and audio stay in sync with each other,
  not just individually correct.
- **Colour range is normalised, never re-declared** (`src/i420-range-expand.h`,
  applied in `engine/src/engine-video.cpp`'s `onRawDataFrameReceived`): the
  engine requests `VideoRawdataColorspace_BT709_F` and the plugin declares
  `VIDEO_RANGE_FULL` on every frame, but the SDK does **not** always deliver
  full range — 16 of 15,203 frames in one live meeting arrived limited, on 5
  of 6 participants (live-probed 2026-08-22). Unexpanded, each is a ~33 ms
  brightness pop: the "gamma flash". `YUVRawDataI420::IsLimitedI420()` reports
  this per frame and had never been called. Expand the PIXELS when it is set;
  do not forward the flag to `obs_source_frame::full_range` — libobs keys async
  texture allocation on that field and `set_async_texture_size` destroys and
  recreates every texture when it changes, so per-frame flipping trades a
  one-frame pop for a rebuild storm. Diagnosed by attaching to the video SHM
  read-only from a third process and histogramming luma; the signature is a
  floor lifted off 0, a ceiling capped near 235, and the sub-16 population
  collapsing to single digits for exactly one frame.
- **Speaker-director tick stays local to the cut** (`src/zoom-dock.cpp`'s
  100ms refresh timer): the Active Speaker cut is entirely self-contained
  in `zoom-source.cpp`'s director-handover machinery. Never call
  `ZoomOutputManager::resubscribe_all()` from the automatic tick path —
  that function is for engine crash/reconnect recovery only (its own doc
  comment says so), and calling it on every ordinary promotion churns
  every unrelated fixed-participant output's video mapping on every
  speaker change (live-caught 2026-08-19, 8 sources resubscribing every
  12-90s for a whole show). The manual speaker Take/Release buttons still
  call it deliberately — that's a rare, operator-initiated action, not
  the automatic path.
- **ISO encoder demotion chain must actually chain**
  (`src/zoom-iso-recorder.cpp`'s `record_video_frame`): never gate a
  fresh demotion attempt on "has this uuid ever been demoted before" —
  `iso_demote_encoder()` (nvenc→qsv→amf→libx264) only terminates because
  `session.video_encoder != "libx264"` eventually goes false; re-checking
  that on every startup failure is the only guard the chain needs. A
  stricter one-shot guard leaves a source stuck at whatever the first
  demotion picked if that tier is ALSO unavailable (live-caught
  2026-08-19: two sources permanently stuck at 2 frames when neither QSV
  nor AMF worked on the box).
- **ISO ffmpeg feed** (`src/iso-ffmpeg-pipe.h`): QProcess is banned on the
  media threads — its Windows stdin chaining needs the owner thread's Qt
  event loop, and the dispatch lanes have none (live 2026-08-18: every ISO
  session froze at exactly 5 frames, 0-byte MP4s, 15 s shutdown kills).
  FFmpeg is fed by a raw pipe + blocking writer thread + bounded
  drop-oldest queue, pinned by CoreVideoIsoFfmpegPipeTest. The `-encoders`
  availability probe may keep QProcess: `waitFor*` pumps without a loop.
- **Talkback keying SELECTS, it never creates** (`session_start` in
  `engine/src/engine-talkback.cpp`, feat/talkback): channels are created at
  NOMINATION time, one `CreateChannel` at a time through the arbiter in
  `src/talkback-channel-owner.h`, and a key press only looks its target up in
  the provisioned table and starts sending. Creating on the press cost a
  create round trip plus an invite round trip before any audio could flow —
  measured live 2026-08-25 as buffers discarded on every press
  (`no_channel_drops`), i.e. the director's first syllable gone every time.
  An unprovisioned target is refused with a reason and never provisioned on
  demand, and a key RELEASE destroys nothing: the channel has to survive it or
  the next press pays the same cost. **Nothing SDK-shaped may sit between the
  key and the first buffer**: `talkback_start` and `talkback_audio` are
  branches of one command loop, so anything on that path runs before the first
  buffer is read. That is why the plugin opens the tap *before*
  `talkback_start` and why the background-volume duck is deferred to the first
  `drain_audio()` after its sends. The talkback ring is re-laid-out by
  `talkback_ring_init()` on every press, so `open_audio()` reads it **from
  index 0** — unlike the main audio ring, where a reader must snap past
  whatever a previous subscription left; snapping here discarded the
  director's first syllable rather than de-staling anything. And a key **may
  not report live over a dead audio path**: `session_start()` consults the
  last `open_audio()` verdict and refuses with its reason, because the
  plugin's status handler is last-write-wins and a `layout_mismatch` from a
  half-applied install would otherwise be overwritten by `live:true` — key
  open, cue played, nothing sent. A key pressed while the nomination ladder
  is still provisioning is REFUSED (`provisioning_incomplete`), never
  half-honoured — briefing ten of eleven while the log says "live" is the
  failure mode this whole feature is written against. A target is not a channel — all-talent
  past 10 people owns `ceil(n/10)` of them and one drain pass fans the same
  PCM out to all (`talkback_channel_serves_target`). Milestone incomplete
  until the Task 6 live gate: the first-syllable claim is measured on the old
  path, not yet re-measured on the new one.
- **Engine teardown**: never let SDK callbacks race teardown; `set_terminate`
  is a bare `_exit(5)` (code 5 maps to EngineCrash recovery; no pipe writes,
  no locks, no allocation in the handler).

## Live testing against a real meeting

The control API (TCP line-JSON, `127.0.0.1:19870`, no HTTP) drives a full
cycle: `join` (accepts a full Zoom URL in `meeting_id`), `start_engine`
**twice** (second grants record rights), `assign_output`, `list_outputs` /
`list_audio_sources` (per-source `audio_latency_us`, `overrun_slots` — the
first is queue depth: sub-ms healthy, tens of ms = something is starving),
`leave`. obs-websocket on 4455 (no auth) handles scenes. Rules learned the
hard way: **never run a second OBS instance** while one is testing (pipe/SDK
singleton collision, crash loop), and send `{"cmd":"leave"}` before closing
OBS. Diagnostic technique: a ring can be probed read-only from a third
process by name — watching `notify`/`write_index` from outside distinguishes
"writer stalled" / "reader wedged" / "ghost writer" in seconds.

`talkback_probe` (`{"cmd":"talkback_probe","participant":"<display name>"}`,
requires an active meeting) fires the Milestone 1 Zoom-talkback probe
(`engine/src/engine-talkback.h`/`.cpp`): can this account open a talkback
channel and put audio in it. The control API response only confirms the
trigger was accepted — every stage of the probe ladder (`controller`,
`meeting_supported`, `create_channel`, `invite`, `send`, `destroy`, timeouts,
stray-channel cleanup, etc.) arrives asynchronously as OBS log lines
(`blog(LOG_INFO, "[obs-zoom-plugin] talkback_probe: ...")`), not over the
control socket — watch the log, not the response.

One asymmetry to know before testing a join fix this way: the join watchdog
(`src/join-watchdog.h`) is armed in `on_join_clicked()` only. A control-API
`join` never sets `m_join_started_ms`, so the watchdog is inert on that path
and a fix to it **cannot** be proven by driving the API — it needs the dock
button.

## The Companion module

`companion/companion-module-corevideo-obs` is a Bitfocus Companion module
speaking the same control API. It needs **Companion v5+**: builds before v5
cap the module API at 1.12 and cannot load `@companion-module/base` 2.x at
all. Three things bite every time:

- `companion/manifest.json` is required (v3+); the legacy `companion` block
  in `package.json` is not enough, and its `runtime.apiVersion` is validated
  against the schema in `@companion-module/base/assets/manifest.schema.json`.
- The build must emit **ESM** (`module`/`moduleResolution: Node16`, plus
  `"type": "module"`). CommonJS output makes the bundler wrap the entrypoint
  so its default export is the namespace object, and Companion's loader
  rejects it with "Module entrypoint did not return a valid constructor
  function". Verify by importing the built bundle the way the loader does.
- Companion refuses to overwrite a module version already on disk, so
  **bump the version on every rebuild you intend to install** or you will
  keep testing the old bundle.

Dropdown choices are baked in at `buildActions()` time, so anything that
changes the roster, the outputs, or the OBS scene list must re-run
`setActionDefinitions(buildActions(this))` — `flushState()` only pushes
variables and feedbacks. Participants are stored **by name**, never by id:
Zoom user ids are meeting-scoped, so a button holding an id points at nobody
after a rejoin and at the wrong face once ids get recycled.

## Releases

`scripts/release-local.ps1 -Version vX.Y.Z -BuildPath build -Configuration
Release` rebuilds with the version stamped (`COREVIDEO_RELEASE_VERSION`; the
`project()` version in CMakeLists is a placeholder), packages zip + NSIS
installer + manifest into `dist/`. Its `-Upload` breaks under nested
PowerShell (credential-fill newlines) — publish with `gh release create`
instead; both paths create a normal release for the tag. **Never publish the
`sdk-assets` draft release** — it is private SDK storage. The public site
(`corevideo.io`, a Cloudflare Worker) takes its version from this repo's
`CHANGELOG.md` heading: add the release entry, then the `Deploy Site` workflow
(auto on docs/site paths, `workflow_dispatch` otherwise) rebuilds `public/`
from `scripts/build-site.mjs` — never hand-edit `public/`.

## Style

Comments state the constraint the code can't show — most files here carry the
defect history that motivates their invariants, in the pattern you see in
`src/audio-timeline.h`. Follow it: when a change is motivated by a live
failure, say what happened, with numbers. Tests pin invariants, not
implementations.
