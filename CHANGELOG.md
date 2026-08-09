# Changelog

All notable changes to CoreVideo are documented in this file. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); releases
are tagged `vMAJOR.MINOR.PATCH` and published as
[GitHub Releases](https://github.com/iamfatness/CoreVideo/releases).

## [Unreleased]

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

[Unreleased]: https://github.com/iamfatness/CoreVideo/compare/v0.1.27...HEAD
[0.1.27]: https://github.com/iamfatness/CoreVideo/compare/v0.1.26...v0.1.27
[0.1.26]: https://github.com/iamfatness/CoreVideo/compare/v0.1.25...v0.1.26
[0.1.25]: https://github.com/iamfatness/CoreVideo/compare/v0.1.24...v0.1.25
[0.1.24]: https://github.com/iamfatness/CoreVideo/compare/v0.1.23...v0.1.24
[0.1.23]: https://github.com/iamfatness/CoreVideo/compare/v0.1.22...v0.1.23
[0.1.22]: https://github.com/iamfatness/CoreVideo/compare/v0.1.21...v0.1.22
[0.1.21]: https://github.com/iamfatness/CoreVideo/compare/v0.1.20...v0.1.21
[0.1.20]: https://github.com/iamfatness/CoreVideo/compare/v0.1.19...v0.1.20
