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

## macOS (Apple Silicon)

Reunified 2026-08-20 (merge of the `mac-port` line). Same two-process design;
the differences that matter: IPC rides Unix sockets (`/tmp/ZoomObsPlugin_*.sock`)
instead of named pipes; the engine is `ZoomObsEngine.app` inside the plugin
bundle with the Zoom SDK copied into its `Contents/Frameworks` (the SDK loads
sibling bundles from there — rpath does not work); install with
`scripts/make-macos-bundle.sh --build-dir <dir> --link-sdk --install`, never by
hand. POSIX shm names are collapsed to 20 chars on Apple (`shm_platform_name`,
PSHMNAMLEN=31) and a shm object is **sized exactly once** (ftruncate on an
existing object is EINVAL — create-over-held fstats and recreates instead).
The engine's SDK-facing rules are documented in `engine/src/main-macos.mm`:
never subscribe to the local user's own share, detach `renderer.delegate`
before `destroyRender:` (synchronous callback re-entry deadlocked the main
thread for hours), frame callbacks `try_lock` and drop rather than block SDK
threads, heartbeat pings route through the main queue only after
`initSDKWithParams` returns. `ShmAudioSlot::capture_ns` crosses processes on
DIFFERENT clocks on macOS (see the struct comment) — `audio_latency_us` is
untrustworthy there until that is reconciled. Kill a mid-meeting engine only
after checking `{"cmd":"status"}` — it IS the meeting session.

The macOS meeting-status callback must emit `awaiting_admission` on every SDK
status change. `ZoomSDKMeetingStatus_WaitingForHost` and
`ZoomSDKMeetingStatus_InWaitingRoom` map to `active:true`; every other status
maps to `active:false`. The dock holds its existing 120-second join watchdog
while that flag is true and starts a fresh full window after Zoom advances.
Keep the symbolic mapping in `engine/src/macos-admission-state.h`, where the
SDK-backed `CoreVideoMacosAdmissionState` test compiles it against the installed
framework. The real callback calls that header's dispatch seam inside the
fresh-callback/epoch gate; the regression records the seam's outgoing event,
asserts it precedes normal status handling, and drives the watchdog with it.

**Talkback does not exist on macOS**, and the dock says so rather than failing
quietly. `engine-talkback.cpp` is in `ENGINE_SOURCES`, which only the Windows
engine target uses; the macOS engine is `main-macos.mm` and never compiles it.
The dock is cross-platform and builds either way, so without a gate it is a
panel whose every control sends a command nothing answers. One constant,
`kTalkbackPlatformSupported` (`src/zoom-talkback-panel.cpp`), feeds
`TalkbackDockSessionView::platform_supported`,
`TalkbackDockKeyContext::platform_supported`, and the Assign/probe buttons'
own `setEnabled`. Three rulings worth keeping. The `#if defined(__APPLE__)`
lives at that ONE call site and the decision crosses into
`talkback-dock-state.h` as a plain bool, because that header is Qt/OBS-free and
compiles everywhere — so the macOS rendering is pinned by a Windows or Linux CI
run, which is the only way a macOS-only branch is tested by anything this
project runs. `TalkbackDockBannerState::Unavailable` is checked FIRST in
`talkback_dock_banner()` and RETURNS, which is what keeps the "coming to macOS"
wording out of the ON AIR strip structurally — with no talkback engine nothing
can key, so nothing can be live, and Unavailable and Live are unreachable
together by construction rather than by promise. In the key chain it sits
directly below `held_here` and nowhere else: never disabling a button the
operator is holding is the stronger law, and honouring it here costs nothing,
because there is no key to hold. The layout instrument
(`COREVIDEO_TALKBACK_LAYOUT_TEST`) is deliberately NOT gated — its job is to
render every state including the tallest live banner, and gating it would
collapse it to the Unavailable strip on the very platform a developer is most
likely running it on. Mutation-proved in
`tests/talkback-dock-state-test.cpp`: disabling either half of the gate fails
its own assertions, and a default-constructed context must stay supported or
the gate turns the feature off on Windows.

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

To look at the **Talkback dock's layout** without a meeting, launch OBS with
`COREVIDEO_TALKBACK_LAYOUT_TEST=8` set: the dock fills itself with a fake cast
covering every cell state and talks to no engine at all (see the talkback
invariants below). Vertical layout on this dock is verified **only** this way —
an offscreen Qt harness has certified it three times and been wrong three times.

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
- **Loudness coefficients follow the RUNTIME sample rate, never 48 kHz**
  (`src/audio-loudness.h`, feat/panelist-feedback): BS.1770-4 publishes its
  two K-weighting biquads' coefficients for 48 kHz and no other rate, and
  this plugin has no guaranteed rate -- Zoom commonly delivers 32 kHz. The
  coefficients are DERIVED from the analog prototype at whatever
  `loudness_meter_configure()` is called with; at 48 kHz that derivation
  reproduces the published table to fourteen digits, which is what the
  engine's tests assert. Pinned at 48 kHz and fed 32 kHz, a 1 kHz tone
  whose true value is -19.98 LUFS reads -18.66: 1.3 LU wrong, on a meter
  whose whole product claim is that a 6 LU spread between panelists is
  visible, and with nothing in the number to say it is wrong. The
  gated-integration window carries the same fragility one level up:
  `loudness_meter_clear_window()` is shared, on purpose, by
  `loudness_meter_configure()` and `loudness_meter_reset_window()` so a
  mid-stream format renegotiation (Zoom renegotiating, or a Mix/Isolated
  role flip changing channel count on the same subscription) cannot leave
  blocks measured under the OLD coefficients sitting in the same check
  window as blocks measured under the new ones -- do not let those two
  call sites diverge.
- **Integrated loudness is gated, and the gate is load-bearing, not an
  optimisation** (`audio-loudness.h`'s two-pass integration: an absolute
  gate at -70 LUFS, then a relative gate at -10 LU below the
  absolute-gated mean, over 400 ms blocks at a 100 ms hop): a panelist is
  silent roughly 80% of a preshow, and ungated silence pulls the mean down
  hard -- 4 s of -20 LUFS speech inside a 20 s window reads -27.08 LUFS
  ungated and -20.16 gated, and -27.08 is not a usably-wrong number, it is
  a differently-shaped one that would fail every panelist on every panel.
  The board's reference is the panel **MEDIAN** of gated integrated
  loudness, never the mean, and only panelists who have cleared the
  minimum gated block count vote on it -- one laptop mic sitting at -35
  LUFS should not get to drag the reference far enough to fail everyone
  else, and a panelist who hasn't spoken yet should not vote at all.
- **The readiness board's `kLoudnessBoardMinRowPx` is a SLOT PITCH, not a
  drawn row height** (`src/loudness-board.h`): `loudness_board_visible_rows()`
  divides available body height by it to decide how many rows fit; the row
  actually drawn is that slot minus `kLoudnessBoardRowGapPx`. The test
  asserts `last.h + kLoudnessBoardRowGapPx >= kLoudnessBoardMinRowPx`
  specifically because `last.h >= kLoudnessBoardMinRowPx` is unsatisfiable
  for any positive gap -- a future "simplification" to the un-added form
  is a regression, not a cleanup. Label refresh on the same board is gated
  on BOTH `model.signature` changing AND `shown` (the visible row count)
  changing, because the signature encodes reference/names/statuses/
  deviations but not how many rows the current canvas height reveals: a
  signature-only gate leaves rows newly exposed by a resize blank
  indefinitely whenever the panel's content hasn't otherwise moved --
  worst during a silent preshow, which is the normal state, since every
  parked row reads identically. The board's own 10 Hz rebuild
  (`meter_video_tick`) is gated on `obs_source_showing()` for the same
  reason as the Talkback dock's roster poll: a meter parked on an unused
  scene should not pay for `g_sources_mtx` plus every live source's mutex
  ten times a second forever. The accumulator SUBTRACTS the 100 ms
  interval rather than zeroing on fire -- zeroing discards the remainder
  that pushed a tick over threshold, which at 60 fps lands every 7 frames
  (~117 ms, ~8.6 Hz) instead of the documented 10 Hz.
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
- **Speaker-director time is monotonic under its mutex**
  (`src/speaker-director.cpp`): callers sample `os_gettime_ns()` before taking
  the director lock, so a contending callback can arrive with an older sample.
  Every time-mutating entry point clamps that sample to the newest accepted
  director time before changing candidate, hold, vacancy, or manual-take
  clocks. Never subtract an unguarded caller timestamp from those clocks;
  unsigned wrap can otherwise satisfy both sensitivity and hold immediately.
  Actual promotions retain a bounded, ID-only attribution history and the dock
  logs it outside the director mutex; keep names and callbacks out of that
  locked path.
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
- **Channel membership must be acoustically NEUTRAL until keyed** (live
  production, 2026-08-29: talent reported their meeting audio ducking the
  moment they were **assigned** to a talkback channel, before any key was
  pressed). Nothing in the engine ducks at provision — the key-down duck is
  armed by `session_start()` and applied on the first `drain_audio()`, and
  restored by `session_stop()` — so the attenuation was **Zoom's own default
  for a channel member**: Zoom appears to create channels already ducked,
  treating membership as "about to be talked to". That silently voids the
  premise the whole pre-provisioned architecture rests on (channels stand for
  the length of the show, so the key press pays for nothing): a standing
  channel became a standing duck, for every nominee, for the whole show. The
  member state is now **deterministic instead of inherited** — every channel
  gets an explicit `SetChannelBackgroundVolume(channel, kBackgroundNeutral)`
  in `onCreateChannelResponse`'s nomination branch, on the command-loop
  thread, outside `m_chan_mtx`, **before any invite is issued** for it (a
  member invited into a channel still at Zoom's default hears the duck for the
  length of the gap), reported once per CHANNEL as
  `"stage":"background_volume_neutral"` — never per member, which on a
  13-channel plan is the message-storm shape this codebase already has a live
  incident about. `kBackgroundNeutral` = **1.0**: the SDK header
  (`meeting_talkback_ctrl_interface.h`) documents the parameter as the main
  meeting audio volume people in the channel hear, range 0.0–2.0, "decrease
  … to hear the channel audio more clearly" — a gain whose midpoint 1.0 is
  unity, i.e. the meeting exactly as everyone outside the channel hears it.
  **One set at creation is the whole contract**, and that is an argument from
  the SDK's shape, not an experiment: the setter is keyed by `channelID`
  ALONE, there is no user parameter and no per-member variant anywhere in
  `IMeetingTalkbackController`, so volume is a property OF THE CHANNEL that a
  member invited later by `resolve_roster_change()` inherits — there is no
  per-member state to re-assert and no API with which to assert it, which is
  why nothing in `onChannelUserJoinResponse` touches volume. The keyed cycle
  is unchanged and now writes the same two named constants around it (duck
  `kBackgroundDucked` = 0.3 on the first drain, restore `kBackgroundNeutral`
  on `session_stop()`) — the restore writes THE CONSTANT, never a value cached
  from before the duck, which became load-bearing the moment Zoom's default
  turned out to be ducked itself: "restore what it was" would hand talent back
  the duck and make idle-after-a-key differ from idle-before-the-first.
  Mutation-proved in `tests/engine-talkback-select-test.cpp` (deleting the
  provision-time set fails six assertions; restoring to a non-neutral value
  fails two) and reverted clean. The probe is unchanged — it ducks for its
  three-second tone and destroys its channel, self-contained. **Not yet
  confirmed live**: this is written from the operator's report plus the
  absence of any duck-at-provision in our own code; it needs the next
  production to confirm talent are no longer ducked on assign.
- **THE THREE TALKBACK DELIVERY LAWS** (ported 2026-08-29 from the sibling
  ZComms project, which spent that day live-hunting talkback delivery failures
  against the same Meeting SDK 7.1.5; its writeup is in
  `C:\Users\walla\ZComms\CLAUDE.md` under "The talkback delivery laws"). **None
  of the three is documented by Zoom** and each one is silent or indefinite in
  the failure direction, which is why they are laws here and not TODOs.
  1. **Talkback delivers ONLY while this client's own meeting audio is OPEN.**
     Muted, `SendAudioDataToChannel` is *accepted* — success codes, members
     confirmed, zero failures — and every member hears silence. The operator's
     own production on 2026-08-29 had the bot muted by the host, so every send
     would have been that accepted-but-silent ghost.
     `EngineTalkback::ensure_mic_open()` reads the authoritative state
     (`GetMySelfUser()->IsAudioMuted()` — `IMeetingAudioController` has **no**
     "am I muted" getter, only `MuteAudio`/`UnMuteAudio`) and unmutes at
     `session_start()`; `mic_tick()` re-asserts every 2 s on the command loop's
     idle turn, because a host can re-mute the bot mid-key.
     `restore_mic_state()` puts it back on release **iff** this file was the
     one that opened it — a bot left hot on the machine running the show is
     the worse failure. An unmute the meeting refuses does **not** refuse the
     key (the channels are real and a host can still unmute); it reports
     `"mic":"blocked"` on the `talkback_session` `live` line, and the dock's
     banner says **"ON AIR - BOT MUTED: \<target\>"** in amber on the live red
     instead of plain "live", which is the one word that made the ghost
     invisible. **The whole chain is named because the first version of this
     entry claimed it and it did not exist** (fix round 1, M1 — the engine
     emitted the field, three comments and this file asserted the dock
     consumed it, and the plugin's parser read only `live`/`reason`):
     `handle_event()`'s live-line branch → `talkback_session_mic_blocked()`
     (`src/talkback-key.h`, beside `talkback_session_state_closes_key()`, and
     extracted for the same reason — both Majors this feature shipped lived in
     wiring no host test could reach) → `TalkbackSessionStatus::mic_blocked` →
     `TalkbackDockSessionView::mic_blocked` → `TalkbackDockBannerState::
     LiveMicBlocked`. Three consequences that are rulings, not accidents: the
     member tally is **dropped** from that headline ("3 of 3 present" beside
     "nobody can hear you" is the instrument that made the ghost look
     healthy); the tally dot and the red CELL are withheld, because this
     dock's standing rule is *red means the director is audible* and they are
     not; and `mic_tick()` **re-emits the confirmed-state line on the EDGE**
     (M1b — it used to report only a stage line, so a host muting the bot at
     second 30 of a latched key left the stored `mic` at "open" for the rest
     of the key). Edge, never every tick: a latched key re-asserts every 2 s
     and the plugin's handler takes a mutex per line. `m_mic_open` exists to
     be that comparison — before M1b it was written in six places and read in
     none, under a comment describing the code above.
     **The leak question, answered from the code rather than assumed**: `Join`
     sets `isAudioOff = false` / `isMyVoiceInMix = true`
     (`engine/src/main.cpp`) and **nothing in this repository calls
     `setExternalAudioSource()`** — the only `IZoomSDKAudioRawDataHelper` use
     anywhere is `engine-audio.cpp`'s `subscribe()`/`unSubscribe()`, the
     *receive* path — so a bare unmute would open the OBS machine's **default
     capture device** live into the meeting. The insurance runs once at auth,
     in `main.cpp`'s existing `CreateSettingService` block (stage
     `mic_insurance`): `SelectMic()` onto a device id that matches nothing plus
     `SetMicVol(0)`. **Weaker than ZComms's** never-fed virtual mic, and
     deliberately so — theirs installs a virtual mic into the same helper our
     show-critical receive subscribe uses. If a live gate ever hears the room
     through this, `setExternalAudioSource()` is the escalation.
  2. **The rate limit is per membership CALL, and invites count** — see the
     next bullet, which this rewrote.
  3. **A same-account host collision hangs the join forever unless answered.**
     Joining with a ZAK for an account already hosting elsewhere (the
     operator's own client in their own PMI — the ordinary way anyone tests)
     does not fail the join: the SDK asks, via
     `IMeetingConfigurationEvent::onEndOtherMeetingToJoinMeetingNotification`,
     whether to end the other meeting. **Unanswered, `Join()` never resolves
     and no `MEETING_STATUS_FAILED` ever arrives** — the dock sits on "joining"
     until someone kills the process. This is the 2026-08-25 displacement
     class. `EngineMeetingEvent` now implements `IMeetingConfigurationEvent`
     (registered via `GetMeetingConfiguration()->SetEvent()`, a **separate**
     registration from `IMeetingService::SetEvent()`) and answers `Cancel()` —
     **never `EndOtherMeeting()`**, which would end the operator's live show to
     join it — then fails the join loudly through the existing
     `{"cmd":"error","msg":"meeting_failed"}` shape with local sentinel code
     `909001` and reason `account_busy_elsewhere` (Zoom never failed the join,
     we did, so there is no Zoom enum for it; the number matches ZComms's so
     two projects' logs read alike).
- **The membership ladder must be PACED — creates AND invites out of ONE
  budget — and code 18 is a wait not a failure**
  (`kMembershipCallSpacing` / `nomination_tick()` in
  `engine/src/engine-talkback.cpp`; live gate run 1, 2026-08-26 20:04, then
  **Law 2**, ZComms live 12-person meeting 2026-08-29): Zoom
  rate-limits back-to-back `CreateChannel` calls. The ladder used to issue
  channel N+1 synchronously from inside channel N's `onCreateChannelResponse`
  — a 0 ms gap — and Zoom refused it with `SDKERR_TOO_FREQUENT_CALL` (enum
  position 18), so **no nomination with more than one channel could ever
  succeed live**, which is every real talent list. No unit test could catch it:
  the fake controller has no rate limit. The create is now scheduled after the
  previous response and issued by `nomination_tick()`; a code-18
  refusal backs off (500 ms doubling, 4 retries per channel) and retries the
  SAME channel, and only cap exhaustion is terminal — with reason
  `create_rate_limited`, never the generic `create_channel_failed`, because
  run 1 spent its first pass suspecting permissions and channel budget.
  **LAW 2 (2026-08-29) widened this from creates to every membership call.**
  ZComms measured Zoom refusing code 18 on **invites**, on every pass, while
  making no creates at all; their working cadence is **one membership call per
  ~600 ms, round-robin across channels**. Our ladder paced creates and fired
  invites *unpaced*, in bursts — every member of a channel back to back inside
  `onCreateChannelResponse`, and every re-resolved name at once from
  `resolve_roster_change()`. Two channels passed the 2026-08-26 gate because
  two channels is two creates and two invites; a 24-talent plan is 13 creates
  and 24+ invites and would have tripped exactly what ZComms measured. So
  `kNominationCreateSpacing` (300 ms, creates only) became
  **`kMembershipCallSpacing` (600 ms, shared)**: both call kinds queue,
  `nomination_tick()` spends **at most one call per turn** against one floor
  (`m_membership_next_at`), creates take priority within a turn (a channel that
  does not exist cannot be invited into), and invites round-robin away from the
  last channel served — at one call per 600 ms, FIFO order decides whether the
  last talent's own private channel is confirmed at second 2 or second 20, and
  it is the private channels the director keys. Invites got the create's
  code-18 treatment too: requeued with a doubling backoff
  (`invite_rate_limited_retry`), capped per (channel, name), and **cap
  exhaustion is NOT terminal for the ladder** — one person's membership is not
  the nomination's progress. **The cost is stated, not hidden**: a 13-channel /
  24-invite plan now takes ~22 s of otherwise-idle wall time to fully provision
  *and* confirm, paid once at nomination and never at key time. **The shared
  floor was unpinnable until a second, narrow test hook existed**
  (`debug_expire_create_schedule_for_test()`): deleting the floor left the
  whole suite green, because every test reached the "next turn" state through
  `debug_expire_create_spacing_for_test()`, which expired the floor along with
  the create deadline — a guard whose only test also disables the thing it
  guards against asserts nothing. Found by mutation, and the *same* mutation
  first survived because the floor was redundantly re-checked inside
  `membership_pump_invite()`; there is now exactly **one** floor check, in
  `nomination_tick()`.
  **Fix round 1 (M2, Major): moving invite issuance onto a free-running pump
  broke the probe/batch-API mutual exclusion.** `Begin/Add/ExecuteBatchInvite
  Users` is a **fourth** Begin/Add/Execute sequence — `tick()`'s inventory
  counted three — and the first on the command loop that does **not** sit
  inside an `owner == Nomination` branch, so fact 2 of that inventory's
  three-fact chain, which is what excludes all the others, never covered it.
  Law 2 is precisely why: invites used to be issued inline from
  `onCreateChannelResponse` (where fact 2 did cover them) and are now issued up
  to ~22 s later. `membership_pump_invite()`'s own comment claimed the
  exclusion was "held where it always was… the queue is only ever FILLED from
  paths already gated on `has_pending_work()`" — true of the **fill**, false of
  the **issue**, which Law 2 had just separated. The trigger is the natural
  operator flow: `nominate_done` fires on the **last create's** response with
  every invite still queued, so the arbiter is free and a dock **Talkback
  probe** press passes every gate `probe()` has and spawns the driving thread.
  Gated now at the top of the pump on `has_pending_work()` — the same question
  `session_start()`/`nominate()`/`resolve_roster_change()` ask, and the right
  polarity here: TRUE means the driving thread has work and may be inside an
  SDK call. It is already correct about the two windows a phase check alone
  gets wrong (`m_driving_thread_in_sdk_call`, and Destroying not storing
  `Done` until after its own batch). The arbiter would be the **wrong** gate —
  a probe holds it only until its create response lands, and the driving
  thread outlives that by the whole tone-and-destroy tail, which is the part
  that batches. It **defers, never drops**: nothing is dequeued, the next 50 ms
  turn retries, and `tick()`'s inventory now counts the invite sequence as
  fact 4, enforced at its call site rather than inferred from where it sits.
  Two more mutation-proved gaps from the same round: **a queued invite
  outliving its channel** (`m_invite_queue.clear()` in
  `nomination_destroy_provisioned()` — the header calls it structural, and
  removing it stayed 68/68 green), and **the invite-side code-18 backoff**,
  unpinnable because `debug_expire_create_spacing_for_test()` expires *three*
  things, not the two its comment named — the third being every queued
  invite's own `not_before`, which every invite assertion in the file went
  through 128 times per call. `debug_expire_membership_floor_for_test()` is the
  narrow hook that can express "the pacer is open and this invite is still
  backed off": the identical shape fixed for the create-side floor, one field
  over, in the same change.
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
  ~600 ms window that opens (kMembershipCallSpacing) is closed where it matters: `nominate()` refuses
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
- **The talent list is ordered by CoreVideo, not by the Zoom SDK** (live
  defect, 2026-08-29, Zoom Events production with breakout rooms). The
  operator's report was "moving from room to room the nomination list doesn't
  update". The roster cache was fresh (`ZoomEngineClient::roster()`, the exact
  cache the dock reads, verified live through the control API's
  `list_participants`) and the rows the dock derived from it were right. The
  defect was the REBUILD GATE: the `(present, name)` signature was built by
  walking the roster in whatever order `GetParticipantsList()` returned, so a
  merely REORDERED roster — same people, same presence — read as a changed one
  and rebuilt the whole `QListWidget`. On this dock's 100 ms tick, in a room
  where two of Zoom's five roster callbacks fire on every mute and camera
  toggle by anyone, that is a `clear()` + re-add several times a second: the
  scroll position resets, and a tick-box click (an ordinary click is
  80–150 ms) lands its press on an item that no longer exists by the release,
  so **the tick never registers**. What the operator saw as "the list doesn't
  update" was their own edits being thrown away, not the roster's. The whole
  rows-and-signature decision now lives in `talkback_nominee_rows()` /
  `talkback_nominee_signature()` / `talkback_nominee_list_refresh()`
  (`src/talkback-dock-state.h`, pinned by `CoreVideoTalkbackDockState`), which
  order the rows from CONTENT alone — everyone present sorted, then the ticked
  names who have left, sorted — so the signature is a function of the set and
  a reorder cannot rebuild. Two deliberate consequences: the visible order
  stops moving under the operator's cursor mid-tick, and the nominee list this
  feature sends is now deterministic, which matters because with the channel
  budget short it is LIST ORDER that decides who gets a private channel
  (`talkback_plan()`, `src/talkback-plan.h`) — roster order made that
  arbitrary and re-rollable on any roster event. Mutation-proved: restoring
  roster-order rows fails five assertions in
  `tests/talkback-dock-state-test.cpp`, including the reorder-must-not-rebuild
  case, and reverts clean.
- **"Nominate" is gone from the operator-facing copy, and only from there**
  (owner: "not a word that really makes sense here"). The section is
  **Talent**, the button is **Assign channels (N)** / **Clear all channels**,
  the plan block leads with "No channels assigned yet." and reports a refusal
  as "Channel setup failed: <reason>.", and the key-button refusals talk about
  channels ("no one has a channel yet", "assign channels again"). The engine's
  echoed recovery hint goes through `talkback_dock_recovery_label()`, which
  spells its `"re-nominate"` token as "re-assign channels" and passes anything
  it does not recognise through VERBATIM — that is a vocabulary map, not an
  inference: a refusal that carried no hint still gets none, pinned by its own
  test. Everything internal is untouched on purpose: the `talkback_nominate`
  wire command and its stage names, `TalkbackNominationPlan` and the rest of
  the identifiers, and the comments, which describe code that still says
  nominate. Companion and the control API depend on that surface.

- **Polish round on the standalone dock** (owner's first look at the built
  panel, 2026-08-29: "UI still doesn't feel polished"). Four defects, all
  confirmed on a screenshot and all now confirmed fixed by an offscreen render
  of the same widget tree rather than by reading the code. (1) **Key labels
  were clipped mid-glyph at BOTH ends** — the grid was hard-coded two-up and
  `QPushButton` does not elide, so centred text lost the same amount at each
  side: "Grant Whitehead" rendered as "rant Whitehead". On a control that opens
  a live microphone to one named person, two names that differ only at the
  start reading identically is a wrong-person hazard. The grid was made
  adaptive (`talkback_dock_key_columns()`: two columns only when two of the
  WIDEST button fit with the gap, else one full-width column) and anything
  that still did not fit was elided with the full name in the tooltip.
  **That helper is gone** — sizing every button to the longest name in the
  room is what produced the 400 px tower the redesign below replaced; the
  elision-with-tooltip half survived it verbatim.
  (2) **The talent list showed about two and a half rows for five people** —
  its height was guessed from `fontMetrics()`, which misses the stylesheet's
  item padding and the frame, and then a dock shorter than the panel squeezed
  it to its 3-row minimum. It is now sized in WHOLE rows
  (`talkback_dock_nominee_visible_rows()`, clamped to 3..6) from the widget's
  measured `sizeHintForRow()`. (3+4) **The whole panel is in a `QScrollArea`
  now**, which is the structural half of the same defect: a `QWidget` in an OBS
  dock that wants more room than it has does not clip politely, Qt shrinks
  whatever is shrinkable — that is what squeezed the list AND what pressed the
  third key row flat against the Key group's bottom border. Sections now keep
  the size they ask for and the dock scrolls. One spacing scale replaces the ad
  hoc 8/6/4/8 (`kDockMargin` 10 / `kSectionGap` 14 / `kInnerGap` 8; the fourth,
  `kGroupPad`, went with the group boxes the redesign below removed), the
  Talent intro is one line with the paragraph moved to
  the section's tooltip, the plan block splits into a body-weight headline and
  a secondary detail label (`#talkbackPlanDetail`) instead of one muted 11 px
  run that buried the answer, the Assign button is full width like the sibling
  docks' section actions, and the Off-air banner drops from 16 px to 14 px —
  the strip earns its height from the LIVE rule, not from a permanent block of
  empty space. Group titles were left alone on purpose: they already come from
  the shared `cv_stylesheet()` rule every other dock uses, which is what
  "consistent with the siblings" means here. Both new helpers are pure and
  pinned in `tests/talkback-dock-state-test.cpp`; both mutation-proved (fixing
  the row count at the minimum, and hard-coding two columns) and reverted
  clean.

- **The dock is an INTERCOM GRID now** (owner, after running the polished
  version live with seven talent, 2026-08-29: "need to rethink how this works
  and how it will look"). The structural flaw was not styling. The **same
  people appeared twice** — a key button each in the Key section, a tick box
  each in the Talent section — so the panel grew at twice the rate of the cast;
  and because the key grid sized every button to the WIDEST label in the room,
  one 28-character Zoom display name ("Ronny Hofsøy, Tromsø, Norway") flipped
  it to a single full-width column: a 400 px tower of buttons for seven people,
  and nothing survivable at twenty-four. The model is now the one the operator
  already carries from a Clear-Com/RTS panel: **ONE grid**, "All talent" as a
  full-width key on top, then one COMPACT fixed-height cell per person, **two
  or three across by dock width and never one**
  (`talkback_dock_cell_columns()`, `kTalkbackDockCellMinPx` = 118 — sized to a
  minimum readable width, not to the longest name, which is the mistake that
  let one person's display name decide the layout for everybody). The tick-box
  list is behind an **[Edit talent]** toggle and takes the grid's *place*
  (`talkback_dock_edit_mode()`), so exactly one list of people is on screen at
  a time; it refuses to open, and re-closes itself, whenever a key is open
  anywhere — hiding a button the operator is holding strands the key (a latch
  loses its only close affordance, a PTT loses its release). The three group
  boxes are gone; everything that is not a person is one bottom strip
  ([Edit talent] · Latch · source combo) over a one-line source status.
  **A cell IS a key button** — a restyled `QPushButton` with two
  `WA_TransparentForMouseEvents` child labels (the name and a state line;
  `QPushButton` draws one font, so a two-line `text()` could not carry two
  type sizes). Every rule the key buttons earned moved across untouched: the
  single `TalkbackDockOpenKey`, `talkback_dock_release_lost()` reading the
  widget's own `isDown()`, `needs_renewal = false`, the never-disable-a-held-
  button guard, the rebuild gate, keying on the Qt main thread.
  **What is new is what a cell SAYS**, and it is the point of the redesign:
  `talkback_dock_cell_state()` maps each person to Ready / OnAir / NoChannel /
  Unreachable / NotInChannel, with a stated precedence (ON AIR beats
  everything; unreachable beats no-channel, because `TalkbackPlan::unreachable`
  is a strict subset of `uncovered_private` and the generic "assign channels
  again" would otherwise send the operator to fix something no assignment can
  reach; no-channel beats not-in-channel, there being no channel to be in).
  **Enablement is NOT re-derived from any of it** — a cell carries
  `talkback_dock_key_buttons()`'s `enabled`/`reason` verbatim, so the state can
  never make a cell live that `key_on()` would refuse, and a stale presence
  observation can never refuse a key on a standing channel (F1's lesson,
  through a new door). The consequence is deliberate and documented: with the
  engine stopped every cell is disabled with the global reason in its tooltip
  while its state line still describes the PERSON.
- **...and where per-person presence comes from** (`TalkbackChannelPresence`,
  `src/talkback-nomination.h`; the mapping in
  `talkback_channel_presence_apply_report()`,
  `src/talkback-nomination-dispatch.h`). **The wire protocol did not change.**
  The engine already reports every membership edge as a
  `"cmd":"talkback_nominate"` stage line, and the 2026-08-29 show's own log is
  what the states were written against:
  `"stage":"invite","name":"John Wallace",…,"code":2` (SDKERR_WRONG_USAGE, on
  both his channels — he was in a **different breakout room** from the engine,
  and Zoom's talkback reaches only the room the inviter is in) and
  `"stage":"participant_talkback_support","name":"Grant Whitehead",
  "supported":false` followed by an invite refused `code:3`. Both men had a
  private channel, a completed `nominate_done` (7 channels), and heard
  **nothing**; to the old dock both were ready, pressable keys.
  `member_invited` → Present; `supported:false` → NoTalkback; a non-zero
  `invite` `code`, `member_invite_failed`, `member_not_in_meeting`,
  `member_left` → NotInChannel. Rules that are not preferences but what the
  wire does: **`supported:true` is not presence** (it is emitted before the
  invite is issued); **NoTalkback survives a later NotInChannel**, because a
  client with no support produces both and the specific diagnosis is the
  useful one; **last edge wins otherwise** (three people that morning were
  refused `code:18` on the all-talent invite and admitted to their own channel
  a second later); **Unknown renders as "ready", never as absent** — an engine
  that reports none of these, or a plugin that has not seen them yet, must not
  paint every person amber. Missing fields default to "nothing happened"
  (`code` 0, `supported` true) for the same mixed-version reason the attempt id
  does, since a DLL-only install is this project's canonical mistake. It is
  **display-only**: nothing in the keying path reads it. Cleared at exactly the
  points `talkback_nomination_reset()` is, plus the SEND of a new nominate — a
  nomination replaces the standing channel set, so every observation is about
  channels the engine is destroying, and clearing to Unknown fails soft.
  A separate function rather than a third out-parameter on
  `talkback_nomination_apply_report()`: these stages carry no `"attempt"` id
  (they are edges about the current channel set, not staged reports of an
  attempt's outcome), and two matching rules behind one call is C1's confusion
  again. Ten mutations run and all ten killed, including the two precedence
  swaps, the not-sticky NoTalkback, a one-column grid, an editor that opens
  over a held key, and a cell that re-derives its own enablement.
  Verified by MEASURED OFFSCREEN RENDER, not by reading the code: at a 320 px
  dock, 2 columns, 48 px cells, 126 px of name room, one of the seven real
  names elided (end-only) and nothing clipped; at 420 px, 3 columns; 24 people
  at 320 px is 25 cells in 910 px inside the scroll area.
- **...and that verification was worth exactly nothing, because it measured a
  render the grid had sized itself** (operator's high-DPI display, 2026-08-29,
  second look: names clipped MID-GLYPH with no ellipsis -- "Grant Whiteh",
  "Jeffrey Wiltsh" -- and the last cell row cut in half across its state line).
  The round above measured once and then spent the answer as a pixel budget
  somewhere else: `layout_cells()` derived a column width arithmetically
  (`(available - gaps) / columns`), elided every name against that number, and
  let the QGridLayout hand the cell a different width. Measured off the
  operator's own screenshot: the grid had re-flowed three columns to two and
  `setColumnStretch()` had been written only for the columns the NEW flow uses,
  so column 2 kept the wider flow's stretch and an EMPTY third of the dock held
  the space -- All talent spanned 405 of 599 px, cells got 194 px each while
  the elision had charged them 291, and "Grant Whitehead" needed 200 px in a
  154 px label. Same cause vertically: the container kept the height it had
  negotiated for the wider flow's four rows while the narrower flow needed
  five. `available` was also read from the grid CONTAINER's own width, which is
  downstream of the flow -- a loop that can only confirm whatever the grid
  already did. The fix is that nothing predicts a pixel any more
  (`src/talkback-cell-grid.{h,cpp}`, new, Qt-Widgets-only so an offscreen
  harness can build the REAL cells): `TalkbackCellButton` elides in its own
  event filter from `QFontMetrics` of the font the label actually has against
  the width the label actually got, reports a HEIGHT from the layout inside it
  (`QPushButton` derives both hints from its own `text()`, which is empty in a
  cell) and NO WIDTH AT ALL -- because a cell elides, and a QLabel that
  defends its full text is how the longest name in the room decides the dock's
  width for everybody, the 400 px tower coming back through the layout's
  minimum; `talkback_layout_cell_grid()` re-writes the stretch of EVERY column
  the layout knows about, live and left-over; the minimum readable cell width
  is measured (`talkback_dock_cell_min_px()`, a gauge string in the live
  label's metrics plus the cell's own measured chrome) with 118 px demoted to
  a floor; and the dock's width is read off the SCROLL AREA'S VIEWPORT, the
  one number the grid cannot inflate. The panel also re-measures on
  `FontChange`/`StyleChange` (OBS themes after construction, and a window
  dragged to a display with different scaling), which nothing did before.
  Pinned: the measured minimum is in `tests/talkback-dock-state-test.cpp`
  (mutation-proved -- pinning it back to the 118 px constant fails two
  assertions). The two layout fixes are proved by an offscreen harness that
  builds the real cells at 1x/1.5x/2x font scale and MEASURES what they render
  (stale stretch: legacy fills 203 of 300 px, fixed fills 300; cut row: legacy
  container 216 px for a 272 px grid, fixed 272; every name complete or
  END-ellipsized at all three scales). Two limits stated rather than hidden:
  the container's explicit minimum-height line is NOT pinned -- deleting it
  does not reproduce the cut, because the honest cell hints already carry it,
  and it is kept only as the explicit statement of the rule for the OBS dock
  the harness cannot reach; and **the harness is not the operator's screen** --
  that is exactly the claim this entry exists to correct.

- **...so the custom sizing machinery is GONE, and verification moved into the
  product** (operator's OBS, 2026-08-30: "still cut off, we gotta do better" --
  the grid area got ~90 px, the single "All talent / assigning..." cell was cut
  across its state line, and ~500 px of panel sat empty below it). THREE rounds
  of hand-written height negotiation, THREE offscreen certifications, THREE
  clipped renders on the operator's screen. The ruling is not "measure harder":
  height negotiation written by hand in this codebase does not survive contact
  with a real OBS dock -- the round above even reproduced its own defect under
  Qt's "minimal" platform plugin and not under "windows", which is an
  invalidation race, i.e. exactly what hand-written negotiation creates and a
  harness cannot see. **Deleted from `src/talkback-cell-grid.{h,cpp}`**: the
  `sizeHint()` and `minimumSizeHint()` overrides, the report-no-minimum-width
  trick (`hint.setWidth(0)`), the per-column `setColumnStretch()` bookkeeping,
  the `grid->invalidate()` + `container->setMinimumHeight()` +
  `updateGeometry()` block, and the stylesheet's `min-height: 46px` on the cell
  (a height decided in two places). **What replaces them is dumb on purpose**: a
  cell is `setFixedHeight()` from the LABELS' OWN live `QFontMetrics` (the
  taller line twice, plus the layout's vertical margin, floored at the 46 px the
  sheet used to carry) -- fixed means minimum == maximum, so `qSmartMinSize()`
  takes it verbatim and `QPushButton`'s text()-derived hints (empty in a cell,
  which is why round 2 overrode them) get no say -- recomputed on every
  font/style change, which is the whole DPI story with no scale factor read
  anywhere. The grid is a plain `QGridLayout` of fixed-height widgets, and
  standard layouts propagate correct minimums up to the scroll area through
  `QWidget::minimumSizeHint()` with no help. A re-flow **deletes the whole
  QGridLayout and builds a new one** rather than editing it: a fresh layout has
  exactly the columns we put in it, so a wider flow's left-over column -- which
  held a third of the operator's dock in round 2 through a stretch nobody
  cleared -- cannot exist as state at all. Kept, because they work and neither
  predicts a pixel: the cell's reactive end-elision in its own event filter, and
  the MEASURED column-count input (`talkback_dock_cell_min_px()`), which is an
  input to a discrete two-or-three choice, not a budget anything is sized
  against. The talent list keeps its `sizeHintForRow()` height and is justified
  in a comment rather than deleted: it is the same shape as the fix (measure one
  live number, set a FIXED height), and a QListWidget's own hint is a scrollable
  viewport's, i.e. the "however many rows fit in whatever it is given" answer
  that showed two and a half rows for five people.
- **...and the verification instrument that replaces the harness**:
  `COREVIDEO_TALKBACK_LAYOUT_TEST` (optionally `=N`, default 8, capped at 64).
  Set it and the Talkback dock populates ITSELF at construction with a fake cast
  -- every cell state at once (ready / ON AIR / no channel / not in channel / no
  talkback / assigning...), the names that have actually broken this grid (the
  28-char Norwegian one with its real diacritics, a 38-char monster, two that
  differ only late, one two-letter), a shortfall-shaped plan report and a LIVE
  banner -- so the REAL panel can be looked at in the REAL OBS with no meeting,
  no talent and no show. Unset, it is one `qEnvironmentVariableIsSet()` check
  and nothing else changes. Set, `refresh()` returns before it reads the engine
  (gating the DATA-driven sections, never the layout machinery -- `layout_cells()`
  still runs on the tick and on every resize, because re-flowing as the dock is
  dragged narrower is precisely what is being verified) and all three paths to
  the engine -- a cell press, Assign channels, the probe -- refuse with a log
  line, so there is no route from this mode to a Zoom SDK call. The cast itself
  is pure (`src/talkback-layout-test-cells.h`) and its coverage claim is pinned
  by `tests/talkback-layout-test-cells-test.cpp` (69 tests now): an instrument
  that silently stopped emitting one state would let the verification pass while
  that state stayed broken -- the same shape, one level up, as the three
  offscreen certifications this replaces. The banner and plan painting were
  extracted from `refresh()` (`paint_banner()`/`paint_plan()`) and are SHARED
  rather than copied, because an instrument painting its own approximation would
  verify a layout the product does not have.
- **The record-privilege handshake is a STATE, not an error** (live defect,
  2026-09-05, launch day: starting the engine popped a "Zoom Join" modal
  reading "raw recording failed" on a session working exactly as designed).
  `canStartRawRecording()` returning `NoPermission(6)` is the NORMAL first
  half of Zoom's record-privilege handshake -- the engine asks the host
  (`requestLocalRecordingPrivilege()`, `engine/src/main-macos.mm`'s
  `handle_start_media()`), the host grants it, and `raw_media_ready` follows
  on its own. `handle_event()`'s `raw_media_start_failed` +
  `privilege_requested` branch already refused (before this fix, and
  unchanged by it) to route this into the join-failure/reconnect machinery --
  its own comment documents a live incident where doing so once flipped a
  healthy joined session to Failed and gated `start_engine`, resubscription
  and recovery for the rest of the session. What it still got wrong: it set
  `m_last_error` and fired every registered `ErrorCallback`, which is what
  pops the dock's `QMessageBox`. Fixed with a SEPARATE `NoticeCallback` list
  (`ZoomEngineClient::add_notice_callback()`/`m_privilege_notice`,
  `src/zoom-engine-client.h`) rather than a severity flag on the existing one:
  every `ErrorCallback` subscriber's contract -- today just the dock, but a
  public extension point -- is "this means show a failure", so a severity
  field is a branch every current and future subscriber has to remember to
  check, where a separate list makes "this can never reach a QMessageBox" true
  by construction. `m_last_error` is deliberately left untouched, because
  other code (the `"left"` handler's `keep_failed` check) reads it as "the
  session actually failed," which a pending grant is not. The operator-facing
  copy and the first-vs-repeat classification (the engine asks the host only
  ONCE per meeting; every later report is the same still-pending wait, told
  apart only by its `"detail"` text containing "already requested") are pure
  functions in `src/zoom-privilege-notice.h`, extracted for the same reason
  `zoom-join-decision.h`/`join-watchdog.h` are: host-tested without Qt/OBS in
  `tests/zoom-privilege-notice-test.cpp`, three mutations run and killed
  (breaking the classifier substring, collapsing the two notices to identical
  copy, and inverting the fail-safe so an empty/unrecognized `detail` reads as
  already-requested). The notice CLEARS on `raw_media_ready` (the engine event
  that means the grant landed) and on the `"left"` per-meeting reset, and the
  dock's `update_privilege_banner()` is driven BOTH by the callback and by a
  100ms poll of `pending_privilege_notice()` in `update_state_indicator()` --
  the poll is what actually hides a stale banner after a leave/rejoin, since
  the `"left"` handler clears the field without a separate notify call, same
  as it has never notified roster callbacks either. Rendered as a
  `CvBanner(Warning)` under the engine controls, not the modal it replaces.

- **Panelist loudness meter is fed on the audio lane, per source, from the
  WIRE format** (`src/zoom-participant-audio-source.cpp`, feat/panelist-feedback
  Task 5): each `CoreVideoAudioSource` now owns a `LoudnessMeter`
  (`src/audio-loudness.h`, a pure BS.1770-4 header from earlier tasks in this
  feature). `output_audio_frame()` feeds it inside the per-slot drain loop --
  same rule as everywhere else in this file: a media event is a coalescing
  PROMPT, feeding "the buffer that woke us" instead of every drained slot
  reads low by a load-dependent amount. It is fed the pre-publish WIRE PCM
  (post resume-fade, pre Mono/Stereo assembly) because the operator's
  Mono/Stereo routing choice for OBS is not what the panelist actually sent,
  and mono-summing a stereo feed reads ~3 LU different from the same person
  carried as stereo. A reset request is an atomic flag consumed on the audio
  lane's own next slot, never called directly from another thread, because a
  reset has to land on a hop boundary the meter itself controls. Display name
  is cached (`ctx->display_name`, guarded by `ctx->mtx`) from the roster
  callback's already-fetched roster copy in `maybe_resubscribe_for_roster()`
  -- never re-resolved per buffer, since `ZoomEngineClient::roster()`
  deep-copies every `ParticipantInfo` under a hot mutex and the readiness
  board this feeds polls at ~10 Hz. `corevideo_loudness_readings()` /
  `corevideo_reset_loudness_windows()` mirror `corevideo_audio_source_infos()`'s
  registry pattern exactly, including its `g_sources_mtx`-then-`ctx->mtx` lock
  order; the audio lane never touches `g_sources_mtx`, so the order cannot
  invert. The window resets on every (un)subscribe transition
  (`unsubscribe_audio()`, `forget_subscription_for_new_engine()`), because
  integrated loudness is scoped to one panelist's mic check, not the source's
  whole lifetime -- without it a re-subscribed source's number is polluted by
  whoever spoke before on the same source.
- **Only Participant-kind sources vote on the panel median, or appear on the
  board at all** (final whole-branch review, 2026-09-05, Critical). Three
  kinds of CoreVideo audio source exist (`CoreVideoAudioKind`,
  `src/audio-subscription-state.h`): Participant, ActiveSpeaker (a resolved
  duplicate of whoever is currently talking) and Audience (the whole-meeting
  mix, no participant id). `corevideo_loudness_readings()` used to hand ALL
  three to the board with no distinction: an ActiveSpeaker source in a scene
  with 4 Participant sources put a duplicate of whoever was talking into a
  5-value median, shifting the reference by up to a full LU on an even/odd
  flip and moving every panelist's pass/fail with it; an Audience source put
  the whole-room mix into a reference meant to describe individual
  microphones; both rendered as phantom `"- unassigned -"` rows since neither
  carries a participant id or (for Audience) ever gets a cached display name.
  `LoudnessReading` now carries `kind`, and the filtering lives in the PURE
  header (`src/loudness-board.h`'s `loudness_panel_median()` and
  `loudness_board_build()`), not in the OBS glue that calls it -- both are
  pinned by a test that a median over a mixed vector equals the median of the
  Participants alone, and the board excludes non-Participant readings from
  rows entirely rather than showing them as read-only entries: the board's
  whole model is "one row per panelist's mic", and a row for a duplicate
  speaker or the room mix is the same mystery-row defect wearing a label,
  not a fix for it. **The board shows SOURCES, not the roster** -- see the
  header comment on `src/zoom-loudness-meter-source.h`: a row exists because
  an operator created a Participant-kind source and pointed it at somebody,
  so a panelist with no source never appears, and a rejoin (participant ids
  are meeting-scoped and do not survive one) drops a row to
  "- unassigned -"/"no audio" until the operator re-points it. Correct
  behaviour, documented because it is the most likely way an operator
  concludes the board itself is broken.
- **The deviation bar is driven by short-term deviation, the row TEXT stays on
  the integrated verdict** (`loudness_board_bar_input()`,
  `src/loudness-board.h`). `LoudnessBoardRow::has_short_term`/`short_term_lufs`
  were measured and carried all the way to the board and read by nothing --
  `row_value_text()` only ever printed the integrated number. Ruling: bars
  are geometry the renderer fills every frame regardless of the label-refresh
  gate, so driving the bar's position from the fast (short-term) measure costs
  no extra text-child churn, while folding short-term into
  `LoudnessBoardModel::signature` would rebuild every row's text children
  ~10x/sec for a value the text never shows -- exactly what
  `loudness_board_needs_label_refresh()` exists to prevent. Where a short-term
  reading or a reference is unavailable, `loudness_board_bar_input()` falls
  back to the integrated deviation the bar always used, rather than showing no
  bar at all. **Regression B, same-day re-review**: the first cut populated
  `has_short_term_deviation` whenever a short-term reading and a reference
  existed, with no gate on whether the row had a real verdict yet.
  `loudness_meter_short_term()` is UNGATED, so a subscribed-but-silent source
  reads its own noise floor (e.g. -90 LUFS) -- against a ~-22 LUFS reference
  that is a ~-68 LU deviation, clamped to a full-length bar pegged hard left,
  in idle grey, right next to the text "no audio"/"measuring". A silent
  preshow is this board's NORMAL state, so this was the common case, not an
  edge one. `has_short_term_deviation` is now gated on `has_deviation`
  itself (the same Pass/Loud/Quiet verdict condition) rather than restated
  independently -- which also guarantees the integrated fallback is always
  available whenever the short-term value is. NoAudio and Measuring rows now
  draw no bar at all, pinned in `tests/loudness-board-test.cpp` by a case
  with a stray -90 LUFS short-term reading on an otherwise-silent row.
- **No `log10` in the relative gate's hot loop**
  (`loudness_meter_integrated()`, `src/audio-loudness.h`). Pass 2 used to
  convert every gated block (up to `kLoudnessMaxGatedBlocks` = 6000) to LUFS
  with `loudness_lufs_from_mean_square()` (a `log10()`) just to compare it
  against a LUFS threshold, once per source per 100 ms poll, on the graphics
  thread inside `corevideo_loudness_readings()`, under the same mutex the
  audio drain holds for its whole drain. The relative gate
  (`kLoudnessRelativeGateLu` = -10 LU below the absolute-gated mean) is
  exactly a factor of ten in LINEAR mean square -- `L(z) > L(mean) - 10 <=>
  z > mean/10` -- so the comparison is now `z > 0.1 * abs_mean_z` with no
  log10 anywhere in the loop, which also removes a float round-trip that
  could flip a block sitting exactly on the threshold. The pinned -27.08/
  -20.16 and -22.96/-20.06 figures in `tests/audio-loudness-test.cpp` are
  unchanged. `loudness_meter_configure()` also now `reserve()`s `gated` to
  `kLoudnessMaxGatedBlocks` up front, so the audio lane is provably
  allocation-free after configure.
- **`corevideo_loudness_readings()` uses `try_lock` on each source's mutex,
  never a blocking `lock()`, and a failed try_lock skips the MEASUREMENT,
  never the ROW** (final whole-branch review, 2026-09-05, Important, plus a
  same-day re-review regression). It runs on the OBS graphics thread via
  `meter_video_tick()` and takes the exact `ctx->mtx` that
  `output_audio_frame()` holds across an entire drain -- including
  `shm_region_open_readwrite()`, `obs_source_output_audio()`, and
  rate-limited `blog()` calls that write to disk -- so a blocking lock here
  could stall the graphics thread for the length of one source's drain.
  `corevideo_audio_source_infos()` (the sibling registry walk) is unchanged
  and still blocks -- it is called far less often and from a different
  context. **Regression A, same day**: the first cut `continue`d past
  `out.push_back(r)` on a failed try_lock, which drops the whole reading, not
  just its numbers -- one fewer row for `loudness_board_row_rect()` to divide
  the canvas by (every OTHER row visibly resizes for one poll), one fewer
  vote for that poll's panel median (every OTHER panelist's pass/fail can
  move for 100 ms), and a changed `model.signature` that forces exactly the
  full child-text rebuild `loudness_board_needs_label_refresh()` exists to
  prevent -- on a mutex that is busy often, not rarely, since
  `output_audio_frame()` holds it across a real drain. The reading is now
  ALWAYS pushed; only the measurement fields (`has_short_term`/
  `has_integrated`/`gated_blocks`) are left at their unavailable defaults
  when the try_lock fails, which `loudness_board_build()` already renders
  correctly as NoAudio -- the same state a source that has simply never
  spoken gets. The row's identity (`source_uuid`, `kind`, `participant_id`)
  was always readable without `ctx->mtx`, but `display_name` was not: it is
  now guarded by its own dedicated `name_mtx` (`CoreVideoAudioSource`, next
  to the field) instead of `ctx->mtx`, specifically so a busy audio drain
  cannot make a row's NAME flicker to "- unassigned -" on the exact polls
  where its numbers go missing. All three `display_name` writers
  (`unsubscribe_audio()`, `forget_subscription_for_new_engine()`, the roster
  callback) moved to `name_mtx` alongside the reader.

## macOS raw-media lifecycle (2026-09-06 soak remediation candidate)

`src/raw-media-lifecycle.h` owns Start intent separately from permission and room
readiness; `engine/src/main-macos.mm` applies its effects on the SDK main queue.
The two grant callbacks cannot issue duplicate starts, and callbacks retained
from a retired record delegate cannot resurrect Stop/leave/replacement intent.
Only `ZoomSDKError_NoPermission` triggers the once-per-meeting host request.
SDK request status 2 is **Timeout**, not Denied; SDK error 21 is **NoLicense**,
not evidence of a temporary permission failure. No new timed session retry loop
is justified by those codes; terminal start failures stay joined and permit an
explicit Start retry or a fresh room readiness transition.

Breakout entry/exit invalidates SDK video/share renderers and the one global
audio subscription while preserving desired source bindings and SHM generations.
Delegates detach before `destroyRender:`. A fresh InMeeting checks raw readiness
and restores current eligible placeholders once; missing room-scoped participant
ids stay pending for plugin roster rebinding. Failed video subscriptions retain
placeholders, so recovery or an ordinary retry cannot lose bindings; removals and
rebinding update the same current table. Target `requested_resolution` survives
recovery; shared-renderer quality arbitration is a separate follow-up.
`raw_media_ready` still means raw recording started, **not** that every source is
healthy. Additive `raw_media_state` invalidates client session readiness during
permission loss/recovery/failure; per-source subscribe errors and first-frame
logs remain independent evidence. All `raw_media_start_failed` reports cross the media-only callback boundary in
`zoom-engine-error-dispatch.h`, including terminal failures without a pending
privilege flag. They cannot invoke meeting/reconnect effects. Terminal media
text is kept in `m_raw_media_error` and exposed by `last_error()` as a fallback
for the control API; internal `m_last_error` stays reserved for meeting failure,
so a later `left` cannot mistake a media failure for a failed meeting. Meeting
status callbacks capture `MeetingCallbackEpoch` at receipt and validate it on
the main queue. Leave rejects queued older work AND subsequent nonterminal
statuses while allowing its terminal SDK acknowledgement; replacement uses a
fresh epoch and delegate identity. Offline CoreVideoRawMediaLifecycle sequences
and a real SDK build cover these code defects; repeated live delayed-grant and
breakout resource-lifetime/recovery-time acceptance remains outstanding.

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

## macOS website downloads (2026-09-06)

The site builder keeps `MAC_VERSION` separate from the stable Windows version.
The macOS download is the locally signed/notarized `.pkg` on v0.1.45-beta.1;
its GitHub filename includes `v`. Do not restore the withdrawn macOS ZIP link.
Home, download, and plugin docs point to `/download/#macos`.

## Media failure presentation (2026-09-06 soak)

`MediaFailureState` tracks current source media failures, bounded
by live source assignments. Eight tiles × three failed attempts retain all
24 raw error diagnostics but emit one nonmodal episode notice. Dock polling
updates the affected count and escalates after three attempts or ten seconds
to Retry Media; it must show media errors while the meeting remains joined.
Connection errors still own `m_last_error` and the fatal callback path;
media failures must never vote in meeting leave/reconnect classification.

Only a successful shared-memory read (both standalone and supersource paths)
may acknowledge video recovery. Capture an assignment ticket before reading,
then validate ticket and participant; do not clear source failures at session
`raw_media_ready` or at a retry subscribe. Removal/reassignment retires only
that source's membership. Never dispatch UI/source callbacks from the frame
acknowledgement while a source lock is held. Permission notices are deduped;
Mac grants start automatically. Denial and timeout are distinct
`raw_media_state` debug events, not necessarily `raw_media_start_failed`.

`raw_data_controller_unavailable` follows the same per-participant video
recovery path. `shm_create_failed`, `subscribe_rejected`, and
`shm_name_collision` stay as persistent per-source media diagnostics (one
nonmodal notice per episode), clearable by removal, explicit stop, or meeting
reset. Their wire reports do not reliably identify a media lane, so neither
a successful video read nor `raw_media_ready` may clear them.

Retry Media is an explicit operator path shared by dock and control
`start_engine`: `ZoomOutputManager::retry_media()` also visits Tiles sources.
Tiles own a separate retry budget; `start_media()` alone is a no-op when raw
media is active and cannot revive an exhausted tile. Only the manual trigger
reopens that budget; ordinary roster/speaker sweeps retain their interval and
attempt caps. Manual retry preserves assignment and failure state, releases the
old SHM mapping, then subscribes. Tile enumeration runs on the OBS UI task queue
with strong source refs and the existing callback gate, skipping collection load.

Manual tile retry also covers retained displayed pixels when a current media
failure exists or the last successful SHM read is at least ten seconds old.
Fresh healthy tiles are skipped; mapping release preserves the decoded image.
This freshness override is manual-only, never a new automatic speaker-tick loop.
