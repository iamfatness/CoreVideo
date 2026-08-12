# Changelog

All notable changes to CoreVideo are documented in this file. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); releases
are tagged `vMAJOR.MINOR.PATCH` and published as
[GitHub Releases](https://github.com/iamfatness/CoreVideo/releases).

## [Unreleased]

## [0.1.37] - 2026-08-12

### Fixed
- **The Active Speaker source no longer cuts several times a second.** During a
  live show it could bounce between two people as little as 0.3s apart, against
  a hold time set to 2s. The hold time was never at fault. Every source refresh
  — and a refresh happens on load and on any property change, not just when you
  edit something — wrote that source's own copy of the speaker settings over
  the shared ones, and a source that had never been given an exclusion wrote an
  empty one. The excluded participant then became eligible, went on air, and
  was thrown off again as soon as the exclusion came back. Being thrown off
  leaves the director with nobody selected, which is the one case where it
  takes the next speaker instantly, ignoring both the hold and the sensitivity.
  A source now writes only the settings it actually carries, so the exclusion
  stays put and the hold applies to every cut.
- **The active-speaker exclusion no longer clears itself.** The same cause: any
  source without an exclusion of its own was quietly wiping the one you had
  set from the dock.
- **Participants no longer flash on air with no speaker change behind them.**
  The Active Speaker source warms up the next speaker's video before cutting,
  so the cut lands on a real frame instead of a gap — but every warm-up frame
  was being sent to the program output, including frames that arrived after the
  warm-up had moved on to somebody else, and frames the engine had already
  queued when the cut finished. On air that was a face appearing for a frame
  with nothing behind it, and when one of those late frames belonged to the
  person you had just cut away from, it cut straight back to them. Only the
  frame the cut is actually waiting for now reaches the output.
- **Less work on the video and audio paths.** The plugin reloaded its settings
  from OBS on two hot paths — once per roster update and once per active-speaker
  audio tick, several times a second each — and re-applied the whole director
  configuration every time. Settings are now applied where they change. This
  also clears the deprecation warnings that were flooding the OBS log.

- **Restarting the Zoom engine no longer leaves a source silent or dark.** If
  the engine process was replaced mid-show — a crash, a stall the watchdog
  caught, or a stop and start by hand — the fresh engine began numbering its
  buffers from scratch and asked for the same names it had used the first time,
  while CoreVideo was still holding the old ones open. Any buffer that needed to
  be larger the second time round could not be built, and audio in particular
  had no way back: it went quiet for the rest of the session. Every source now
  hands back the buffers it is holding before the replacement engine is even
  launched — video, screen share, audio, active-speaker preview and every tile
  on the Tiles wall together — so the new engine starts with a clear field.
  Handing the buffers back is only half of it, and the other half is asking the
  new engine for the feed again. Video sources were already re-subscribed on
  recovery; the Participant Audio, Audience Audio and Active Speaker Audio
  sources were not, and would sit silent on a subscription that had died with
  the old engine. They now forget that subscription when the engine is
  replaced and ask the new one for their audio as soon as it reports who is in
  the meeting.
- **Audio sources already on screen when OBS starts are no longer silent for
  the session.** A Participant Audio or Audience Audio source that was in the
  live scene before the Zoom engine had been requested asked for its audio
  immediately, before there was anything to ask, and then recorded itself as
  subscribed — so when the meeting was finally joined it never asked again and
  stayed quiet until you hid and re-showed it. These sources now only count
  themselves subscribed once the request has actually reached an engine, and
  keep asking on each participant update until it has.
- **"Zoom engine could not allocate shared memory" no longer strands a source.**
  A feed that re-pointed often enough — the Active Speaker source re-points on
  every speaker change — could reach a state where the engine kept failing to
  build that source's video buffer, and its frames were dropped for the rest of
  the session. Each rebuilt buffer was asking for a name the previous one had
  used, which Windows refuses to resize while CoreVideo still has the old one
  open. Buffers now move to a fresh name every time they are rebuilt, for
  video, screen share, and audio alike, so a rebuild cannot collide with a
  buffer still in use. This was the third time this fault reached air; it is
  fixed at the mechanism rather than at the timing that exposed it.
- **Feeds no longer flash bright garbage when they change participant.** On
  air, every active-speaker change made the affected source flash a frame or
  two of bright colour noise before settling. Any re-point of a source — an
  active-speaker cut, a reassignment in the Output Manager, an automatic
  recovery of a dropped feed — now hands the old video buffer back before
  asking for the new one, so the engine can rebuild it at the new
  participant's size instead of being blocked by the buffer CoreVideo was
  still holding. Switches cut cleanly, on Zoom sources and on the Tiles wall
  alike.
- **A source no longer goes permanently silent after an active-speaker
  change.** The same held-buffer problem could hit a source's audio on an
  active-speaker cut, and audio had no way to recover on its own: the source
  stayed silent for the rest of the session unless you hid and re-showed it.
  It only bit when the incoming participant needed a larger audio buffer than
  the outgoing one, which is why it survived casual testing.
- **Requesting the Zoom engine works on the first attempt.** A `ZoomObsEngine`
  left over from a previous OBS session keeps holding the Zoom SDK for a while
  after OBS exits — the SDK's own shutdown runs long — so the new engine's
  `InitSDK` failed with `SDKERR_OTHER_SDK_INSTANCE_RUNNING` and the request died
  as an opaque authentication failure. CoreVideo now recognises that specific
  collision and waits the leftover engine out (2s, 4s, then 8s a time, 78s in
  total), replaying the handshake itself; the join you already asked for goes
  through as soon as it succeeds.
- **A failed engine request no longer leaves a dead engine behind.** If the
  wait above runs out, CoreVideo stops its own engine before reporting the
  failure, so requesting the engine again really does launch a fresh one.
  Previously the engine process stayed alive but unauthenticated and every
  further request was silently ignored. The error now names the other Zoom SDK
  instance as the cause and says what to do about it.
- **An out-of-date effect file no longer blanks the whole Tiles wall.** The
  Tiles wall is drawn by a shader that ships beside the plugin, and an install
  that updated the plugin without updating its data folder left the two out of
  step. The wall then refused to draw anything at all — a black source in the
  middle of a show — because one optional part of the newer shader was missing.
  The wall now draws everything it still can: tiles, background, borders and
  crop are unaffected, only the tile glow is switched off, and the log says
  exactly which file is out of date and what to reinstall.

### Added
- **Luma range diagnostics.** Each source logs the luma range of its first frame
  (`Luma range probe: … min= max= under16= over235=`), which tells you whether
  video is arriving from the Zoom SDK full-range or limited-range. Nothing to
  enable.
- **Tile shape.** "Tile shape" sets the shape of every tile on the wall —
  16:9, 4:3, 5:4, 1:1, 3:4 or 9:16 — with a "Custom ratio" entry for anything
  else. A narrower tile fed by a widescreen camera crops more off the sides,
  because the tile is always filled and never letterboxed. Defaults to 16:9,
  which is what the wall has always been.
- **Wall spacing.** "Gap between tiles" and "Margin around the wall" are now
  yours to set, each as a percentage of the canvas height so the spacing scales
  with the canvas. Both default to 0.741%, which is the 8 px at 1080p the wall
  has always used, so an untouched scene lays out exactly as it did.
- **A background for the Tiles wall.** "Background colour" fills the gutters,
  margins and any uncovered canvas, and "Background source" draws any
  video-producing OBS source — an Image, a Media Source, a Browser Source —
  behind the tiles and over that colour. Leave the source on "- none -" for
  colour only. A background source that is deleted, or one that would render
  itself (the wall, or a scene containing it), falls back to the colour
  rather than breaking the wall.
- **Tile borders.** "Border width" (0-64 px), "Border colour", and a "Corner
  shape" of Square or Rounded with a "Corner radius" (0-128 px, shown only
  for Rounded). Rounded corners cut the video itself, so the background shows
  through them. Width defaults to 0, so existing walls look exactly as they
  did until you move it.
- **Tile glow.** Every tile can sit on a soft halo bleeding out into the
  background: "Glow size" (0-256 px), "Glow colour", "Glow intensity" and
  "Glow softness". Softness shapes how the halo falls away from the tile — 0%
  is strongest right at the tile edge and drops off immediately, 100% holds it
  just outside the tile before fading — so it can be matched against a
  reference by eye. Size defaults to 0, so existing walls look exactly as they
  did until you move it, and softness defaults to 0%. Note that the halo is at
  full strength at the tile edge whatever the softness; a Photoshop-style outer
  glow sits around half strength there, so start at roughly 50% intensity if
  you are matching one.
- **Per-tile crop.** A collapsible "Per-tile crop" group gives every tile its
  own left and right crop, as a percentage of the source width (0-45% a side),
  for reframing a guest sitting too far off-centre without disturbing the grid
  or any other tile. Crops belong to the tile position, not to whoever is in
  it, so they apply in Auto mode too. All crops default to 0.
- Tiles can now create and maintain one Zoom participant audio source per tile,
  inside a group you nominate, so each person gets a live fader and an ISO
  track without building the wall twice. Off until you name a group. The wall
  itself stays silent, so cutting between scenes never swaps audio. Tracks 2-6
  carry five ISO stems; past that, participants are on the program track only.
  Clearing the group again switches the feature off properly: the sources it
  made are muted rather than deleted, so your faders and filters survive and
  naming a group again brings back whoever is on the wall.
  **Use the audio group on one Tiles source only.** If you run a second wall —
  a panel wall beside a main one, say — leave its "Participant audio group"
  blank. Audio for each person belongs to whichever wall created it, so on a
  second wall someone who drops off that wall can be left muted while still on
  screen on the other one, and both walls number their ISO stems from track 2
  up, which can put two people on the same stem in the recording. The plugin
  logs a warning if it sees a second wall with a group set.

## [0.1.36] - 2026-08-09

### Fixed
- **Switching a source to Active Speaker no longer kills other feeds.** When
  the active speaker resolved to a participant already shown at a lower
  resolution, the engine tore down and recreated that participant's video
  renderer to raise the resolution — and the Zoom SDK's asynchronous release
  made the recreate fail (`WRONG_USAGE`), blanking every source sharing that
  participant until you hit Apply. Resolution is now raised in place on the
  live renderer, so nothing is torn down.
- **Video no longer freezes/stutters on scene switches.** CoreVideo sources
  render unbuffered, so a scene cut shows the current frame immediately
  instead of holding the last frame while OBS rebuilds a frame buffer. Also
  lowers latency.
- **Mapped feeds stay live like a webcam.** Subscriptions follow the
  assignment, not scene visibility — feeds stay warm across scene switches
  and fades instead of being torn down and rebuilt (which could also drive
  the Zoom SDK into a fatal that closed the engine). Dropped feeds now
  re-establish automatically by intent, so recovering video no longer needs
  a manual Apply/Recover.
- **The Active Speaker output keeps the last speaker on screen.** Muting,
  turning off video, silence, or a brief roster blip no longer dethrones the
  current speaker; only an explicit exclusion, a real departure (gone > 1
  min), or another participant speaking replaces them.
- **Stuck feeds recover sanely.** A feed that cannot subscribe (camera-off /
  phone-only participant) now backs off exponentially and shows "Can't
  subscribe" instead of retrying every 10 seconds forever.
- Speaker-director exclusions now persist correctly for participants with
  large Zoom user IDs (they were truncated through a 32-bit field).
- A revoked Zoom refresh token now clears itself and prompts a fresh
  sign-in instead of an endlessly failing Refresh.
- Hardware-accelerated conversion retries after a transient filter-graph
  build failure instead of permanently falling back to CPU for the session.

### Added
- Output Manager rows are sorted sensibly (Participant, Participant 2 …
  Participant 8, then Active Speaker, then Slots) instead of load order.

### Changed
- Per-frame debug telemetry is suppressed from the OBS log unless
  `CV_ZOOM_VERBOSE_LOG` is set (a 90-minute meeting was producing a 27 MB
  log); support bundles still capture it in memory.

## [0.1.35] - 2026-08-09

### Added
- **Automatic ISO encoder placement (new default).** Hardware encoders have
  a shared session budget (GeForce NVENC allows 8 concurrent sessions,
  shared with OBS's own program/stream outputs) — so instead of letting an
  8-feed NVENC configuration oversubscribe the GPU, CoreVideo now places
  each ISO feed itself: NVENC while the budget lasts (counting OBS's active
  NVENC encoders), then Intel Quick Sync, then CPU x264. The per-row
  Encoder column shows each feed's placement. Explicit encoder choices are
  still honored, but overflow beyond the session budget is placed down the
  same chain instead of failing. A feed whose encoder fails at startup
  automatically retries on the next encoder down.
- ISO sessions that fall behind the encoder now drop frames (bounded
  memory) and show "Encoder falling behind (N dropped)" instead of
  failing opaquely.

### Fixed
- **ISO recordings no longer end "randomly" mid-run.** A transient
  participant unresolve (engine reconnect, camera toggle) finalized the
  file immediately and started a new segment; unresolved participants now
  get a 60-second grace window before their recording is finalized. Only
  genuine resolution changes still cut (labeled) segments.
- Mid-recording session closes (resolution changes, participant departures,
  disk-full) no longer block frame delivery — with 8 feeds upgrading
  resolution together, the old blocking closes could starve the engine the
  same way the fixed stop() path once did.
- **The Output Manager always displays the truth now.** A participant
  assignment that momentarily couldn't be matched (e.g. during a roster
  refresh) displayed as "Active speaker" — and the refresh cycle then
  persisted that misdisplay, flipping every row's real assignment at once.
  Unmatched assignments now display faithfully ("Participant N (not in
  meeting)"), and refreshes preserve only edits you actually made — so
  assignments made in a source's Properties dialog finally show up in the
  Output Manager instead of being overwritten by a stale snapshot.
- A revoked Zoom refresh token (e.g. superseded by a newer sign-in on
  another install) no longer produces an endlessly failing Refresh button:
  the plugin clears the dead credentials on `invalid_grant` and tells you
  plainly to sign in with Zoom again.

## [0.1.34] - 2026-08-08

### Fixed
- **Stopping ISO recording no longer risks freezing the Zoom engine and
  dropping the meeting.** Stopping many simultaneous ISO sessions blocked
  frame delivery for up to 5 seconds per session while each encoder shut
  down; with 8 sessions that starved the engine's IPC channel long enough
  for the plugin to declare the engine dead and tear the session down.
  Session shutdown now signals all encoders at once and waits outside the
  frame path against a single 15-second budget.
- **ISO recordings now survive crashes and power loss.** ISO MP4s are
  written as fragmented MP4, so a recording is playable up to the last
  written moment even if FFmpeg is killed mid-write. Finalizing a recording
  no longer rewrites the whole file (which could take long enough on
  cloud-synced folders — e.g. OneDrive — to hit the old shutdown timeout).
  Tip: keep the ISO output folder on a local, non-synced drive for best
  performance.
- Terminated ISO encoders now report an accurate status ("did not exit
  within the shutdown budget") instead of "FFmpeg crashed."

## [0.1.33] - 2026-08-08

### Fixed
- **Participant feeds no longer freeze permanently when Zoom raises a feed's
  quality mid-meeting.** When a participant's video resolution increased, the
  engine had to grow that feed's shared-memory region — but Windows does not
  allow recreating a named section at a larger size while the plugin still
  maps the old one, so the allocation failed on every frame ("Zoom engine
  could not allocate shared memory … frames are being dropped") and never
  recovered. Regions now use generation-suffixed names, so a resize always
  lands on a fresh name that nothing stale can block. Audio regions got the
  same protection ahead of stereo support.
- Clearing an output's assignment ("None") now releases the plugin's
  shared-memory mappings, making None → reassign an effective operator
  recovery action.
- Shared-memory allocation failures now include the underlying OS error code
  in the engine log and support bundle.
- Zoom Marketplace share-link visitors hitting the OAuth broker are now
  greeted with guidance instead of a "Missing OAuth state" error.

## [0.1.31] - 2026-08-01

### Fixed
- **Hardware-accelerated video conversion works on OBS 32.2+ again.** v0.1.30
  avoided the OBS 32.2 FFmpeg collision by binding the FFmpeg build OBS
  already had in the process — but OBS's slim build has no `scale_cuda` or
  `vpp_qsv`, so hardware conversion silently fell back to the CPU path. The
  bundled runtime is now renamed to globally unique DLL names
  (`cvfilter-11.dll`, `cvutil-60.dll`, …, patched in the PE name strings —
  the same effect as an FFmpeg `--build-suffix` build), so CoreVideo always
  loads its own full-featured FFmpeg on every OBS version with zero
  possibility of colliding with OBS's. A new automated test loads the
  renamed runtime, asserts `scale_cuda`/`vpp_qsv` are present, and asserts
  no original-named FFmpeg module leaks into the process.

## [0.1.30] - 2026-07-31

### Fixed
- **OBS 32.2 compatibility: OBS no longer breaks after installing CoreVideo.**
  OBS Studio 32.2 upgraded its bundled FFmpeg to major version 8, which uses
  the same DLL filenames (`avcodec-62.dll`, `avutil-60.dll`, …) as the FFmpeg
  runtime CoreVideo shipped loose into `obs-plugins\64bit`. On OBS 32.2+ those
  loose copies shadowed OBS's own DLLs, so OBS's built-in `obs-ffmpeg` module
  (and CoreVideo itself) failed to load, taking down recording/streaming
  encoders with it. The FFmpeg runtime now lives in a private
  `obs-plugins\64bit\corevideo-ffmpeg\` directory and is delay-loaded through
  a resolver that binds whatever FFmpeg the OBS process already has (OBS
  32.2+) or CoreVideo's bundled copy (OBS ≤ 32.1) — never a mix. The
  installer removes the legacy loose DLLs on upgrade; zip users should delete
  `av*-*.dll` / `sw*-*.dll` from `obs-plugins\64bit` manually. If a usable
  FFmpeg runtime cannot be bound, hardware-accelerated conversion falls back
  to the CPU path with a clear log message instead of failing.

## [0.1.29] - 2026-07-30

### Fixed
- Setting an output's assignment to "None" now actually stops the previous
  feed: the engine subscription is torn down and the signal readout clears.
  Previously the old participant kept streaming into shared memory (a live
  signal on a "None" row) and could not be cleaned up without restarting
  the engine.
- Unassigned outputs are no longer graded as stale/waiting or offered
  Recover actions in the Output Manager.
- Sign-in and join now always present production Zoom credentials: the
  production OAuth broker URL is baked into every build (locally-built
  releases previously embedded a blank broker identity, which allowed
  leftover developer credential overrides on tester machines), and the
  broker's Meeting SDK token service was updated to production credentials
  server-side.

## [0.1.28] - 2026-07-30

First public beta release.

### Added
- In-app update check: the Zoom Control dock shows a non-intrusive banner
  when a newer CoreVideo release is available, with a toggle in Settings to
  disable the startup check.
- The plugin version is now shown in the Settings dialog.
- Beta support surface: GitHub bug-report/feature-request templates (with
  support-bundle instructions), a much larger Troubleshooting section in the
  docs, and a documented flow for adding/removing CoreVideo on a Zoom account.
- macOS groundwork: the real plugin and OAuth helper now build on Apple
  Silicon CI and OAuth tokens use the macOS Keychain — no macOS packages are
  published yet; Windows x64 remains the supported platform.

### Fixed
- Frozen-frame-forever after an engine crash/restart: shared-memory regions
  now carry a generation stamp so sources re-attach automatically, shared
  memory failures are surfaced as visible errors instead of dropped
  silently, and region counts are capped with a clear "capacity" error.
- Changing the OSC port no longer leaks the old poll timer and stack
  duplicate handlers.
- Leaving a meeting now clears the stored recovery session (including join
  tokens) instead of keeping it in memory until the next join.
- Bitfocus Companion module builds again against @companion-module/base 2.1.

### Changed
- Sign-in/join now runs through one centralized, unit-tested decision path:
  every join attempt logs a single `[join-decision]` line and failures map
  to distinct, actionable messages (expired token, wrong environment,
  missing approval, and so on).
- Zoom Meeting SDK updated from 7.0.2 to 7.1.5.
- README documents the honest platform support matrix, and non-Windows
  builds now log a prominent warning that OAuth tokens are stored without
  OS-level encryption (Windows DPAPI is unchanged).
- Documentation architecture diagrams rebuilt for readability (dark theme,
  legible text, a hand-drawn system overview), fixing a content-security
  policy issue that made production render them with Mermaid's light theme.
- Test suite grew from 2 to 17 suites (IPC hardening, control/OSC parsing,
  reconnect backoff and cancellation, join decisions, version comparison),
  and the dock lifecycle smoke script now asserts reopen and shutdown
  ordering.

## [0.1.27] - 2026-07-29

### Fixed
- Output Manager table rows: preview thumbnails, assignment/quality/audio
  dropdowns, and labels now share one vertical baseline per row instead of
  three different alignments, with tighter row heights.

### Changed
- Documentation site rebuilt in the app's broadcast-console design language,
  with real plugin screenshots replacing placeholder boxes, fixed Mermaid
  diagram contrast/legibility, a shared header/footer across doc pages, and
  the OAuth setup page removed in favor of the updated Plugin Docs.
  `corevideo.io` is now the primary domain with Cloudflare cache purge on
  deploy.

## [0.1.26] - 2026-06-13

### Fixed
- Output Manager live-refresh stability: the table and its controls no
  longer disrupt an in-progress row selection or edit while background
  refreshes are running.

## [0.1.25] - 2026-06-13

### Changed
- The `ZoomObsEngine` helper process console window is now hidden, so
  joining a meeting no longer flashes a visible terminal window on Windows.

## [0.1.24] - 2026-06-13

### Added
- Video source subscriptions are now capped at a bounded maximum, preventing
  unbounded growth when many sources are reconfigured in a session.

### Fixed
- Windows CI can now run without the LGPL FFmpeg asset present, and the
  documentation site deploy step no longer hard-fails when Cloudflare
  credentials are not configured.

## [0.1.23] - 2026-06-13

### Added
- Unit test coverage for `SpeakerDirector`, output health, IPC parsing, and
  reconnect logic.
- IPC heartbeat to detect a hung Zoom engine process and trigger recovery.
- CI hardening: a Linux build, hard-fail static analysis, and Companion
  (Bitfocus) integration tests.

### Fixed
- IPC write failures are now detected and recovered from instead of leaving
  the engine connection silently stuck.
- Engine SDK calls are now null-checked, and reconnect jitter/session
  persistence was corrected.
- ISO recorder failure handling hardened against partial/failed encodes.
- Windows CI package validation is skipped (instead of failing) when the
  restricted Zoom SDK is unavailable to the build.

### Docs
- Explored Spout/Syphon GPU texture sharing as a future high-density video
  transport (see `docs/GPU_TEXTURE_SHARING_RESEARCH.md`).

## [0.1.22] - 2026-05-26

### Added
- Windows installer packaging (`CoreVideo-Setup-vX.Y.Z.exe` via NSIS),
  produced alongside the existing ZIP package by `release-local.ps1` and the
  release workflow.

## [0.1.21] - 2026-05-26

### Added
- ISO recorder encoder selection (CPU/GPU H.264) plus per-feed diagnostics
  in the ISO Recorder dock.

## [0.1.20] - 2026-05-23

### Fixed
- Reopening the Zoom Control dock after closing it no longer leaves the
  panel in a broken state.

## 0.1.0 - 0.1.19 - Early development

Initial development of the OBS plugin, the `ZoomObsEngine` helper process,
and the surrounding tooling: Zoom Meeting SDK raw video/audio capture over a
named-pipe/shared-memory IPC bridge, the dockable Qt control panel, OAuth PKCE
sign-in, per-participant and screen-share sources, the Active Speaker
Director, auto-reconnect, TCP/OSC control APIs, ISO recording, the Output
Manager, and the initial Windows release packaging and CI pipeline.

[Unreleased]: https://github.com/iamfatness/CoreVideo/compare/v0.1.37...HEAD
[0.1.37]: https://github.com/iamfatness/CoreVideo/compare/v0.1.36...v0.1.37
[0.1.27]: https://github.com/iamfatness/CoreVideo/compare/v0.1.26...v0.1.27
[0.1.26]: https://github.com/iamfatness/CoreVideo/compare/v0.1.25...v0.1.26
[0.1.25]: https://github.com/iamfatness/CoreVideo/compare/v0.1.24...v0.1.25
[0.1.24]: https://github.com/iamfatness/CoreVideo/compare/v0.1.23...v0.1.24
[0.1.23]: https://github.com/iamfatness/CoreVideo/compare/v0.1.22...v0.1.23
[0.1.22]: https://github.com/iamfatness/CoreVideo/compare/v0.1.21...v0.1.22
[0.1.21]: https://github.com/iamfatness/CoreVideo/compare/v0.1.20...v0.1.21
[0.1.20]: https://github.com/iamfatness/CoreVideo/compare/v0.1.19...v0.1.20
