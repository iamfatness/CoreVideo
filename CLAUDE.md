# CLAUDE.md

Project notes for Claude Code sessions working in this repository: CoreVideo,
the OBS Studio plugin that pulls Zoom meeting video and audio into OBS as
native sources. Current release: **v0.1.41** (2026-08-18). Update this file in
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
- **Director handover** (`src/director-handover.h`): on an Active Speaker cut
  the hidden preview covers air until the main subscription delivers the
  participant we cut TO; exactly one of the two slots publishes audio at any
  instant (gate polarity in `on_engine_audio` / `on_director_preview_audio`).
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
