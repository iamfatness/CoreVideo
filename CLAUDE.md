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
  until the live gate passes: the first-syllable claim is measured on the old
  path, not yet re-measured on the new one. **Gate run 1 (2026-08-26) failed
  before it could measure anything** — see the create-pacing entry below and
  `docs/superpowers/notes/2026-08-26-talkback-preprovisioned-live-gate.md`.
- **The nomination ladder must be PACED, and code 18 is a wait not a failure**
  (`kNominationCreateSpacing` / `nomination_tick()` in
  `engine/src/engine-talkback.cpp`, live gate run 1, 2026-08-26 20:04): Zoom
  rate-limits back-to-back `CreateChannel` calls. The ladder used to issue
  channel N+1 synchronously from inside channel N's `onCreateChannelResponse`
  — a 0 ms gap — and Zoom refused it with `SDKERR_TOO_FREQUENT_CALL` (enum
  position 18), so **no nomination with more than one channel could ever
  succeed live**, which is every real talent list. No unit test could catch it:
  the fake controller has no rate limit. The create is now scheduled 300 ms
  after the previous response and issued by `nomination_tick()`; a code-18
  refusal backs off (500 ms doubling, 4 retries per channel) and retries the
  SAME channel, and only cap exhaustion is terminal — with reason
  `create_rate_limited`, never the generic `create_channel_failed`, because
  run 1 spent its first pass suspecting permissions and channel budget.
  **`nomination_tick()` is not `tick()` and must never be folded into it**:
  `tick()` has exactly one driver, the thread `main.cpp` spawns when `probe()`
  returns true, which by construction does not exist during a nomination — a
  create scheduled there would never be issued, and if it were it would run on
  the probe's thread, breaking both the command-loop-thread rule `CreateChannel`
  lives under and fact 2 of `tick()`'s batch-destroy chain. The pump rides the
  command loop's own 50 ms idle turn instead (`on_idle` in
  `ipc_read_line_with_message_pump()`). The arbiter claim is taken at ISSUE,
  not held across the wait — a scheduled create is not outstanding, and
  claiming early would arm `kAwaitTimeout` against a request Zoom has never
  seen, so the spacing wait would self-expire the ladder it is pacing. The
  ~300 ms window that opens is closed where it matters: `nominate()` refuses
  `create_busy` while a create is *scheduled* as well as outstanding, or a
  re-nomination landing in that window would destroy a running ladder's
  channels and leave it with no terminal report — the one rule the whole abort
  machinery exists to hold. That gate was found by mutation, not by review.
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
  on both of `nomination_create_next()`'s SYNCHRONOUS abort branches --
  **incomplete, see fix round 3 below: three more ASYNC abort branches had
  the identical gap** -- a field, not a reason string, because
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
- **The operator surface, Task 5 fix round 3**: round 2 fixed N1 on the two
  branches the review named by line number and not on the disease.
  `nominate()`'s ladder can also abort ASYNCHRONOUSLY, after `nominate()`
  itself has already returned true: `onCreateChannelResponse`'s
  `channel_failed` (a genuine Zoom-side rejection -- budget past 16 channels,
  permission, transport -- and the LIKELIER real-world failure, since
  `CreateChannel()`'s synchronous return mostly just validates arguments),
  `channel_stale` (a late response for an already-abandoned create), and
  `handle_expired_create()`'s Nomination arm (a swallowed response, self-healed
  lazily by a later `nominate()`/`probe()` call) all clear the queue, destroy
  what was provisioned, and reported only a diagnostic stage -- N6, the same
  Major, on three more structurally identical branches. Ruling: stop patching
  branches one at a time and make the omission inexpressible. Every one of
  these paths (or "its queue-clearing sibling", `channel_stale`'s narrower
  one-channel destroy) now funnels through ONE new function,
  `EngineTalkback::nomination_abort_ladder(reason)`
  (`engine/src/engine-talkback.cpp`): clear the queue, destroy what's
  provisioned, THEN report `"nominate","ok":false,"channels_destroyed":true`
  -- a sixth future abort branch cannot skip the report without also skipping
  the teardown, because there is no longer a separate step to forget. The
  engine-side pin the round-2 re-review found totally missing ("nothing pins
  the engine side... mutant (c) survives") is a new `EngineIpc::test_sink()`
  hook (`engine/src/engine-writer.h`, TEST-ONLY, no production call site) that
  makes a `report_nomination()` line observable at all from
  `CoreVideoEngineTalkbackSelectTest`, plus a sibling of
  `debug_expire_pending_invites_for_test()` for the CREATE-side deadline
  (`debug_expire_pending_create_for_test()`) so the swallowed-response
  self-heal can be driven without a real 10s wait. Three new tests drive the
  synchronous `CreateChannel()` failure, the async `channel_failed` response,
  and the self-healed swallowed create -- all three fail if
  `nomination_abort_ladder()`'s report is deleted OR if a call site bypasses
  it entirely (both mutation-proved). `channel_stale` was wired into the same
  function per the ruling and per this file's own standing policy (never
  assert a branch unreachable -- two Majors have lived behind exactly that
  claim in this feature already) but is NOT covered by a passing/failing
  test: reaching it requires `onCreateChannelResponse` to see
  `owner==Nomination` with a stale `outstanding_generation` simultaneously,
  and every code path that bumps the generation (`talkback_new_ladder()`,
  gated on `owner==None`; `talkback_expire()`, which sets `owner=None` in the
  same transition) appears to make that combination unreachable through the
  exposed engine API as the code stands today -- consistent with, not
  contradicting, the file's refusal to assert it can't happen. N7 (this
  round's own regression): `stop_for_reconnect()` is not on every path to a
  fresh `start()` -- `monitor_loop()` declining recovery (policy disabled,
  auth failure, max attempts, no stored session) and
  `fail_after_init_retries_exhausted()` both clear `m_running` directly, and
  the operator's next manual dock Join calls `start()` with nothing in
  between, reopening F2/N1's exact symptom on a third trigger. Restored
  `start()`'s own reset -- AFTER its joins this time (N2's actual bug was the
  position, not the existence, of that reset) -- alongside
  `stop_for_reconnect()`'s; both are idempotent so keeping both costs
  nothing. N8: fixed the third surviving copy of the stale "falls through to
  `session_start()`'s async refusal" claim, in `src/talkback-plan.h`'s own
  header comment (N3 had corrected CLAUDE.md and `talkback-controller.cpp`
  but missed this one, which the controller comment pointed straight at).
- **The operator surface, Task 5 final whole-branch review (2026-08-26)**:
  both Criticals lived in the SEAMS between tasks -- state each task got
  right alone, wired together in an order no single task owned. **C1: a
  nomination attempt had no identity on the wire.** The plugin stages an
  in-flight attempt in ONE slot, reset at SEND time, so a re-nomination sent
  while an earlier ladder was still provisioning wiped the earlier attempt's
  staging -- and the earlier ladder's own `nominate_done` then committed the
  LATER attempt's nominee list against the EARLIER ladder's channels. Not
  exotic: the engine refuses the second attempt with `create_busy` and
  correctly leaves the first ladder running, so EVERY mid-ladder
  re-nomination took this path. Both directions of `key_on()`'s pre-check
  failed at once (a standing channel refused locally -- F1's exact symptom
  through a third door; an unprovisioned name passing the check, opening the
  tap and retracting) and the first attempt's `uncovered_private`/
  `unreachable` names silently vanished from the polled `talkback_status`
  field. Fixed with an explicit echoed id, NOT an ordering argument: the
  plugin stamps a process-wide monotonic `"attempt"` into every
  `talkback_nominate` request (`ZoomEngineClient::m_talkback_nominate_attempt`
  -- never reset by a world-reset, because a re-usable id is an id that can
  match across one), the engine carries it in `m_nomination_attempt` and
  echoes it in every TERMINAL report for that attempt (the seven early
  refusals, `nomination_abort_ladder()`'s report, `nominate_done`,
  `malformed_nominees`), and `talkback_nomination_apply_report()` acts only
  on a matching attempt. A FIFO of staged attempts was rejected -- this
  milestone already shipped a Critical out of an un-popped queue. Refusals
  carry the id of the attempt BEING REFUSED, never the running ladder's.
  Wire compatibility is a stated choice, not an accident: a report with NO
  `"attempt"` field is treated as MATCHING, because an engine older than this
  fix emits none and a DLL-only install is this project's canonical mistake;
  attempt 0 (a raw-pipe caller) suppresses the field entirely so those
  reports are byte-identical to a pre-fix engine's. A superseded terminal
  never commits; the two shapes that PROVE the channel set moved
  (`nominate_done`, and any abort carrying `channels_destroyed:true`)
  invalidate the confirmed plan instead (`talkback_nomination_note_
  superseded()`), which fails closed -- every target refused with "no one has
  been nominated yet" until a re-nominate, recoverable in one command --
  rather than leaving the record permanently describing channels the
  superseded ladder's own replace step destroyed. **C2: a ladder abort
  destroyed the channels a LIVE key was talking on and nothing un-lived the
  session.** Keying `"all"` mid-ladder is legal by design (`session_start()`
  gates only on `still_coming` for its OWN target), so a later
  `channel_failed` -- the LIKELIER real-world failure -- reached
  `nomination_abort_ladder()`, which batch-destroyed every channel and
  cleared `m_session_channels` underneath the press. `report_session_state()`
  had eleven call sites and every one was PRE-live, so `m_session_live`
  stayed true, the plugin's `TalkbackSessionStatus.live` stayed true,
  `evaluate()` saw nothing to close: key open, cue played, tally red, zero
  audio. The indivisible teardown now has a THIRD responsibility -- decide
  under `m_chan_mtx` (before the destroy, which clears the selection) whether
  the live session's channels intersect what is about to go, then
  `session_stop()` BEFORE the destroy (so the duck is restored on channels
  that still exist) and `report_session_state(false, "channels_destroyed")`
  after it, both outside the lock. The plugin half already existed: the
  `explicit_failure` path in `evaluate()` closes any `live:false` with a
  non-empty reason -- that rule is now
  `talkback_session_state_closes_key()` in `src/talkback-key.h` so a host
  test can actually pin it, which nothing could while it sat inline in a file
  needing libobs and Qt to compile. **M1: `present` could hold a dead uid.**
  `TalkbackProvisionedChannel::present` stores a `user_id` but the roster
  diff matched by NAME only, so a leave+rejoin under a new id with no
  resolution in between left `present_here` and `was_present` both true --
  no departure, no re-invite, ever, while `members_present` counted the dead
  id and told the director "1 of 1 present" for someone who hears nothing.
  Now pruned by uid, mirroring the pending-invite prune, BEFORE the per-name
  diff so the same pass re-invites the new id. The `has_pending_work()` gate
  that used to drop the WHOLE resolution is **narrowed, not hoisted**: the
  roster snapshot, both prunes and the departure diff now run regardless
  (they touch only this file's tables and call no talkback SDK API; reading
  the participants list is not new exposure -- `rebuild_roster()` already
  does it on every one of the same callbacks), and only the invite issuance
  is still gated. Its old comment ("a refusal here costs nothing but a delay
  ... the next roster event gets another chance") was true of invites, whose
  state persists, and FALSE of departures, which are edges against a live
  snapshot: a refusal did not delay that edge, it destroyed it, for up to the
  ~30s a probe runs. Three comment-lies fixed alongside (this feature is at
  seventeen findings): the `channel_stale` directive that forbade touching
  `m_provisioned_channels` three lines above a call that correctly does;
  `report_session_state`'s doc in BOTH `engine-talkback.h` and the
  `open_audio()` copy, which attributed session-state/"live" to
  `onCreateChannelResponse` -- false since Task 3, and the belief that hid
  C2; and `nomination_destroy_provisioned()`'s "every caller has already
  ruled out a live key press", which was C2's enabling belief. All three
  fixes mutation-proved: dropping the id check in `apply_report()`, removing
  the un-live step from the teardown, and removing the uid prune each fail
  their own test deterministically.
- **...and its verification round**: one real finding, and it was a test that
  could not fail. The superseded-`nominate_done` case fed a
  DEFAULT-CONSTRUCTED plan, so `requested` was already empty and `done`
  already false -- every assertion passed on the early return alone, and
  deleting `talkback_nomination_note_superseded()` left 67/67 green. The
  superseded-ABORT case beside it was genuinely pinned only because
  `committed_baseline()` gave it something to destroy. Same shape as the
  round-2 deadline backstop and the round-3 "nothing pins the engine side":
  **a fail-closed invalidation is unpinnable against an already-empty
  record** -- give it something to invalidate or it asserts nothing. Also
  this round: the staging stage lines (`uncovered_private`, `unreachable`,
  `plan`) now carry the attempt id too, not just the terminals. The first
  version omitted them behind a circular justification ("the mapping ignores
  unmatched stage lines anyway" -- no stage line COULD be unmatched, because
  that decision was why none carried an id), and the omission was a real if
  narrow hole: two nominates can sit in the pipe before the engine reads the
  first, so the plugin can already have staged the second when the first's
  stage lines arrive, folding one attempt's shortfall names into the other's
  record -- and `uncovered_private` is read by
  `talkback_target_known_unprovisioned()`, so a spurious name there refuses a
  key on a standing channel. The real rule is now stated: the id goes on
  everything the plugin's state machine CONSUMES (three staging stages + all
  terminals) and nothing else. Two limits documented rather than fixed:
  `pc.failed` holds NAMES with no uid, so the M1 uid prune cannot mirror into
  it -- a permanently-failed invite whose talent rejoins under a new uid with
  no observed absence is not retried for the rest of the meeting, which stays
  HONEST ("0 of 1", since they were pruned out of `present`) and is
  recoverable by re-nominating; and `"attempt"` is emitted BEFORE `"nominees"`
  because `json_uint()` is a first-match scan, not a parser.
- **The dock keys, by owner decision (Milestone 7)**: the spec
  (`docs/superpowers/specs/2026-08-24-zoom-talkback-design.md`) locks keying to
  Companion/control API/hotkey and says "**Not** the dock"; the owner has since
  asked for a drivable dock, so CoreVideo's own **Talkback dock**
  (`src/zoom-talkback-panel.cpp`, registered as `ZoomTalkbackDock` alongside
  ISO Recorder / Output Manager / Diagnostics) nominates and keys — a
  deliberate, approved spec deviation, with everything else
  unchanged (identity by display name, keying SELECTS a provisioned channel,
  every refusal fails closed). Its decisions live in the Qt/OBS-free
  `src/talkback-dock-state.h` (pinned by `CoreVideoTalkbackDockState`) because
  the two Majors this feature shipped both lived in wiring no test could reach.
  Dock keys pass **`needs_renewal = false`** — `src/talkback-key.h`'s rule for
  a surface whose release is in-process and reliable: press/release are Qt
  signals on the same main thread the controller's `QTimer` runs `evaluate()`/
  `key_off()` on, with no transport to drop them, and demanding a heartbeat
  from a UI-thread timer instead would close a genuinely held key on any ~1s
  OBS UI stall. A release can still vanish in-process in SEVERAL ways —
  `QAbstractButton` drops `down` without emitting `released()` on an
  `EnabledChange`, on any non-popup focus loss, and anywhere `setDown(false)`
  is reached — so `talkback_dock_release_lost()` is cause-agnostic by design:
  it asks the widget whether it is still down, never why, and closes any
  dock-owned PTT key that is not. It runs on the existing 100 ms tick (no
  second timer), and reading the widget rather than a deadline is what makes it
  unable to false-close during a stall. It is load-bearing, not a second
  opinion — "the dock never disables a held button" closes one cause, not the
  class. **Fix round 1 (M1, Major)**: the latch toggle-off used to read the
  Latch CHECKBOX while the release path read the mode captured at the press, so
  unchecking Latch while a latched key was live left it un-closeable from the
  dock — `key_on()` answered "already open" and `released()` bailed out, with
  the director still live to talent. One record now answers all three
  questions (press / release / backstop): `TalkbackDockOpenKey`, carrying the
  mode the open key was OPENED with. Buttons also refuse while another surface
  holds the key (m3) and while no talk source is chosen (m4), instead of
  offering a press that can only refuse; the source scan is gated to 1 Hz and
  to the dock being visible (m2), since `obs_enum_sources()` holds libobs's
  source mutex.
- **...and it is its OWN dock, after the first live render (Milestone 7,
  re-home)**: it shipped as two group boxes at the bottom of the Zoom Control
  dock, under Join / Engine / Routing, and the owner's verdict was "the UI is
  not good... it needs to be its own panel". Five defects, all confirmed on
  screen: the LIVE state — the single most important fact on the surface — was
  one line of small red text UNDER the buttons; the roster rendered as a
  stretched blue selection bar that read as a mis-styled button; the
  program-track advisory was a paragraph of amber prose beside a combo; nothing
  had hierarchy (Nominate was a full-width blue slab while the key buttons
  looked secondary); and the copy carried literal `--` runs and quoted names.
  The re-home is **layout only** — every rule listed above (the single
  `TalkbackDockOpenKey`, the cause-agnostic `talkback_dock_release_lost()`,
  `needs_renewal = false`, the enablement rules, the 1 Hz + visibility-gated
  source scan, the `(present, name)` roster-signature diff with check-state
  reapply, keying on the Qt main thread) moved across unchanged, and the tick
  it all rides is the panel's own 100 ms `QTimer` because the backstop's
  resolution IS that interval. What changed: an **ON AIR banner** at the top
  (`talkback_dock_banner()`, which REPLACED `talkback_dock_tally()` — same four
  states, same "live means the ENGINE confirmed it" rule, same echoed recovery
  hint, rendered as a full-width strip instead of a caption); a `short_text`
  on the track warning with the paragraph demoted to its tooltip; name lists
  elided past `kTalkbackDockNameListMax` = 5 with the FULL list in
  `TalkbackDockNominationReport::tooltip`, so eliding never costs a name the
  reporting chain exists to say out loud; the nominee list with selection
  switched off entirely (selecting a row of tick boxes means nothing, and the
  highlight was the thing impersonating a button); and the Milestone 1 probe
  moved across too, collapsed, at the bottom. Six mutations were run against
  the new pins in `tests/talkback-dock-state-test.cpp` (banner ignoring engine
  confirmation, elision disabled, tooltip carrying the elided list, short
  status reverting to the paragraph, the raw `"all"` sentinel leaking to the
  banner, a failed closed key shown as clean idle) and all six were killed.
  `src/cv-combo-utils.h` is new: the combo helpers and `participant_label()`
  the two docks now share, extracted rather than copied because
  `replace_combo_items()` BLOCKS the combo's signals while it rebuilds — a
  hand-copied second version that forgot to would rewrite the saved setting on
  every refresh tick. **Fix round:** the probe's roster poll was the one thing
  that did not get the visibility discipline across the re-home — it copied the
  whole roster and rebuilt a combo at 10 Hz for a section that is folded by
  default, on a dock created at `FINISHED_LOADING` whether or not anyone opens
  it. It is now gated exactly like the source scan (visible **and** unfolded,
  then 1 Hz), and `TalkbackProbeExpanded` is persisted **because** folding it
  is what turns that work off, not merely which way an arrow points. Also:
  `keyed="true"` is now gated on the banner saying Live **and** on
  `dock_owned`, so another surface's key can no longer paint our own disabled
  button red (red means the director is audible, on the button exactly as on
  the banner); the held-button tooltip is written before the never-disable
  guard instead of after it; and `rebuild_key_buttons()` hides before
  `deleteLater()`.

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
