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

---

## Features

- **Raw video capture** - I420 YUV, selectable 360p / 720p / 1080p resolution
- **Hardware-accelerated video** - optional FFmpeg I420->NV12 conversion via CUDA, VAAPI, VideoToolbox, or QSV (`-DCOREVIDEO_HW_ACCEL=ON`)
- **Video loss mode** - hold last frame or show black when a feed drops; shows color-bar placeholder before first frame
- **Raw audio capture** - 48 kHz PCM, mono or stereo, with per-participant audio isolation and mixer routing
- **Auto ISO recording** - record assigned participant/active-speaker/spotlight outputs to separate FFmpeg-encoded MP4 files with matching PCM WAV audio, plus optional main OBS program recording
- **Assignment modes** - each source independently follows: a fixed participant, the active speaker, a ZoomISO-style spotlight slot (1...N), or the active screen share
- **Failover participant** - configure a secondary participant that activates automatically when the primary leaves
- **Active Speaker Director** - configurable sensitivity + hold-time switching, manual take/release supersede, and a dedicated `CoreVideo Active Speaker` OBS source for clean speaker-follow output
- **Spotlight / ZoomISO** - subscribe a source to spotlight slot N; engine resolves which participant is spotlighted
- **Screen share capture** - source subscribes to the active meeting screen-share feed
- **Zoom interpretation audio channel capture** - dedicated OBS source for existing Zoom interpretation audio channels
- **Per-participant audio sources** - standalone OBS audio source per meeting participant
- **Webinar support** - join Zoom Webinars using the dedicated SDK entry point (Webinar checkbox in control dock)
- **Participant roster** - live list with video, mute, talking, host, co-host, raised hand, spotlight slot, and screen-sharing state
- **Control dock** - dockable Qt panel with animated status dot, join/leave, token-type selector, recovery countdown, Active Speaker Director controls, and a routing section that opens the dedicated Output Manager; persists last meeting ID and display name across sessions
- **Diagnostics dock** - dockable OBS panel showing requested vs observed resolution, FPS, frame age, retry counts, and recent engine debug events for live troubleshooting
- **Auto-reconnect** - exponential back-off recovery after engine crash, network drop, or unexpected disconnect
- **OBS hotkeys** - per-source hotkeys to enable/disable active speaker mode
- **TCP control API** - JSON server on `127.0.0.1:19870` for scripts and dashboards; includes `oauth_callback` command for custom URL scheme forwarding
- **OSC control API** - UDP OSC server on `127.0.0.1:19871` for lighting consoles and broadcast hardware
- **Output profiles** - save and load named participant-to-source mappings as JSON files
- **Output manager dock** - dockable OBS panel and API for viewing and reconfiguring all sources at runtime
- **JWT generation** - CoreVideo generates Meeting SDK JWTs locally from key+secret; manual override available
- **Zoom OAuth PKCE** - user-level OAuth 2.0 with PKCE for attributed joins and Marketplace compliance; fetches a short-lived ZAK via `GET /v2/users/me/zak`; `corevideo://` custom URL scheme with platform callback helpers (`CoreVideoOAuthCallback.exe` / `.app`); DPAPI token protection on Windows; confidential client mode supported
- **SDK 5.17.x and 7.x** - auto-detects flat and subfolder header layouts
- **Hardened security** - constant-time token comparison, validated IPC input, sanitised participant IDs, SIGPIPE handling
- **Modern UI** - CoreVideo stylesheet with dark theme, animated `CvStatusDot`, `CvBanner` first-run notices, and button role variants (primary / danger)
- **Multi-platform** - Windows (x64/arm64), macOS (universal arm64 + x86_64), Linux

## Requirements

| Dependency | Version | Notes |
|---|---|---|
| OBS Studio | 30+ | `libobs` + `obs-frontend-api` |
| CMake | 3.16+ | Build system |
| Qt | 6.x | Core + Network + Widgets |
| FFmpeg | Runtime executable | Required for auto ISO recording. Must be on `PATH` or supplied via `ffmpeg_path`. |
| Zoom Meeting SDK | **5.17.x / 7.x** | Place in `third_party/zoom-sdk/`. Windows builds support the older flat header layout and the newer 7.x subfolder header layout. |
| C++ compiler | C++17 | MSVC 2022 / Clang 14+ / GCC 11+ |
| Zoom Developer Account | - | Meeting SDK key + secret. OAuth Client ID required for Marketplace / external-account joins. |

## Quick Start

1. **Get the Zoom SDK** - download from the [Zoom Developer Portal](https://developers.zoom.us/docs/meeting-sdk/releases/) and place it at `third_party/zoom-sdk/`. CMake auto-detects x64/arm64/x86 sub-layouts on Windows.

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

4. **Configure credentials** - open OBS -> **Tools -> Zoom Plugin Settings** and enter your SDK Key and SDK Secret. CoreVideo generates a short-lived Meeting SDK JWT locally when joining. The JWT Token field is optional and overrides generated tokens when set. You may also set a control server token and custom ports.

### Windows release packaging

GitHub Actions can validate the Windows build without the restricted Zoom
runtime, but public client releases must include `zoom-runtime\sdk.dll` and the
other Zoom SDK runtime DLLs. If `ZOOM_SDK_WINDOWS_URL` is not configured as a
GitHub repository secret, CI skips publishing a GitHub Release instead of
shipping an incomplete package.

For fast local releases from a machine that already has the Zoom runtime, use:

```powershell
.\scripts\release-local.ps1 -Version v0.1.6 -Upload
```

If an FFmpeg shared development tree exists at `C:\ffmpeg`, local releases
automatically enable hardware I420->NV12 conversion and bundle the FFmpeg DLLs
beside the OBS plugin. Pass `-DisableFfmpegHwAccel` to force a CPU-only build.

Useful options:

```powershell
.\scripts\release-local.ps1 -Version v0.1.6 -Install -ObsInstallPath "C:\Program Files\obs-studio"
.\scripts\release-local.ps1 -Version v0.1.6 -BuildPath build-nmake -Upload
```

The script builds, installs into a staging folder, verifies
`obs-zoom-plugin.dll`, `ZoomObsEngine.exe`, and `zoom-runtime\sdk.dll`, creates a
ZIP under `dist/`, and optionally uploads it to the matching GitHub Release.

### OBS scene smoke test

Use the OBS smoke test when validating Sidecar/OBS scene graph behavior on a
machine with OBS running and obs-websocket enabled. The script connects directly
to obs-websocket v5, creates a deterministic CoreVideo test scene, links
participant sources through nested slot scenes, switches OBS to the test scene,
and audits that the expected scenes, inputs, and scene items are present.

```powershell
.\scripts\obs-scene-smoke-test.ps1
```

If obs-websocket has a password, pass it explicitly or use the environment:

```powershell
$env:OBS_WEBSOCKET_PASSWORD = "your-websocket-password"
.\scripts\obs-scene-smoke-test.ps1 -ParticipantCount 8
```

To verify an already-created scene graph without creating or modifying sources:

```powershell
.\scripts\obs-scene-smoke-test.ps1 -AuditOnly -SceneName "CoreVideo Smoke Test"
```

5. **Set up OAuth (for Marketplace / external-account joins)** - in the Settings dialog, enter your OAuth Client ID, set the Redirect URI to `corevideo://oauth/callback`, click **Register corevideo:// URL Scheme**, then click **Authorize with Zoom**. See [`docs/ZOOM_MARKETPLACE_OAUTH.md`](docs/ZOOM_MARKETPLACE_OAUTH.md) for the full walkthrough.

6. **Join once, then assign outputs** - use the CoreVideo dock or the TCP/OSC control APIs to join the meeting once per OBS session. Then add **Zoom Participant**, **Zoom Participant Audio**, **Zoom Share**, or **Zoom Interpretation Audio** sources and assign them to participants or dynamic roles.

## Zoom OAuth PKCE

CoreVideo supports user-level OAuth 2.0 with PKCE for attributed meeting joins and Zoom App Marketplace compliance. This is required when joining meetings hosted by accounts other than the SDK account.

### Flow
1. In **Tools -> Zoom Plugin Settings**, enter the OAuth Client ID (and optionally Client Secret for confidential/test apps). Set Redirect URI to `corevideo://oauth/callback`.
2. Click **Register corevideo:// URL Scheme** - registers the scheme in the OS so the callback helper can intercept the redirect.
3. Click **Authorize with Zoom** - opens the browser with a PKCE authorization request (S256 code challenge, high-entropy verifier, state CSRF token).
4. Zoom redirects to `corevideo://oauth/callback?code=...&state=...`.
5. `CoreVideoOAuthCallback.exe` (Windows) or `CoreVideoOAuthCallback.app` (macOS) forwards the URL to the plugin via the TCP control server (`oauth_callback` command).
6. The plugin verifies state, exchanges the code at `https://zoom.us/oauth/token`, and persists access + refresh tokens. On Windows, tokens are DPAPI-protected before storage.
7. Before each meeting join, CoreVideo refreshes the token if needed and fetches a ZAK from `GET /v2/users/me/zak`. The ZAK is passed into the SDK `JoinParam4WithoutLogin`.

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
echo '{"cmd":"oauth_callback","url":"corevideo://oauth/callback?code=...&state=..."}' | nc 127.0.0.1 19870
```

Commands: `help`, `status`, `list_participants`, `list_outputs`, `assign_output`, `assign_output_ex`, `recover_stale_outputs`, `upgrade_low_quality_outputs`, `join`, `leave`, `oauth_callback`, `iso_recording_start`, `iso_recording_stop`, `iso_recording_status`, `speaker_director_status`, `speaker_director_configure`, `speaker_director_take`, `speaker_director_release`.

### Auto ISO Recording

Use **OBS -> Tools -> Zoom ISO Recorder** for the operator UI. The dock provides
an output-folder picker, FFmpeg path/test controls, a program-recording toggle,
Start/Stop buttons, and a live table of active ISO sessions and file paths.

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
| `/zoom/list_participants` | - | Reply: one `/zoom/participant` per user |
| `/zoom/list_outputs` | - | Reply: one `/zoom/output` per source |
| `/zoom/recover_stale_outputs` | `[,i]` | Retry stale video outputs; optional `1` forces cooldown bypass |
| `/zoom/upgrade_low_quality_outputs` | `[,i]` | Retry outputs below requested resolution; skips feeds already at 1080p |
| `/zoom/join` | `,sss` | meeting_id, passcode, display_name |
| `/zoom/leave` | - | Leave meeting |
| `/zoom/assign_output` | `,si[i]` | source, participant_id, [active_speaker] |
| `/zoom/assign_output/active_speaker` | `,s` | source |
| `/zoom/isolate_audio` | `,si` | source, 0\|1 |
| `/zoom/iso/start` | `[,s]` | optional output directory |
| `/zoom/iso/stop` | - | Stop ISO recording |

## Active Speaker Mode

CoreVideo has two active-speaker workflows:

- Set a normal **Zoom Participant** source to **Active Speaker** assignment mode when that source should follow the directed speaker.
- Add the dedicated **CoreVideo Active Speaker** source when you want a single speaker-follow OBS source. It uses a two-slot handoff internally: the current participant stays visible while the next participant warms on a hidden slot, then the source cuts only after a valid frame is available.

The **Active Speaker Director** in the Zoom Control dock decides which participant is directed. It tracks the raw Zoom speaker, candidate speaker, directed speaker, last directed speaker, and any manual supersede.

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

## Output Profiles

Named profiles save the full source-to-participant mapping to JSON files under:

```
obs-studio/plugin_config/obs-zoom-plugin/profiles/<name>.json
```

Use the **Zoom Output Manager** dock, or **OBS -> Tools -> Zoom Output Manager** to focus it, to save, load, and delete profiles interactively. Code can call `ZoomOutputProfile::save() / load() / list() / remove()` directly.

## Architecture Overview

```
OBS Studio
`-- obs-zoom-plugin  (no Zoom SDK dependency)
    |-- ZoomDock              - dockable Qt panel: animated CvStatusDot, join/leave,
    |                           token-type selector, recovery countdown,
    |                           Active Speaker Director controls, routing actions;
    |                           CvBanner first-run credentials notice; persists last
    |                           meeting ID + display name
    |-- ZoomOAuthManager      - OAuth 2.0 PKCE: begin_authorization, handle_redirect_url,
    |                           register_url_scheme, refresh_access_token_blocking,
    |                           fetch_zak_blocking; DPAPI token storage on Windows;
    |                           confidential + public client modes
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

## CoreVideo Sidecar

CoreVideo Sidecar is the standalone broadcast production console that ships alongside the OBS plugin. It is a Qt 6 application that connects to OBS via obs-websocket v5 and to the CoreVideo plugin via the TCP control API. It is designed to be the operator's primary interface during a live show.

### M/E Bus

The M/E (Mix/Effects) bus is the broadcast switcher core. It owns two Looks: **Program** (PGM — on-air) and **Preview** (PVW — staged next). All transitions route through `MEBus` so the UI, OBS bridge, and remote control share one state.

| Operation | Description |
|---|---|
| **TAKE (cut)** | Commits PVW → PGM instantly; fires `tookProgram` so OBS receives the new Look immediately |
| **AUTO** | Timed take: fades PGM through black, commits at the midpoint (OBS push fires here), then fades back up |
| **FTB toggle** | Fade-to-black: fades PGM out, or back in if already black — cleanest "kill program" gesture |
| **Swap** | Swaps PGM ↔ PVW in memory without pushing to OBS — useful for previewing a swap before committing |

### Look Library

A **Look** is the complete description of what is on air — template, slot assignments, tile style, and overlays. The Look Library ships 17 broadcast-ready presets:

| Category | Looks |
|---|---|
| **Clean** | `one-up-clean`, `two-up-clean`, `three-up-feature`, `four-up-clean`, `five-up-feature`, `six-up-grid`, `seven-up-feature`, `eight-up-grid` |
| **Speaker** | `solo-spotlight`, `speaker-screenshare` |
| **News** | `news-desk`, `news-breaking` |
| **Talk Show** | `talk-show-classic`, `talk-show-warm` |
| **Podcast** | `podcast-duo`, `podcast-pip` |
| **Panel** | `panel-discussion` |

Each Look JSON lives in `sidecar/data/looks/` and references a template by ID, carries `TileStyle` (canvas colour, border, corner radius, drop shadow, name tags), and can embed `Overlay` objects. Looks are displayed in the **Look Panel** and can be staged to PVW with a single click.

### Templates

Templates define the slot geometry. 11 templates ship in `sidecar/data/templates/`:

`1-up`, `2-up-pip`, `2-up-sbs`, `3-up-feature`, `4-up-grid`, `5-up-feature`, `6-up-grid`, `7-up-feature`, `8-up-grid`, `speaker-screenshare`, `talk-show`

### Broadcast Overlays

Overlays sit above the slot composition in canvas-relative coordinates (0..1). Five overlay types are supported:

| Type | Use case |
|---|---|
| `LowerThird` | Name + title strip at the bottom of a tile |
| `Bug` | Small channel bug / logo |
| `Ticker` | Scrolling news-style bottom bar |
| `TitleCard` | Full-frame title / segment intro |
| `Bumper` | Timed transition graphic |

Overlays are attached to a Look and rendered on the Sidecar preview canvas. The **Overlay Panel** lets operators add, edit, and remove overlays live.

### Command Palette

Press **Ctrl+K** (or the toolbar button) to open the Spotlight-style command palette. It provides instant search-and-execute access to every Sidecar action: apply Look, change template, set show phase, switch OBS scene, or jump to any panel.

### Sidecar TCP Control Server

Sidecar exposes its own JSON control server on `127.0.0.1:19880` (distinct from the plugin's port 19870) for Bitfocus Companion and other automation surfaces.

| Command | Fields | Description |
|---|---|---|
| `status` | — | Returns phase, template, OBS state, current scene |
| `list_templates` | — | Returns all available templates |
| `set_phase` | `phase` | Set show phase (`pre_show`, `live`, `post_show`) |
| `apply_template` | `template_id` | Apply a template by ID |
| `set_scene` | `scene` | Switch OBS to a named scene |
| `subscribe_events` | — | Keep socket open; receives push events |

Push events: `phase_changed`, `template_changed`, `obs_state`, `scene_changed`, `scenes_updated`.

### Build the Sidecar

```sh
cmake -B build-sidecar \
  -DCOREVIDEO_BUILD_SIDECAR=ON \
  -DCMAKE_PREFIX_PATH="/path/to/Qt6"
cmake --build build-sidecar --config Release
```

Sidecar requires Qt 6 with WebSockets support. No Zoom SDK or OBS SDK is needed.

### Connecting Sidecar to OBS

1. In OBS: **Tools → obs-websocket Settings** — enable WebSocket Server, note the port (default 4455) and password.
2. Launch CoreVideo Sidecar.
3. In Sidecar **Settings**: enter OBS host, port, and password.
4. Sidecar discovers scenes, sources, and starts receiving state events automatically.

## Bitfocus Companion Integration

CoreVideo ships a full Companion v3 module at `companion/companion-module-corevideo-obs/`. It connects to both the CoreVideo TCP API (port 19870) and Sidecar's control server (port 19880).

### Actions

| Action | Description |
|---|---|
| Zoom: Join Meeting | Join by ID, URL, or passcode |
| Zoom: Leave Meeting | Leave the current meeting |
| Zoom: Assign Participant to Output | Route a participant to an OBS source |
| Zoom: Assign Spotlight Slot | Track Zoom spotlight slot N |
| Zoom: Assign Screen Share | Route active screen share |
| Zoom: Cancel Auto-Recovery | Abort a reconnect attempt |
| Sidecar: Set Show Phase | `pre_show` / `live` / `post_show` |
| Sidecar: Apply Template | Apply a Look template by ID |
| Sidecar: Switch OBS Scene | Switch to a named OBS scene |

### Feedbacks

| Feedback | Description |
|---|---|
| Meeting State | Is meeting in state X? |
| Meeting State Color | Button color tracks state automatically |
| Is Active Speaker | Lights up when participant ID is active speaker |
| Output Has Participant | Lights up when source has someone assigned |
| Output Tracking Active Speaker | Lights up when source is in active-speaker mode |
| Auto-Recovery Active | Lights up during reconnect attempts |

### Variables

| Variable | Value |
|---|---|
| `$(corevideo-obs:meeting_state)` | `idle` / `joining` / `in_meeting` / … |
| `$(corevideo-obs:active_speaker_name)` | Display name of current active speaker |
| `$(corevideo-obs:active_speaker_id)` | Numeric user ID |
| `$(corevideo-obs:participant_count)` | Number of participants in roster |

### Installing

```bash
cd companion/companion-module-corevideo-obs
npm install && npm run build
# Add dist/ as a local module in Companion Developer Mode
```

See [`companion/COMPANION_INTEGRATION.md`](companion/COMPANION_INTEGRATION.md) for the full setup guide and OSC module option.

## Project Structure

```
CoreVideo/
|-- CMakeLists.txt
|-- buildspec/
|   |-- macos.cmake
|   `-- windows.cmake
|-- cmake/
|   `-- CoreVideoOAuthCallback-Info.plist.in  # macOS OAuth helper bundle plist
|-- companion/
|   |-- COMPANION_INTEGRATION.md             # Bitfocus Companion setup guide
|   `-- companion-module-corevideo-obs/       # Companion v3 TypeScript module
|       `-- src/
|           |-- index.ts                     # Module entry: OBS-ws + Sidecar clients, events
|           |-- actions.ts                   # All actions (zoom join/leave/assign, sidecar phase/template)
|           |-- feedbacks.ts                 # Button feedbacks and color logic
|           |-- variables.ts                 # Dynamic variables (meeting state, active speaker, …)
|           |-- state.ts                     # Shared module state
|           |-- obs-ws-client.ts             # obs-websocket v5 WebSocket client
|           `-- sidecar-client.ts            # Sidecar TCP control server client
|-- data/locale/en-US.ini
|-- docs/                                    # GitHub Pages documentation
|   |-- index.html
|   |-- ZOOM_MARKETPLACE_OAUTH.md            # OAuth setup guide
|   |-- COMPANION_INTEGRATION.md
|   |-- PRODUCTION_CONSOLE_ARCHITECTURE.md
|   `-- policies/                            # Security & privacy policy documents (CVP-01 … CVP-06)
|-- engine/src/                              # ZoomObsEngine (owns ALL SDK access)
|   |-- main.cpp                             # IPC loop, SDK auth/join/webinar, spotlight tracking
|   |-- engine-video.cpp/h                   # IZoomSDKRenderer -> named shared memory (I420)
|   `-- engine-audio.cpp/h                   # SDK audio -> named shared memory (PCM)
|-- sidecar/                                 # CoreVideo Sidecar (standalone broadcast console)
|   |-- CMakeLists.txt
|   |-- data/
|   |   |-- looks/                           # 17 broadcast Look presets (JSON)
|   |   `-- templates/                       # 11 slot-geometry templates (JSON)
|   `-- src/
|       |-- main.cpp                         # Sidecar app entry point
|       |-- mainwindow.*                     # Main window: M/E bus, panels, toolbar, keyboard shortcuts
|       |-- me-bus.*                         # MEBus: PGM/PVW state, TAKE/AUTO/FTB transitions
|       |-- look.h                           # Look struct: template + slot assignments + overlays + style
|       |-- look-library.*                   # Look catalog: load from disk, built-in presets
|       |-- look-panel.*                     # Look Library panel widget
|       |-- look-render-plan.*               # Resolves a Look into an OBS scene layout plan
|       |-- obs-look-renderer.*              # Pushes a LookRenderPlan to OBS via obs-websocket
|       |-- overlay.h                        # Overlay struct: LowerThird / Bug / Ticker / TitleCard / Bumper
|       |-- overlay-panel.*                  # Overlay editing panel widget
|       |-- lower-third-controller.*         # Lower-third lifecycle and timed-out logic
|       |-- layout-template.*                # LayoutTemplate: named slots with geometry
|       |-- template-manager.*              # Loads and indexes all templates
|       |-- template-panel.*                # Template picker panel widget
|       |-- participant-panel.*              # Participant roster with drag-to-slot
|       |-- preview-canvas.*                # QPainter canvas: live PGM/PVW preview
|       |-- obs-client.*                     # OBSClient: obs-websocket v5 QWebSocket wrapper
|       |-- sidecar-control-server.*         # TCP JSON control server on port 19880
|       |-- zoom-control-client.*            # TCP client to CoreVideo plugin (port 19870)
|       |-- command-palette.*                # Spotlight-style Ctrl+K command palette
|       |-- obs-live-validator.*             # Live OBS scene graph validator
|       |-- obs-audit-report.*              # OBS scene audit report generator
|       |-- speaker-director.*              # Director automation helpers
|       `-- sidecar-style.h                  # Sidecar QSS dark theme
`-- src/                                     # OBS plugin (no SDK linkage)
    |-- plugin-main.cpp                      # Module load/unload, dock, Tools menu, SIGPIPE
    |-- zoom-source.*                        # Participant source: ShmRegion, AssignmentMode,
    |                                        #   HwVideoPipeline, failover, hotkeys, placeholder
    |-- zoom-engine-client.*                 # IPC singleton: engine launch, spotlight/screenshare,
    |                                        #   monitor thread, deferred join, roster callbacks
    |-- zoom-oauth.*                         # OAuth 2.0 PKCE: ZoomOAuthManager, ZAK fetch,
    |                                        #   register_url_scheme, token refresh + DPAPI storage
    |-- oauth-callback-helper.cpp            # Windows: CoreVideoOAuthCallback.exe entry point
    |-- oauth-callback-helper-macos.mm       # macOS: CoreVideoOAuthCallback.app entry point
    |-- zoom-dock.*                          # Qt dockable join/leave/recovery control panel;
    |                                        #   CvStatusDot, CvBanner, token-type selector
    |-- zoom-reconnect.*                     # Auto-reconnect with exponential back-off
    |-- zoom-iso-recorder.*                  # Auto ISO recording: per-source FFmpeg+WAV sessions
    |-- zoom-iso-panel.*                     # Zoom ISO Recorder dock widget (Tools menu)
    |-- zoom-diagnostics-dialog.*            # Diagnostics dock: resolution, FPS, frame age, events
    |-- speaker-director.*                   # Directed active speaker: debounce, take/release
    |-- zoom-types.h                         # MeetingState, AssignmentMode, MeetingKind,
    |                                        #   RecoveryReason, ParticipantInfo, ZoomJoinAuthTokens…
    |-- cv-style.h                           # CoreVideo QSS stylesheet (dark theme, button roles)
    |-- cv-widgets.*                         # CvStatusDot (animated dot), CvBanner (notice strip)
    |-- cv-onboarding.*                      # First-run onboarding wizard
    |-- hw-video-pipeline.*                  # FFmpeg I420->NV12 (CUDA/VAAPI/VideoToolbox/QSV)
    |-- zoom-audio-delegate.*                # Mixed/isolated SDK audio -> OBS
    |-- zoom-audio-router.*                  # Central SDK audio fan-out
    |-- zoom-auth.*                          # JWT auth + observable auth state
    |-- zoom-meeting.*                       # Meeting state machine
    |-- zoom-participants.*                  # Roster, active speaker, spotlight callbacks
    |-- zoom-participant-audio-source.*      # Per-participant audio OBS source
    |-- zoom-interpretation-audio-source.*   # Language interpretation OBS source
    |-- zoom-video-delegate.*               # Video frames, resolution, loss mode, preview
    |-- zoom-share-delegate.*               # Screen share frames -> OBS
    |-- zoom-output-manager.*               # Source registry + runtime reconfiguration
    |-- zoom-output-profile.*               # Named JSON profile persistence
    |-- zoom-output-dialog.*                # Qt Output Manager dock widget
    |-- zoom-output-health.h                # Output health telemetry (resolution, FPS, age, retries)
    |-- zoom-control-server.*               # TCP JSON API (port 19870) + oauth_callback command
    |-- zoom-osc-server.*                   # UDP OSC API (port 19871)
    |-- zoom-settings.*                     # SDK key/secret/JWT + OAuth tokens + port persistence
    |-- zoom-settings-dialog.*              # Qt Settings dialog with simplified OAuth sign-in
    |-- zoom-credentials.h.in              # Embedded SDK credentials (CMake-generated)
    |-- obs-zoom-version.h.in              # Plugin version (CMake-generated)
    |-- engine-ipc.h                        # IPC constants + cross-platform helpers
    `-- obs-utils.*                         # OBS helper functions
```

## Security

See [SECURITY.md](SECURITY.md) for the vulnerability disclosure policy.

## License

See [LICENSE](LICENSE) for details.
