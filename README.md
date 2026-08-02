# CoreVideo

> **ZOOM RAW DATA AND BANDWIDTH LIMITS - READ BEFORE USE**
>
> CoreVideo uses the Zoom Meeting SDK raw data APIs. Raw data access is available
> through Meeting SDK apps; negotiated quality follows the signed-in Zoom account
> and app entitlements. Standard accounts are typically constrained by a 30 Mbps
> incoming video budget, while Enhanced Media / HBM can raise that envelope to
> roughly 100 Mbps. At about 4-6 Mbps per standard 1080p stream, several feeds
> can work without EM; with EM/HBM, plan around up to 16 standard 1080p feeds or
> about 8 high-bitrate/60 fps feeds before hitting the downlink budget.
>
> Contact your Zoom account representative or visit
> [Zoom Plans](https://zoom.us/pricing) to verify production quality,
> bandwidth, and developer/app entitlements before deploying this plugin.

**OBS Studio plugin for live Zoom meeting video, audio, screen share, and Zoom interpretation audio channel capture.**

CoreVideo integrates the Zoom Meeting SDK into OBS - no screen capture or virtual camera required. The SDK runs in a dedicated `ZoomObsEngine` child process; the OBS plugin communicates with it through a `ZoomEngineClient` singleton over cross-platform IPC (named pipes on Windows, Unix sockets on macOS/Linux) with frame data delivered through named shared memory. A built-in dockable control panel manages joining, and a `ZoomReconnectManager` handles automatic recovery after crashes or disconnects.

Docs: **[Full Documentation & Architecture Diagrams ->](https://corevideo.iamfatness.us/documentation/)**
Guide: **[Core Plugin Guide & Examples ->](https://corevideo.iamfatness.us/core-plugin/)**
Operator Quickstart: **[Install, sign in, assign outputs, record ISO ->](docs/OPERATOR_QUICKSTART.md)**
Changelog: **[Release notes & version history ->](CHANGELOG.md)**

---

## Features

- **Raw video capture** - I420 YUV, selectable 360p / 720p / 1080p resolution
- **Hardware-accelerated video** - optional FFmpeg I420->NV12 conversion via CUDA, VAAPI, VideoToolbox, or QSV (`-DCOREVIDEO_HW_ACCEL=ON`)
- **Video loss mode** - hold last frame or show black when a feed drops; shows color-bar placeholder before first frame
- **Raw audio capture** - 48 kHz PCM, mono or stereo, with per-participant audio isolation and mixer routing
- **Auto ISO recording** - record assigned participant/active-speaker/spotlight outputs to separate FFmpeg-encoded MP4 files with matching PCM WAV audio, plus optional main OBS program recording
- **ISO capacity guidance** - recorder panel estimates ISO/program encode paths and warns when CPU or GPU encoder pressure is likely
- **Assignment modes** - each source independently follows: a fixed participant, the active speaker, a ZoomISO-style spotlight slot (1-8), or the active screen share
- **Failover participant** - configure a secondary participant that activates automatically when the primary leaves
- **Active Speaker Director** - configurable sensitivity + hold-time switching, manual take/release supersede, and a dedicated `CoreVideo Active Speaker` OBS source for clean speaker-follow output
- **Spotlight / ZoomISO** - subscribe a source to Spotlight 1-8; engine resolves which participant is spotlighted
- **Screen share capture** - source subscribes to the active meeting screen-share feed
- **Zoom interpretation audio channel capture** - dedicated OBS source for existing Zoom interpretation audio channels
- **Per-participant audio sources** - standalone OBS audio source per meeting participant
- **Webinar support** - join Zoom Webinars using the dedicated SDK entry point (Webinar checkbox in control dock)
- **Participant roster** - live list with video, mute, talking, host, co-host, raised hand, spotlight slot, and screen-sharing state
- **Control dock** - dockable Qt panel with animated status dot, join/leave, token-type selector, recovery countdown, Active Speaker Director controls, and a routing section that opens the dedicated Output Manager; persists last meeting ID and display name across sessions
- **Update check** - once per OBS session, a plain unauthenticated GET to the public GitHub Releases API checks for a newer CoreVideo release; if one exists, a dismissible `CvBanner` notice appears in the Zoom Control dock linking to the release page. Never auto-downloads, never blocks startup, fails completely silently offline, and can be turned off via **Settings -> Check for updates on startup**
- **Diagnostics dock** - dockable OBS panel showing requested vs observed resolution, FPS, frame age, retry counts, recent engine debug events, ISO/FFmpeg recorder status, and a redacted support-bundle zip/folder export with a scrubbed OBS log excerpt for live troubleshooting
- **Auto-reconnect** - exponential back-off recovery after engine crash, network drop, or unexpected disconnect
- **Recovery cancel** - the dock, TCP API, and OSC cancel paths stop the engine,
  clear the stored join session, and cancel pending reconnect timers so a retry
  loop cannot restart itself after cancellation
- **OBS hotkeys** - per-source hotkeys to enable/disable active speaker mode
- **TCP control API** - JSON server on `127.0.0.1:19870` for scripts and dashboards; includes `oauth_callback` command for custom URL scheme forwarding
- **OSC control API** - UDP OSC server on `127.0.0.1:19871` for lighting consoles and broadcast hardware
- **Output profiles** - save and load named participant-to-source mappings as JSON files
- **Output manager dock** - dockable OBS panel and API for viewing and reconfiguring all sources at runtime, including active speaker, screen-share, and Spotlight 1-8 assignments with roster markers
- **Public Client Meeting SDK authentication** - published builds pass the Marketplace Public Client ID as `AuthContext.publicAppKey`; no Meeting SDK secret is shipped in the desktop app
- **Zoom OAuth PKCE** - user-level OAuth 2.0 with PKCE (public client, no desktop secret) for attributed joins and Marketplace compliance; the broker start URL is baked in at build time; `corevideo://` custom URL scheme with platform callback helpers (`CoreVideoOAuthCallback.exe` / `.app`); DPAPI token protection on Windows (plaintext with a logged warning elsewhere - see [Security](#security))
- **Visible Zoom Meeting SDK window** - the helper process uses Zoom's default Meeting SDK UI so operators can admit waiting-room participants, start self video/audio, and use normal in-meeting controls while OBS receives raw feeds
- **SDK 5.17.x and 7.x** - auto-detects flat and subfolder header layouts
- **Hardened security** - constant-time token comparison, validated IPC input, sanitised participant IDs, SIGPIPE handling
- **Modern UI** - CoreVideo stylesheet with dark theme, animated `CvStatusDot`, `CvBanner` first-run notices, and button role variants (primary / danger)
- **Platform support** - Windows 10/11 x64 is the only supported, packaged, and CI-released target. The same CMake project also configures on Windows arm64, macOS, and Linux, so source builds are possible there, but they are unsupported/experimental: no official installers or ZIPs are published for them today. See [Platform Support](#platform-support) below.

## Requirements

| Dependency | Version | Notes |
|---|---|---|
| OBS Studio | 30+ | `libobs` + `obs-frontend-api` |
| CMake | 3.16+ | Build system |
| Qt | 6.x | Core + Network + Widgets |
| FFmpeg | Runtime executable | Required for auto ISO recording. Must be on `PATH` or supplied via `ffmpeg_path`. |
| Zoom Meeting SDK | **5.17.x / 7.x** | Source builds only: place in `third_party/zoom-sdk/`. Official Windows release downloads bundle the runtime files needed by end users. |
| C++ compiler | C++17 | MSVC 2022 / Clang 14+ / GCC 11+ |
| Zoom Developer Account | - | Marketplace app with Public Client OAuth + PKCE and Meeting SDK / Embed enabled for the same environment. |

## Platform Support

| Platform | Status |
|---|---|
| **Windows 10/11 x64** | **Supported.** The only platform with a maintained release pipeline (`.github/workflows/release-windows.yml`): checksummed ZIP and NSIS installer, published to GitHub Releases. This is the only configuration the maintainers build, test, and run in production. |
| Windows arm64 | Source build only, untested. `CMakeLists.txt` auto-detects an arm64 Zoom SDK layout under `third_party/zoom-sdk/arm64` if present, but there is no arm64 CI job, no arm64 release artifact, and no maintainer testing on arm64 hardware. Treat it as "may compile," not "known to work." |
| macOS (arm64 / x86_64) | Source build only, unsupported - **no official packages**. CI (`build.yml`, macOS job) only compiles the cross-platform engine/plugin sources with `-DCOREVIDEO_BUILD_PLUGIN=OFF -DCOREVIDEO_BUILD_ENGINE=OFF` to catch portability regressions; it does not build, link, or package the actual OBS plugin, and the CI artifact it produces is a compile-validation ZIP, not an installable macOS build (see below). There is no macOS Keychain-backed token storage yet (see Security), no notarization/signing, and no macOS QA. |
| Linux | Source build only, unsupported - **no official packages**. CI (`build.yml`, Linux job) compiles and unit-tests only the cross-platform C++ (`BUILD_TESTING`) with the plugin/engine/sidecar all `OFF`; it never links against Qt6, OBS, or the Zoom SDK on Linux. There is no Linux packaging, no distro integration, and no libsecret-backed token storage yet (see Security). |

The CMake project genuinely supports configuring on all of the above (see `buildspec/macos.cmake`, the Unix-socket IPC path in `engine-ipc.h`, and `oauth-callback-helper-macos.mm`), so a source build is *possible* on macOS/Linux/Windows-arm64 - it is just not something the project packages, tests, or supports today. If you get one working, bug reports and PRs are welcome, but expect to do your own SDK/Qt/OBS wiring.

## Beta Status

CoreVideo is in **public beta**. The only supported, packaged release target is
**Windows x64** (installer or ZIP - see [Platform Support](#platform-support)
above); the beta installer is not yet code-signed, so Windows SmartScreen will
flag it on first run (see [Troubleshooting](https://corevideo.io/documentation/#troubleshooting)).
Known limitations and what's planned next are tracked in the
**[Roadmap](docs/ROADMAP.md)**. To report a bug or request a feature, open a
[GitHub issue](https://github.com/iamfatness/CoreVideo/issues/new/choose) -
the bug report template walks you through attaching a redacted support bundle
from the Zoom Diagnostics dock. New releases are announced through a
dismissible in-app update banner in the Zoom Control dock (a plain,
unauthenticated check against the public GitHub Releases API, once per OBS
session) and on the [Releases page](https://github.com/iamfatness/CoreVideo/releases).

## Quick Start

1. **For source builds, get the Zoom SDK** - download from the [Zoom Developer Portal](https://developers.zoom.us/docs/meeting-sdk/releases/) and place it at `third_party/zoom-sdk/`. CMake auto-detects x64/arm64/x86 sub-layouts on Windows. End users installing an official Windows release do not need to download or provide SDK files.

2. **Configure & build**
   ```sh
   cmake -B build \
     -DCMAKE_BUILD_TYPE=Release \
     -DZOOM_SDK_DIR=third_party/zoom-sdk \
     -DCMAKE_PREFIX_PATH="/path/to/obs-studio;/path/to/Qt6"

   cmake --build build --config Release
   ```

   On Windows, run CMake from a Visual Studio Developer PowerShell or use an
   explicit Visual Studio generator:
   ```powershell
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
     -DZOOM_SDK_DIR=third_party/zoom-sdk `
     -DCMAKE_PREFIX_PATH="C:/path/to/obs-studio-build;C:/path/to/Qt/6.x/msvc2022_64"

   cmake --build build --config Release
   ```

   To validate the Zoom SDK helper process before wiring up OBS and Qt, build
   only the engine:
   ```powershell
   cmake -S . -B build-engine -G "Visual Studio 17 2022" -A x64 `
     -DCOREVIDEO_BUILD_PLUGIN=OFF `
     -DZOOM_SDK_DIR=third_party/zoom-sdk

   cmake --build build-engine --config Release --target ZoomObsEngine
   ```

   If MSBuild reports `Item has already been added. Key in dictionary: 'Path'
   Key being added: 'PATH'`, normalize the process environment before running
   CMake from PowerShell:
   ```powershell
   Remove-Item Env:PATH -ErrorAction SilentlyContinue
   $env:Path = "C:\Program Files\CMake\bin;C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem;C:\Windows\System32\WindowsPowerShell\v1.0"
   ```

   A normal OBS installation under `C:\Program Files\obs-studio` contains the
   runtime binaries, not the development CMake packages. For the full plugin
   build, `CMAKE_PREFIX_PATH` must include an OBS build/install tree that
   contains `LibObsConfig.cmake` and `obs-frontend-apiConfig.cmake`, plus a
   matching Qt 6 MSVC package.

   Windows builds must ship Qt's TLS backend plugins (`obs-plugins/64bit/plugins/tls/`)
   for OAuth HTTPS requests to succeed.

3. **Install into OBS**
   ```sh
   cmake --install build --prefix "/path/to/obs-studio"
   ```

4. **Configure local settings** - published builds already contain the CoreVideo broker URL and do not require end users to enter Zoom app credentials. Open OBS -> **Tools -> Zoom Plugin Settings** to sign in with Zoom, configure the local control-server token/ports, and manage reconnect behavior. Developer-only builds can still use local credential overrides when the embedded broker identity is blank.

### Windows release packaging

GitHub Actions can validate the Windows build without the restricted Zoom
runtime, but public client releases must include `ZoomObsEngine.exe`,
`zoom-runtime\sdk.dll`, Qt TLS plugins, and the other bundled runtime files.
CI fetches the license-restricted Zoom SDK from an asset on a **draft** GitHub
release in this repository (draft releases are not publicly visible; the
workflow token downloads the asset at build time). If the asset is missing, CI
skips publishing a GitHub Release instead of shipping an incomplete package.
Keep that release a draft — publishing it would make the SDK public.

For fast local releases from a machine that already has the Zoom runtime, use:

```powershell
.\scripts\release-local.ps1 -Version v0.1.6 -Upload
```

When NSIS is installed, the release script also creates and uploads
`CoreVideo-Setup-vX.Y.Z.exe`. This is the recommended end-user installer: it
verifies the staged plugin/runtime files, detects a standard OBS Studio install
path, requires OBS to be closed, installs the files, verifies the installed
runtime, and registers an uninstaller in Windows Apps & Features. The ZIP
remains available for manual or advanced installs. Each release asset also gets
a matching `.sha256` checksum file. Pass `-SkipInstaller` if you only want the
ZIP package.

If an FFmpeg shared development tree exists at `C:\ffmpeg`, local releases
automatically enable hardware I420->NV12 conversion and bundle the FFmpeg DLLs
beside the OBS plugin. Pass `-DisableFfmpegHwAccel` to force a CPU-only build.

Useful options:

```powershell
.\scripts\release-local.ps1 -Version v0.1.6 -Install -ObsInstallPath "C:\Program Files\obs-studio"
.\scripts\release-local.ps1 -Version v0.1.6 -BuildPath build-nmake -Upload
```

The script builds, installs into a staging folder, validates the plugin, OAuth
callback helper, Qt/TLS files, locale data, embedded public OAuth / Meeting SDK
app identity, sidecar templates/looks, Zoom SDK runtime files, and any bundled
FFmpeg runtime, creates a ZIP under `dist/`, optionally creates the NSIS setup
EXE, and optionally uploads both assets plus their `.sha256` files to the
matching GitHub Release. Local `-Install` refuses to copy into OBS while
`obs64.exe` is running, matching the installer guard.

`-Version` (with `-Configure`, or against an already-configured build
directory) also passes `-DCOREVIDEO_RELEASE_VERSION` to CMake, so the built
plugin reports that exact release tag at runtime (logs, the Diagnostics
support bundle, and the in-app update check) instead of the `project()`
placeholder version in `CMakeLists.txt`, which is only bumped by hand and
drifts behind real releases.

### OBS scene smoke test

Use the OBS smoke test when validating plugin-created OBS scene graph behavior
on a machine with OBS running and obs-websocket enabled. It does not exercise
the optional Sidecar UI. The script connects directly to obs-websocket v5,
can verify that CoreVideo input kinds are registered, creates a deterministic
CoreVideo test scene, links participant sources through nested slot scenes,
creates the screen-share source/nested scene, switches OBS to the test scene,
and audits that the expected scenes, inputs, placeholders, and scene items are
present.

```powershell
.\scripts\obs-scene-smoke-test.ps1 -VerifyCoreVideoPlugin
```

If obs-websocket has a password, pass it explicitly or use the environment:

```powershell
$env:OBS_WEBSOCKET_PASSWORD = "your-websocket-password"
.\scripts\obs-scene-smoke-test.ps1 -VerifyCoreVideoPlugin -ParticipantCount 8
```

To verify an already-created scene graph without creating or modifying sources:

```powershell
.\scripts\obs-scene-smoke-test.ps1 -AuditOnly -VerifyCoreVideoPlugin -SceneName "CoreVideo Smoke Test"
```

### Load measurement

Use the load measurement script during 8-feed testing after OBS is in the
target state. It samples `obs64`, `ZoomObsEngine`, and `ffmpeg`, then writes raw
samples, CSV summary, JSON summary, and warnings when required processes are
missing, disappear during the run, or the expected ISO recorder count is not
present for every sample.

```powershell
.\scripts\Measure-CoreVideoLoad.ps1 -DurationSeconds 1800 -SampleSeconds 5 `
  -ExpectedFeeds 8 -ExpectedIsoRecorders 8 -RequireObs
```

To audit plugin load and dock registration markers, pass the current OBS log
path. After manually opening docks in OBS, include their dock IDs in
`-ExpectedDockId`. Add `-ExpectDockShow` after exercising **Tools > Zoom
Control**, **Tools > Zoom Output Manager**, **Tools > Zoom Diagnostics**, and
**Tools > Zoom ISO Recorder** to prove the reopen path logged each dock show:

```powershell
.\scripts\obs-scene-smoke-test.ps1 -AuditOnly -VerifyCoreVideoPlugin `
  -ObsLogPath "$env:APPDATA\obs-studio\logs\latest.log" `
  -ExpectedDockId ZoomControlDock,ZoomOutputManagerDock,ZoomDiagnosticsDock,ZoomIsoRecorderDock `
  -ExpectDockShow
```

After closing OBS, run the log-only lifecycle audit against the same log to
verify that CoreVideo registered the docks and reached its shutdown path without
obvious crash markers:

```powershell
.\scripts\obs-scene-smoke-test.ps1 -LogOnly -ExpectShutdown `
  -ObsLogPath "$env:APPDATA\obs-studio\logs\latest.log" `
  -ExpectedDockId ZoomControlDock,ZoomOutputManagerDock,ZoomDiagnosticsDock,ZoomIsoRecorderDock `
  -ExpectDockShow
```

To exercise the **repeated open/close** acceptance criterion, close and reopen
each dock several times in one OBS session, then assert each dock logged at
least N shows with `-MinDockShows N`. With `-ExpectShutdown` the audit also
enforces clean teardown ordering: it fails if any dock is (re)registered or
shown *after* the shutdown marker, or if a dock was ever shown without an OBS
host wrapper (the blank/detached-panel symptom), or if a crash marker appears:

```powershell
.\scripts\obs-scene-smoke-test.ps1 -LogOnly -ExpectShutdown `
  -ObsLogPath "$env:APPDATA\obs-studio\logs\latest.log" `
  -ExpectedDockId ZoomControlDock,ZoomOutputManagerDock,ZoomDiagnosticsDock,ZoomIsoRecorderDock `
  -ExpectDockShow -MinDockShows 2
```

5. **Set up OAuth (for Marketplace / external-account joins)** - publishers configure the Cloudflare broker and bake `-DZOOM_EMBED_OAUTH_AUTHORIZATION_URL=https://corevideo.iamfatness.us/oauth/start` into the build. End users just open the Settings dialog and click **Sign in with Zoom**. See [`docs/ZOOM_MARKETPLACE_OAUTH.md`](docs/ZOOM_MARKETPLACE_OAUTH.md) for the full walkthrough.

6. **Join once, then assign outputs** - use the CoreVideo dock or the TCP/OSC control APIs to join the meeting once per OBS session. Then add **Zoom Participant**, **Zoom Participant Audio**, **Zoom Share**, or **Zoom Interpretation Audio** sources and assign them to participants, active speaker, screen share, or Spotlight 1-8 dynamic roles.

## Zoom OAuth PKCE

CoreVideo uses user-level OAuth 2.0 with PKCE through the CoreVideo HTTPS broker for attributed meeting joins and Zoom App Marketplace compliance. Published builds do not ship OAuth or Meeting SDK secrets in the OBS plugin. The broker performs the Zoom token exchange with Public Client OAuth; the helper process authenticates the Meeting SDK with the same Marketplace Public Client ID as `AuthContext.publicAppKey`.

### Build-time configuration (publisher, one-time)
Pass the broker identity at CMake configure time:
```
cmake -B build \
  -DZOOM_EMBED_OAUTH_CLIENT_ID=y6sIWSwiTZe1JygMx4C9EQ \
  -DZOOM_EMBED_OAUTH_AUTHORIZATION_URL=https://corevideo.iamfatness.us/oauth/start \
  -DZOOM_EMBED_MEETING_SDK_PUBLIC_APP_KEY=y6sIWSwiTZe1JygMx4C9EQ ...
```
These values are compiled into the plugin and used for every install of that build. There is no UI for entering production credentials, and embedded values override stale `global.ini` entries. Current production builds use the Public Client ID for OAuth and pass the same value to the Meeting SDK as `AuthContext.publicAppKey`.

### Flow
1. In **Tools -> Zoom Plugin Settings**, click **Authorize with Zoom** (no IDs to enter). The plugin registers the `corevideo://` URL scheme automatically on first use.
2. The browser opens at `https://corevideo.iamfatness.us/oauth/start`; the broker generates the PKCE verifier/challenge and redirects to Zoom.
3. Zoom redirects to `https://corevideo.iamfatness.us/oauth/callback`; the broker returns a short-lived broker token to `corevideo://oauth/callback`.
4. `CoreVideoOAuthCallback.exe` (Windows) or `CoreVideoOAuthCallback.app` (macOS) forwards the URL to the plugin via the TCP control server (`oauth_callback` command).
5. The plugin verifies state, redeems the broker token over HTTPS, and persists access + refresh tokens. On Windows, tokens are DPAPI-protected before storage; on macOS/Linux they are currently stored in plaintext with a logged warning (see [Security](#security)).
6. Before each meeting join, CoreVideo refreshes the token if needed and fetches the signed-in user's ZAK. The Zoom helper process initializes the SDK with `AuthContext.publicAppKey` set to the embedded Public Client ID and `AuthContext.jwt_token` set to null, then joins with the ZAK.

See [`docs/ZOOM_MARKETPLACE_OAUTH.md`](docs/ZOOM_MARKETPLACE_OAUTH.md) for the full setup guide and security notes.

## Control APIs

### TCP JSON (port 19870)

```sh
# Meeting status
echo '{"cmd":"status"}' | nc 127.0.0.1 19870

# List participants with video/mute/talking state
echo '{"cmd":"list_participants"}' | nc 127.0.0.1 19870

# Reassign source to participant at runtime
echo '{"cmd":"assign_output","source":"Zoom Participant 1","participant_id":123,"isolate_audio":true,"audio_channels":"stereo"}' | nc 127.0.0.1 19870

# Forward OAuth callback URL from custom scheme helper
echo '{"cmd":"oauth_callback","url":"corevideo://oauth/callback?broker_token=...&state=..."}' | nc 127.0.0.1 19870
```

Commands: `help`, `status`, `list_participants`, `list_outputs`, `assign_output`, `assign_output_ex`, `recover_stale_outputs`, `upgrade_low_quality_outputs`, `join`, `leave`, `oauth_callback`, `iso_recording_start`, `iso_recording_stop`, `iso_recording_status`, `speaker_director_status`, `speaker_director_configure`, `speaker_director_take`, `speaker_director_release`.

### Auto ISO Recording

Use **OBS -> Tools -> Zoom ISO Recorder** for the operator UI. The dock provides
an output-folder picker, FFmpeg path/test controls, CPU/GPU H.264 encoder
selection, a program-recording toggle, Start/Stop buttons, disk-space warnings,
encoder capacity guidance, and a live table of active ISO sessions and file
paths. Recently completed sessions remain visible after stop so operators can
verify the MP4/WAV outputs without browsing the folder manually. GPU encoders
lower CPU use but consume one encoder session per ISO feed;
8 ISO feeds plus an OBS program stream may require using CPU x264 for one path.
If a selected hardware encoder is missing from the FFmpeg build, CoreVideo falls
back to `libx264` when available and shows the requested vs actual encoder in
the ISO panel and TCP status.
The recorder blocks starts below 2 GB free, warns below 10 GB free, and surfaces
FFmpeg process errors in the session table and TCP/OSC status JSON.

```sh
# Start ISO recording. record_program=true also starts OBS program recording.
echo '{"cmd":"iso_recording_start","output_dir":"C:/Recordings/CoreVideo","ffmpeg_path":"ffmpeg","record_program":true}' | nc 127.0.0.1 19870

# Inspect active ISO sessions and output file paths.
echo '{"cmd":"iso_recording_status"}' | nc 127.0.0.1 19870

# Stop ISO recording.
echo '{"cmd":"iso_recording_stop"}' | nc 127.0.0.1 19870
```

Each active source segment writes one `*.mp4` video file and one matching `*.wav` PCM audio file. A new segment starts when the resolved participant or source resolution changes.

### UDP OSC (port 19871)

| Address | Type tags | Action |
|---|---|---|
| `/zoom/status` | - | Reply: meeting state + active speaker |
| `/zoom/list_participants` | - | Reply: one legacy `/zoom/participant` plus one `/zoom/participant/detail` per user; detail includes host/co-host, raised hand, spotlight, and screen-share flags |
| `/zoom/list_outputs` | - | Reply: one `/zoom/output` per source |
| `/zoom/recover_stale_outputs` | `[,i]` | Retry stale video outputs; optional `1` forces cooldown bypass |
| `/zoom/upgrade_low_quality_outputs` | `[,i]` | Retry outputs below requested resolution; automatic retries continue with capped backoff and skip feeds already at the requested size |
| `/zoom/speaker_director/status` | - | Reply with directed/raw/candidate/manual speaker and timing state |
| `/zoom/speaker_director/configure` | `[,iiiii]` | Set sensitivity ms, hold ms, optional require-video flag, and up to two excluded participant IDs |
| `/zoom/speaker_director/take` | `[,i]` | Manually hold a participant as the directed speaker |
| `/zoom/speaker_director/release` | - | Return the director to automatic switching |
| `/zoom/join` | `,sss` | meeting_id, passcode, display_name |
| `/zoom/leave` | - | Leave meeting |
| `/zoom/assign_output` | `,si[i]` | source, participant_id, [active_speaker] |
| `/zoom/assign_output/active_speaker` | `,s` | source |
| `/zoom/isolate_audio` | `,si` | source, 0\|1 |
| `/zoom/iso/start` | `[,ssi]` | optional output directory, video encoder, record-program flag |
| `/zoom/iso/status` | - | Reply with active count, completed count, requested/actual encoder, fallback, hardware, disk warning, and recorder warning |
| `/zoom/iso/stop` | - | Stop ISO recording |

`/zoom/status` also replies with `/zoom/status/screen_share ,is` containing
the active sharing participant ID and display name, or `0` and an empty name
when nobody is sharing. Subscribers also receive `/zoom/event/screen_share ,is`
when the active sharer changes.

## Active Speaker Mode

CoreVideo has two active-speaker workflows:

- Set a normal **Zoom Participant** source to **Active Speaker** assignment mode when that source should follow the directed speaker.
- Add the dedicated **CoreVideo Active Speaker** source when you want a single speaker-follow OBS source. It uses a two-slot handoff internally: the current participant stays visible while the next participant warms on a hidden slot, then the source cuts only after a valid frame is available.

The **Active Speaker Director** in the Zoom Control dock decides which participant is directed. It tracks the raw Zoom speaker, candidate speaker, directed speaker, last directed speaker, and any manual supersede. The dock also shows a plain-language status line so the operator can see whether CoreVideo is waiting, holding the current speaker, evaluating a candidate, or locked by manual supersede.

### Debounce

Two independent timers prevent rapid camera cuts:

| Parameter | Default | Description |
|---|---|---|
| **Sensitivity** (`speaker_sensitivity_ms`) | 500 ms | New speaker must hold the floor continuously for this long before the switch fires. A different speaker speaking resets the clock. |
| **Hold** (`speaker_hold_ms`) | 2 000 ms | After any switch, no further switch occurs for at least this long. |

The effective delay before each switch is `max(hold_remaining, sensitivity_remaining)`. If the delay is zero the switch fires immediately; otherwise a background thread sleeps for the delay and re-evaluates on the OBS UI thread.

### Safety

- **Liveness flag** - a `shared_ptr<atomic<bool>>` captured in every in-flight lambda ensures deferred callbacks bail safely if the source is destroyed before the timer fires.
- **Supersede logic** - a new candidate replaces the pending one, restarting the sensitivity clock. Stale callbacks silently discard themselves.
- **Final verification** - before committing a switch the code re-checks that the candidate is still the active speaker, so no switch fires for someone who stopped talking during the hold window.
- **UI-thread commitment** - all state mutations run on the OBS UI thread via `obs_queue_task`, preventing data races with the properties panel.

### Audio isolation interaction

When **Isolate Audio** is also enabled, every speaker switch sends an updated `subscribe` command to the engine with the new participant ID and `isolate_audio=true`, so the audio track always follows the same participant as the video.

### Director TCP controls

```sh
# Inspect directed/raw/candidate/last/manual speaker state.
echo '{"cmd":"speaker_director_status"}' | nc 127.0.0.1 19870

# Update director timing.
echo '{"cmd":"speaker_director_configure","sensitivity_ms":650,"hold_ms":2500}' | nc 127.0.0.1 19870

# Manually take a participant until released.
echo '{"cmd":"speaker_director_take","participant_id":123456}' | nc 127.0.0.1 19870

# Return to automatic speaker direction.
echo '{"cmd":"speaker_director_release"}' | nc 127.0.0.1 19870
```

`speaker_director_status` returns both numeric IDs and resolved participant
objects (`directed_speaker`, `raw_speaker`, `candidate_speaker`,
`last_speaker`, `manual_speaker`, and `excluded_participants`) plus a `status`
field. TCP subscribers receive `speaker_director_changed` events whenever the
directed, candidate, or manual speaker changes.

OSC `/zoom/speaker_director/status` keeps the legacy numeric reply and also
emits `/zoom/speaker_director/status/detail ,iiiiiiiiiiiis` with directed, raw,
candidate, last, manual, sensitivity, hold, require-video, candidate elapsed,
hold remaining, exclusion 1, exclusion 2, and status text.

## Output Profiles

Named profiles save the full source-to-participant mapping to JSON files under:

```
obs-studio/plugin_config/obs-zoom-plugin/profiles/<name>.json
```

Use the **Zoom Output Manager** dock, or **OBS -> Tools -> Zoom Output Manager** to focus it, to save, load, and delete profiles interactively. Profiles preserve assignment mode, requested resolution, channel mode, and audio role (`Mix`, `Isolated`, or `Audience`). Loading a profile reports how many saved outputs matched the current OBS sources and lists missing saved source names so mismatched profiles are visible before Apply. Code can call `ZoomOutputProfile::save() / load() / list() / remove()` directly.

## Repeatable Load Measurements

For 8-feed stability testing, start OBS in the target production state first:
eight participant feeds assigned, ISO recording enabled if needed, and the
program stream/recording active. Then run:

```powershell
.\scripts\Measure-CoreVideoLoad.ps1 -DurationSeconds 300 -SampleSeconds 5
```

The script writes CSV samples and a summary under `artifacts/load-tests/` for
`obs64`, `ZoomObsEngine`, and `ffmpeg`, making CPU and memory comparisons
repeatable across builds. The summary includes minimum, maximum, and final
process counts plus zero-count sample totals so engine restarts, OBS exits, and
ISO encoder drops are visible instead of being hidden by a healthy final sample.

## Architecture Overview

```
OBS Studio
`-- obs-zoom-plugin  (no Zoom SDK dependency)
    |-- ZoomDock              - dockable Qt panel: animated CvStatusDot, join/leave,
    |                           token-type selector, recovery countdown,
    |                           Active Speaker Director controls, routing actions;
    |                           CvBanner first-run credentials + update-available
    |                           notices; persists last meeting ID + display name
    |-- CvUpdateChecker  *    - once-per-session GitHub Releases API check (opt-out
    |                           via Settings); async, silent-fail, no telemetry
    |-- ZoomOAuthManager      - broker-backed OAuth 2.0 PKCE: begin_authorization,
    |                           handle_redirect_url, register_url_scheme,
    |                           refresh_access_token_blocking, fetch_user_zak_blocking;
    |                           DPAPI token storage on Windows
    |-- ZoomEngineClient  *  - IPC singleton: launches engine, owns pipes/sockets,
    |                           tracks roster/speaker, dispatches frame callbacks,
    |                           subscribe_spotlight / subscribe_screenshare
    |-- ZoomReconnectManager  - exponential back-off recovery after crash/disconnect;
    |                           stores session credentials for re-join
    |-- ZoomSource            - per-source: reads I420+PCM from ShmRegion,
    |                           AssignmentMode (Participant/ActiveSpeaker/Spotlight/ScreenShare),
    |                           failover_participant_id, HwVideoPipeline, OBS hotkeys
    |-- HwVideoPipeline       - optional FFmpeg I420->NV12 (CUDA/VAAPI/VideoToolbox/QSV)
    |-- ZoomAudioRouter       - SDK audio fan-out to all registered sinks
    |-- SpeakerDirector       - directed active speaker state, debounce,
    |                           manual take/release, clean source handoff
    |-- ZoomOutputManager     - central source registry for runtime reconfiguration
    |-- ZoomOutputProfile     - named JSON profile persistence
    |-- ZoomControlServer     - TCP JSON API on port 19870 (hardened token auth);
    |                           oauth_callback command for URL scheme forwarding
    |-- ZoomOscServer         - UDP OSC API on port 19871
    |-- cv-style.h / cv-widgets - CoreVideo stylesheet, CvStatusDot, CvBanner
    `-- zoom-types.h          - MeetingState, AssignmentMode, MeetingKind,
                                RecoveryReason, ParticipantInfo, ZoomJoinAuthTokens...

ZoomObsEngine  (separate child process - owns ALL Zoom SDK access)
|-- Zoom SDK 5.17+/7.x (auth, meeting+webinar join, participant/spotlight tracking,
|                        raw video/audio capture)
`-- Communicates with plugin via:
    |-- JSON over named pipes (Windows) or Unix sockets (macOS/Linux)
    |   Plugin->Engine: init - join(kind) - leave - subscribe - subscribe_spotlight
    |                  subscribe_screenshare - unsubscribe - quit
    |   Engine->Plugin: ready - auth_ok - auth_fail - joined - left - frame - audio
    |                  participants(+spotlight_index/is_sharing_screen) - active_speaker - error
    `-- Named shared memory (ZoomObsPlugin_<uuid>) for I420 video + PCM audio frames

CoreVideoOAuthCallback  (thin helper binary - ships beside the plugin)
|-- Windows: CoreVideoOAuthCallback.exe - intercepts corevideo:// URI via registry;
|            reads control-server port from OBS global config; POSTs oauth_callback command
`-- macOS:   CoreVideoOAuthCallback.app - registered with Launch Services for corevideo://;
             same forwarding behaviour
```

See the **[full documentation](https://corevideo.iamfatness.us/documentation/)** for all architecture diagrams including the ZoomEngineClient deep-dive, OAuth PKCE flow, assignment mode flows, auto-reconnect, hardware video acceleration, TCP + OSC API references, output profile format, and full IPC protocol reference.

See the **[product roadmap](docs/PRODUCT_ROADMAP.md)** for the phased plan that
tracks the highest-impact missing features, bugs, and production-readiness work.

## Project Structure

```
CoreVideo/
|-- CMakeLists.txt
|-- buildspec/
|   |-- macos.cmake
|   `-- windows.cmake
|-- cmake/
|   `-- CoreVideoOAuthCallback-Info.plist.in  # macOS OAuth helper bundle plist
|-- data/locale/en-US.ini
|-- docs/                                     # GitHub Pages documentation
|   |-- index.html
|   |-- OPERATOR_QUICKSTART.md                # End-user production workflow
|   |-- RELEASE_CHECKLIST.md                  # Publisher release validation steps
|   |-- GPU_TEXTURE_SHARING_RESEARCH.md       # Spout/Syphon future transport notes
|   |-- ZOOM_MARKETPLACE_OAUTH.md             # OAuth setup guide
|   `-- policies/                             # Security & privacy policy documents
|-- engine/src/                               # ZoomObsEngine (owns ALL SDK access)
|   |-- main.cpp                              # IPC loop, SDK auth/join/webinar, spotlight tracking
|   |-- engine-video.cpp/h                    # IZoomSDKRenderer -> named shared memory (I420)
|   `-- engine-audio.cpp/h                    # SDK audio -> named shared memory (PCM)
`-- src/                                      # OBS plugin (no SDK linkage)
    |-- plugin-main.cpp                       # Module load/unload, dock, Tools menu, SIGPIPE
    |-- zoom-source.*                         # Participant source: ShmRegion, AssignmentMode,
    |                                         #   HwVideoPipeline, failover, hotkeys, placeholder
    |-- zoom-engine-client.*                  # IPC singleton: engine launch, spotlight/screenshare,
    |                                         #   monitor thread, deferred join, roster callbacks
    |-- zoom-oauth.*                          # Broker PKCE: ZoomOAuthManager, token storage,
    |                                         #   register_url_scheme, token refresh + DPAPI storage
    |-- oauth-callback-helper.cpp             # Windows: CoreVideoOAuthCallback.exe entry point
    |-- oauth-callback-helper-macos.mm        # macOS: CoreVideoOAuthCallback.app entry point
    |-- zoom-dock.*                           # Qt dockable join/leave/recovery control panel;
    |                                         #   CvStatusDot, CvBanner, token-type selector
    |-- zoom-reconnect.*                      # Auto-reconnect with exponential back-off
    |-- zoom-types.h                          # MeetingState, AssignmentMode, MeetingKind,
    |                                         #   RecoveryReason, ParticipantInfo, ZoomJoinAuthTokens...
    |-- cv-style.h                            # CoreVideo QSS stylesheet (dark theme, button roles)
    |-- cv-widgets.*                          # CvStatusDot (animated dot), CvBanner (notice strip)
    |-- cv-update-check.*                     # CvUpdateChecker: once-per-session GitHub Releases
    |                                         #   check, async + silent-fail (Qt Network)
    |-- cv-version-compare.h                  # Dependency-free semver-ish tag comparison (unit tested)
    |-- hw-video-pipeline.*                   # FFmpeg I420->NV12 (CUDA/VAAPI/VideoToolbox/QSV)
    |-- zoom-audio-delegate.*                 # Mixed/isolated SDK audio -> OBS
    |-- zoom-audio-router.*                   # Central SDK audio fan-out
    |-- zoom-auth.*                           # legacy/manual SDK auth state helpers
    |-- zoom-meeting.*                        # Meeting state machine
    |-- zoom-participants.*                   # Roster, active speaker, spotlight callbacks
    |-- zoom-participant-audio-source.*       # Per-participant audio OBS source
    |-- zoom-interpretation-audio-source.*    # Language interpretation OBS source
    |-- zoom-video-delegate.*                 # Video frames, resolution, loss mode, preview
    |-- zoom-share-delegate.*                 # Screen share frames -> OBS
    |-- zoom-output-manager.*                 # Source registry + runtime reconfiguration
    |-- zoom-output-profile.*                 # Named JSON profile persistence
    |-- zoom-output-dialog.*                  # Qt Output Manager dock widget
    |-- zoom-diagnostics-dialog.*             # Qt Diagnostics dock widget
    |-- zoom-control-server.*                 # TCP JSON API (port 19870) + oauth_callback command
    |-- zoom-osc-server.*                     # UDP OSC API (port 19871)
    |-- zoom-settings.*                       # Broker URL, OAuth tokens, local ports + reconnect persistence
    |-- zoom-settings-dialog.*                # Qt Settings dialog with Zoom sign-in + local settings
    |-- zoom-credentials.h.in                 # Embedded SDK credentials (CMake-generated)
    |-- obs-zoom-version.h.in                 # Plugin version (CMake-generated)
    |-- engine-ipc.h                          # IPC constants + cross-platform helpers
    `-- obs-utils.*                           # OBS helper functions
```

## Security

See [SECURITY.md](SECURITY.md) for the vulnerability disclosure policy.

### OAuth token storage

Zoom OAuth access and refresh tokens are persisted in OBS's global config
(`global.ini`, `[ZoomPlugin]` section).

- **Windows** - tokens are encrypted with DPAPI (`CryptProtectData`, scoped to
  the current Windows user) before being written to disk.
- **macOS / Linux** - there is currently no OS-level secret store wired up
  (no Keychain, no libsecret), so tokens are written to `global.ini` in
  **plaintext**. The plugin logs a prominent one-time warning
  (`SECURITY: OAuth tokens are stored WITHOUT OS-level encryption...`) to the
  OBS log on these platforms so this isn't silent. Anyone with read access to
  the OBS config directory can read the stored tokens. Follow-up work to add
  Keychain (`Security.framework`) support on macOS and `libsecret` support on
  Linux is tracked as a to-do; see the PR that introduced this note for
  details.

## License

See [LICENSE](LICENSE) for details.
