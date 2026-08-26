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
  lost slot (`audio_timeline_skip`). "Account for" includes the LAP: the
  talkback drain's skip-forward counts the slots it steps over into `*lost`
  (`src/talkback-ring.h`), which it did not until 2026-08-25 — only seqlock
  give-ups were counted, so the larger and likelier loss was the one nothing
  reported, under a comment saying the caller reports it.
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
- **Talkback roster re-resolution** (`resolve_roster_change()` in
  `engine/src/engine-talkback.cpp`, called from `roster_changed()` in
  `engine/src/main.cpp`'s five roster SDK callbacks): a rejoin has to rebuild
  membership with no operator action, because nominations store names, never
  ids. This function may INVITE into a channel that already exists; it must
  NEVER call `CreateChannel` — that stays command-loop-thread-only under the
  arbiter's single-outstanding-create rule, and a roster burst has no way to
  serialize a create against one issued from `nominate()`. Presence is
  tracked per provisioned channel (`TalkbackProvisionedChannel::present`,
  confirmed by `onChannelUserJoinResponse`) so a burst of the five callbacks
  for one join invites exactly once — and `TALKBACK_ERROR_ALREADY_EXIST`
  (which can only ever arrive on that async callback, never on
  `ExecuteBatchInviteUsers`'s synchronous return) is treated as confirmed
  presence, not a failure to retry. `session_start()`'s `session_live` report
  now carries `members_present`/`members_total` for the keyed target — Task 3
  deliberately left this out because the provisioned entry didn't track
  membership at all. **A pending invite MUST expire** (fix round 1, C1,
  Critical, 2026-08-26): the suppression check keys on (channel, NAME), the
  only clearer keys on (channel, USER ID), and with no expiry a talent who
  drops while an invite is in flight and rejoins under a new id was
  permanently suppressed — the director sees "1 of 1 present" while the
  talent hears nothing. `TalkbackPendingInvite` now carries a `kAwaitTimeout`
  deadline AND is dropped the moment its `user_id` leaves the roster (the
  semantically right trigger, fires immediately rather than after 10s); both
  are needed, they close different halves. A genuinely failing invite
  (`TalkbackProvisionedChannel::failed`) is retried only on that PERSON's own
  join transition, never on a timer or on every roster event — two of the
  five callbacks fire on every mute and camera toggle by anyone in the
  meeting. The roster path reassigns `m_svc`/`m_ctrl` only when no session is
  live, matching `probe()`/`nominate()`'s own guard, because a roster event
  can fire mid-press and a stray null return from
  `GetMeetingTalkbackController()` would otherwise null `m_ctrl` for the rest
  of the press. `onChannelUserLeaveResponse` decrements `present` for a
  CHANNEL-side removal (host action, Zoom-side eviction) — the mirror image
  of the join correlation, and previously an empty stub. **The deadline half
  of C1 needed its own test** (fix round 2, 2026-08-26): disabling only the
  timeout trigger (leaving the uid-left prune intact) left the suite green,
  because every other test happened to exercise expiry via a departure —
  the exact "unpinned backstop" shape that had already regressed twice in
  this milestone. Closed with a TEST-ONLY hook
  (`debug_expire_pending_invites_for_test()`, guarded by `m_chan_mtx` like
  everything else in that table) rather than sleeping `kAwaitTimeout` for
  real, and a scenario where the uid never leaves the roster so only a real
  timeout can free the name. `members_present_for_target()` and
  `session_start()` now share one `members_present_locked()` implementation
  instead of two hand-copied loops that could drift silently.
- **Engine teardown**: never let SDK callbacks race teardown; `set_terminate`
  is a bare `_exit(5)` (code 5 maps to EngineCrash recovery; no pipe writes,
  no locks, no allocation in the handler).
- **The operator surface, Task 5**: nothing on the plugin side could drive
  the engine's pre-provisioned channels until this task -- every key press
  refused `no_nomination` before it, because nothing ever sent
  `talkback_nominate`. The control API's `talkback_nominate` and
  `talkback_key` (now target-based: `"all"` or a nominee's name, not a
  participant to open a channel for) are **fire-and-acknowledge**, the same
  shape as `talkback_probe`: the plan outcome (channel count, who has a
  private channel, who is uncovered, who is unreachable) arrives
  asynchronously as `"cmd":"talkback_nominate"` stage lines
  (`ZoomEngineClient::handle_event()`, logged verbatim like `talkback_probe`)
  and is summarised for polling in `talkback_status`'s new `"nomination"`
  field -- there is no synchronous round trip, on purpose. `TalkbackController
  ::key_on()` refuses a target the last nomination's *reported* plan already
  proves cannot work (`talkback_target_known_unprovisioned()`,
  `src/talkback-plan.h`) BEFORE opening the tap, closing the same
  open-then-retract flicker window the engine-running/in-meeting checks
  already existed to avoid -- but this can only ever prove "known bad": a
  target whose channel is still mid-creation (`provisioning_incomplete`) is
  invisible to it, because the plugin is only ever told the *finished* plan,
  never the ladder's live progress. **Corrected in fix round 2 (N3):** that
  case does NOT fall through to `session_start()`'s async refusal unchanged
  -- it is refused LOCALLY too, from whatever the last CONFIRMED plan says
  (`src/talkback-nomination.h`). During a first-ever nomination's whole
  ladder `requested` is still empty, so every target (including `"all"`) is
  refused with "No one has been nominated yet"; during a re-nomination's
  ladder the previous plan's names still correctly fall through while
  brand-new names are refused as not-provisioned. `ZoomControlServer`'s
  socket handlers run on the Qt main
  thread (confirmed: `QTcpServer`/`QTcpSocket` are parented to the server,
  which is constructed on that thread, and `readyRead` is a plain
  `Qt::AutoConnection`) -- the same thread `TalkbackController`'s `QTimer`
  drives `evaluate()`/`key_off()` on, so `handle_line()` calling `key_on()`/
  `key_off()` directly (no dispatch) is correct, not a latent cross-thread
  bug; a hotkey or Companion surface added later must confirm it lands on
  that same thread before reusing this call shape.
- **The operator surface, Task 5 fix round 1**: the review's Majors were
  variations on one disease -- the plugin's local nomination record tracked
  what was **sent**, not what the engine had **confirmed**. F1: writing
  `requested` at `talkback_nominate()`'s send time meant a re-nomination the
  engine refused (`session_live`/`probe_busy`/`create_busy`/etc. -- seven
  paths, all of which leave the standing channel set untouched per
  `nominate()`'s own comment) still overwrote the plugin's record, so
  `key_on()` falsely refused a target whose channel was still standing --
  worse than no pre-check at all, on the operator's own mistake-recovery
  path. F2: nothing ever cleared the record at Leave/rejoin/engine restart,
  so it kept advertising a plan the engine had already destroyed, reopening
  the exact open-then-retract flicker the pre-check exists to close. Fixed
  by extracting the whole thing into `src/talkback-nomination.h` (pure,
  Qt/OBS-free, mirroring `talkback-plan.h`'s reason for existing): a
  `TalkbackNominationPlan` (the CONFIRMED record) is written ONLY by
  `talkback_nomination_commit()`, which fires on the engine's own
  `nominate_done` for an attempt that was never refused; a refusal
  (`talkback_nomination_note_refused()`) touches only diagnostic
  `last_attempt_ok`/`last_attempt_reason` fields, never the confirmed
  `requested`/`uncovered_private` `key_on()` reads. In-flight stage reports
  stage into a separate `TalkbackNominationPending` first. `talkback_nomination
  _reset()` is wired into existing per-meeting/per-process world-resets
  (`handle_event()`'s `"left"` branch; `stop_for_reconnect()` as of fix round
  2, see below) rather than a new hook. Both defects were mutation-tested: reverting
  either fix (`note_refused` touching `requested`; `reset()` as a no-op) in
  `src/talkback-nomination.h` fails `tests/talkback-nomination-test.cpp`
  deterministically, reverted cleanly afterward. Also this round:
  `talkback_nominate` now dedupes nominees plugin-side before recording them
  (F4, matching the engine's own dedup in `talkback_plan()`), acks
  `ok:false` when the engine pipe isn't running instead of silently dropping
  the command (F6), and `engine/src/main.cpp`'s `"participant"` fallback for
  `talkback_start` was deleted (F7 -- it said "delete once Task 5 ships" and
  Task 5 shipped). Left documented, not fixed: a nominee display name
  containing a control character (`\n`/`\r`/`\t`) desyncs the plugin's
  `requested` list against what the engine's naive line-oriented parser
  actually decodes (F5, `json_escape()` in `zoom-engine-client.cpp`) --
  narrow, and the shared decoder is not something to change opportunistically
  for one caller.
- **The operator surface, Task 5 fix round 2**: round 1's {confirmed,
  refused} model missed a THIRD engine outcome (N1, Major). `nominate()`
  destroys the standing provisioned set BEFORE planning a re-nomination
  (`nomination_destroy_provisioned()`, its own comment: "destroying is safe
  HERE and nowhere earlier"); if the ladder then aborts part-way
  (`nomination_create_next()`'s arbiter-busy check, or `CreateChannel`
  itself failing), the engine ends the attempt having ALREADY destroyed what
  it was replacing -- and the `CreateChannel`-failure branch reported only a
  diagnostic `"create_channel"` stage, never a terminal outcome, so the
  plugin's confirmed plan just sat there describing channels that no longer
  existed (a discarded `bool` at the `main.cpp` call site is how a
  no-terminal-report path could exist undetected at all). Fixed on both
  ends: the engine now emits `"nominate","ok":false,"channels_destroyed":true`
  on both abort branches -- a field, not a reason string, because
  `nominate()`'s OWN early gate reports the IDENTICAL `"create_busy"` reason
  for a refusal that does NOT destroy anything; reason strings collide, the
  field does not. The plugin maps `channels_destroyed:true` to a NEW
  transition, `talkback_nomination_note_failed_after_destroy()` (resets the
  confirmed plan like a world-reset, but keeps the reason as a diagnostic,
  unlike a bare `reset()`), never to `note_refused()`. N2/N4: the
  nomination-record reset for an engine restart moved from `start()` (which
  ran it BEFORE joining the previous session's reader thread -- violating
  the ordering rule stated three lines below it in that same function) into
  `stop_for_reconnect()`, the third world-reset point, which every restart
  path already calls AFTER joining both threads. N5, the structural finding:
  round 1's mutation tests covered only the pure state machine
  (`src/talkback-nomination.h`); both F1 and N1 were bugs in the WIRING
  (which report shape maps to which transition), which lived inlined in
  `handle_event()` and could not be reached by a host test. That mapping is
  now `talkback_nomination_apply_report()` in the new
  `src/talkback-nomination-dispatch.h` -- Qt-JSON-only, no OBS/socket/thread
  dependency, the same bar `tests/zoom-control-parse-test.cpp` already
  clears -- and `tests/talkback-nomination-dispatch-test.cpp` drives it with
  the exact report shapes the engine emits, including the
  `"create_busy"`-means-two-different-things case. Mutation-reproved: F1
  again (the refusal branch calling `commit()`), and the N1 mapping
  (ignoring `channels_destroyed` and always calling `note_refused()`), both
  caught by the new dispatch test where round 1's header-only test could
  not see them. N3: corrected the round-1 CLAUDE.md paragraph and a matching
  comment in `talkback-controller.cpp` that claimed a mid-ladder key press
  "falls through to `session_start()`'s async refusal, unchanged" -- it does
  not; it is refused locally too, from whatever the last confirmed plan says.

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

`talkback_nominate` (`{"cmd":"talkback_nominate","nominees":["Name", ...]}`,
requires an active meeting AND a running engine — acks `engine_not_running`
if the pipe isn't up) provisions channels for the given talent list — same
fire-and-acknowledge shape as `talkback_probe` above: the response only
confirms the trigger was accepted, and the plan outcome (channel count,
`all_talent_complete`, who is `uncovered_private`, who is `unreachable`)
arrives as `"cmd":"talkback_nominate"` log lines AND is polled via
`talkback_status`'s `"nomination"` field (which also derives
`has_private_channel` client-side, since the engine only ever names
shortfalls, never successes). That field is the **confirmed** plan only —
fix round 1 fixed a Major where it used to be written optimistically at send
time (see the CLAUDE.md entry above); `"last_attempt_ok"`/
`"last_attempt_reason"` describe the most recent nominate *attempt*
separately, which can disagree with the confirmed fields (e.g. a refused
re-nomination) without corrupting them. `talkback_key` now takes
`"target":"all"` or a nominee's display name (not `"participant"`) —
`{"state":"off"}` still always succeeds; `{"state":"on"}` is refused with a
specific message for a target the last *confirmed* nomination already
proves has no channel.

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
