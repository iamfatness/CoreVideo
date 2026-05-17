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

CoreVideo integrates the Zoom Meeting SDK into OBS â€” no screen capture or virtual camera required. The SDK runs in a dedicated `ZoomObsEngine` child process; the OBS plugin communicates with it through a `ZoomEngineClient` singleton over cross-platform IPC (named pipes on Windows, Unix sockets on macOS/Linux) with frame data delivered through named shared memory. A built-in dockable control panel manages joining, and a `ZoomReconnectManager` handles automatic recovery after crashes or disconnects.

ðŸ“– **[Full Documentation & Architecture Diagrams â†’](https://corevideo.iamfatness.us/documentation/)**
ðŸŽ›ï¸ **[Core Plugin Guide & Examples â†’](https://corevideo.iamfatness.us/core-plugin/)**

---

## Features

- **Raw video capture** â€” I420 YUV, selectable 360p / 720p / 1080p resolution
- **Hardware-accelerated video** â€” optional FFmpeg I420â†’NV12 conversion via CUDA, VAAPI, VideoToolbox, or QSV (`-DCOREVIDEO_HW_ACCEL=ON`)
- **Video loss mode** â€” hold last frame or show black when a feed drops; shows color-bar placeholder before first frame
- **Raw audio capture** â€” 48 kHz PCM, mono or stereo, with per-participant audio isolation and mixer routing
- **Auto ISO recording** â€” record assigned participant/active-speaker/spotlight outputs to separate FFmpeg-encoded MP4 files with matching PCM WAV audio, plus optional main OBS program recording
- **Assignment modes** â€” each source independently follows: a fixed participant, the active speaker, a ZoomISO-style spotlight slot (1â€¦N), or the active screen share
- **Failover participant** â€” configure a secondary participant that activates automatically when the primary leaves
- **Active speaker mode** â€” configurable sensitivity + hold-time debounce, liveness guard, supersede logic
- **Spotlight / ZoomISO** â€” subscribe a source to spotlight slot N; engine resolves which participant is spotlighted
- **Screen share capture** â€” source subscribes to the active meeting screen-share feed
- **Zoom interpretation audio channel capture** â€” dedicated OBS source for existing Zoom interpretation audio channels
- **Per-participant audio sources** â€” standalone OBS audio source per meeting participant
- **Webinar support** â€” join Zoom Webinars using the dedicated SDK entry point (Webinar checkbox in control dock)
- **Participant roster** â€” live list with video, mute, talking, host, co-host, raised hand, spotlight slot, and screen-sharing state
- **Control dock** â€” dockable Qt panel with animated status dot, join/leave, token-type selector, recovery countdown, and inline output table; persists last meeting ID and display name across sessions
- **Auto-reconnect** â€” exponential back-off recovery after engine crash, network drop, or unexpected disconnect
- **OBS hotkeys** â€” per-source hotkeys to enable/disable active speaker mode
- **TCP control API** â€” JSON server on `127.0.0.1:19870` for scripts and dashboards; includes `oauth_callback` command for custom URL scheme forwarding
- **OSC control API** â€” UDP OSC server on `127.0.0.1:19871` for lighting consoles and broadcast hardware
- **Output profiles** â€” save and load named participant-to-source mappings as JSON files
- **Output manager** â€” Qt dialog and API for viewing and reconfiguring all sources at runtime
- **JWT generation** â€” CoreVideo generates Meeting SDK JWTs locally from key+secret; manual override available
- **Zoom OAuth PKCE** â€” user-level OAuth 2.0 with PKCE for attributed joins and Marketplace compliance; fetches a short-lived ZAK via `GET /v2/users/me/zak`; `corevideo://` custom URL scheme with platform callback helpers (`CoreVideoOAuthCallback.exe` / `.app`); DPAPI token protection on Windows; confidential client mode supported
- **SDK 5.17.x and 7.x** â€” auto-detects flat and subfolder header layouts
- **Hardened security** â€” constant-time token comparison, validated IPC input, sanitised participant IDs, SIGPIPE handling
- **Modern UI** â€” CoreVideo stylesheet with dark theme, animated `CvStatusDot`, `CvBanner` first-run notices, and button role variants (primary / danger)
- **Multi-platform** â€” Windows (x64/arm64), macOS (universal arm64 + x86_64), Linux

## Requirements

| Dependency | Version | Notes |
|---|---|---|
| OBS Studio | 30+ | `libobs` + `obs-frontend-api` |
| CMake | 3.16+ | Build system |
| Qt | 6.x | Core + Network + Widgets |
| FFmpeg | Runtime executable | Required for auto ISO recording. Must be on `PATH` or supplied via `ffmpeg_path`. |
| Zoom Meeting SDK | **5.17.x / 7.x** | Place in `third_party/zoom-sdk/`. Windows builds support the older flat header layout and the newer 7.x subfolder header layout. |
| C++ compiler | C++17 | MSVC 2022 / Clang 14+ / GCC 11+ |
| Zoom Developer Account | â€” | Meeting SDK key + secret. OAuth Client ID required for Marketplace / external-account joins. |

## Quick Start

1. **Get the Zoom SDK** â€” download from the [Zoom Developer Portal](https://developers.zoom.us/docs/meeting-sdk/releases/) and place it at `third_party/zoom-sdk/`. CMake auto-detects x64/arm64/x86 sub-layouts on Windows.

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

4. **Configure credentials** â€” open OBS â†’ **Tools â†’ Zoom Plugin Settings** and enter your SDK Key and SDK Secret. CoreVideo generates a short-lived Meeting SDK JWT locally when joining. The JWT Token field is optional and overrides generated tokens when set. You may also set a control server token and custom ports.

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

5. **Set up OAuth (for Marketplace / external-account joins)** â€” in the Settings dialog, enter your OAuth Client ID, set the Redirect URI to `corevideo://oauth/callback`, click **Register corevideo:// URL Scheme**, then click **Authorize with Zoom**. See [`docs/ZOOM_MARKETPLACE_OAUTH.md`](docs/ZOOM_MARKETPLACE_OAUTH.md) for the full walkthrough.

6. **Join once, then assign outputs** â€” use the CoreVideo dock or the TCP/OSC control APIs to join the meeting once per OBS session. Then add **Zoom Participant**, **Zoom Participant Audio**, **Zoom Share**, or **Zoom Interpretation Audio** sources and assign them to participants or dynamic roles.

## Zoom OAuth PKCE

CoreVideo supports user-level OAuth 2.0 with PKCE for attributed meeting joins and Zoom App Marketplace compliance. This is required when joining meetings hosted by accounts other than the SDK account.

### Flow
1. In **Tools â†’ Zoom Plugin Settings**, enter the OAuth Client ID (and optionally Client Secret for confidential/test apps). Set Redirect URI to `corevideo://oauth/callback`.
2. Click **Register corevideo:// URL Scheme** â€” registers the scheme in the OS so the callback helper can intercept the redirect.
3. Click **Authorize with Zoom** â€” opens the browser with a PKCE authorization request (S256 code challenge, high-entropy verifier, state CSRF token).
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

Commands: `help`, `status`, `list_participants`, `list_outputs`, `assign_output`, `assign_output_ex`, `join`, `leave`, `oauth_callback`, `iso_recording_start`, `iso_recording_stop`, `iso_recording_status`.

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
| `/zoom/status` | â€” | Reply: meeting state + active speaker |
| `/zoom/list_participants` | â€” | Reply: one `/zoom/participant` per user |
| `/zoom/list_outputs` | â€” | Reply: one `/zoom/output` per source |
| `/zoom/join` | `,sss` | meeting_id, passcode, display_name |
| `/zoom/leave` | â€” | Leave meeting |
| `/zoom/assign_output` | `,si[i]` | source, participant_id, [active_speaker] |
| `/zoom/assign_output/active_speaker` | `,s` | source |
| `/zoom/isolate_audio` | `,si` | source, 0\|1 |
| `/zoom/iso/start` | `[,s]` | optional output directory |
| `/zoom/iso/stop` | â€” | Stop ISO recording |

## Active Speaker Mode

Enable **Follow active speaker** on any Zoom Participant source to automatically switch the video (and audio) feed to whoever is currently speaking.

### Debounce

Two independent timers prevent rapid camera cuts:

| Parameter | Default | Description |
|---|---|---|
| **Sensitivity** (`speaker_sensitivity_ms`) | 300 ms | New speaker must hold the floor continuously for this long before the switch fires. A different speaker speaking resets the clock. |
| **Hold** (`speaker_hold_ms`) | 2 000 ms | After any switch, no further switch occurs for at least this long. |

The effective delay before each switch is `max(hold_remaining, sensitivity_remaining)`. If the delay is zero the switch fires immediately; otherwise a background thread sleeps for the delay and re-evaluates on the OBS UI thread.

### Safety

- **Liveness flag** â€” a `shared_ptr<atomic<bool>>` captured in every in-flight lambda ensures deferred callbacks bail safely if the source is destroyed before the timer fires.
- **Supersede logic** â€” a new candidate replaces the pending one, restarting the sensitivity clock. Stale callbacks silently discard themselves.
- **Final verification** â€” before committing a switch the code re-checks that the candidate is still the active speaker, so no switch fires for someone who stopped talking during the hold window.
- **UI-thread commitment** â€” all state mutations run on the OBS UI thread via `obs_queue_task`, preventing data races with the properties panel.

### Audio isolation interaction

When **Isolate Audio** is also enabled, every speaker switch sends an updated `subscribe` command to the engine with the new participant ID and `isolate_audio=true`, so the audio track always follows the same participant as the video.

## Output Profiles

Named profiles save the full source-to-participant mapping to JSON files under:

```
obs-studio/plugin_config/obs-zoom-plugin/profiles/<name>.json
```

Use **OBS â†’ Tools â†’ Zoom Output Manager** to save, load, and delete profiles interactively, or call `ZoomOutputProfile::save() / load() / list() / remove()` from code.

## Architecture Overview

```
OBS Studio
â””â”€â”€ obs-zoom-plugin  (no Zoom SDK dependency)
    â”œâ”€â”€ ZoomDock              â€” dockable Qt panel: animated CvStatusDot, join/leave,
    â”‚                           token-type selector, recovery countdown, output table;
    â”‚                           CvBanner first-run credentials notice; persists last
    â”‚                           meeting ID + display name
    â”œâ”€â”€ ZoomOAuthManager      â€” OAuth 2.0 PKCE: begin_authorization, handle_redirect_url,
    â”‚                           register_url_scheme, refresh_access_token_blocking,
    â”‚                           fetch_zak_blocking; DPAPI token storage on Windows;
    â”‚                           confidential + public client modes
    â”œâ”€â”€ ZoomEngineClient  â˜…  â€” IPC singleton: launches engine, owns pipes/sockets,
    â”‚                           tracks roster/speaker, dispatches frame callbacks,
    â”‚                           subscribe_spotlight / subscribe_screenshare
    â”œâ”€â”€ ZoomReconnectManager  â€” exponential back-off recovery after crash/disconnect;
    â”‚                           stores session credentials for re-join
    â”œâ”€â”€ ZoomSource            â€” per-source: reads I420+PCM from ShmRegion,
    â”‚                           AssignmentMode (Participant/ActiveSpeaker/Spotlight/ScreenShare),
    â”‚                           failover_participant_id, HwVideoPipeline, OBS hotkeys
    â”œâ”€â”€ HwVideoPipeline       â€” optional FFmpeg I420â†’NV12 (CUDA/VAAPI/VideoToolbox/QSV)
    â”œâ”€â”€ ZoomAudioRouter       â€” SDK audio fan-out to all registered sinks
    â”œâ”€â”€ ZoomOutputManager     â€” central source registry for runtime reconfiguration
    â”œâ”€â”€ ZoomOutputProfile     â€” named JSON profile persistence
    â”œâ”€â”€ ZoomControlServer     â€” TCP JSON API on port 19870 (hardened token auth);
    â”‚                           oauth_callback command for URL scheme forwarding
    â”œâ”€â”€ ZoomOscServer         â€” UDP OSC API on port 19871
    â”œâ”€â”€ cv-style.h / cv-widgets â€” CoreVideo stylesheet, CvStatusDot, CvBanner
    â””â”€â”€ zoom-types.h          â€” MeetingState, AssignmentMode, MeetingKind,
                                RecoveryReason, ParticipantInfo, ZoomJoinAuthTokensâ€¦

ZoomObsEngine  (separate child process â€” owns ALL Zoom SDK access)
â”œâ”€â”€ Zoom SDK 5.17+/7.x (auth, meeting+webinar join, participant/spotlight tracking,
â”‚                        raw video/audio capture)
â””â”€â”€ Communicates with plugin via:
    â”œâ”€â”€ JSON over named pipes (Windows) or Unix sockets (macOS/Linux)
    â”‚   Pluginâ†’Engine: init Â· join(kind) Â· leave Â· subscribe Â· subscribe_spotlight
    â”‚                  subscribe_screenshare Â· unsubscribe Â· quit
    â”‚   Engineâ†’Plugin: ready Â· auth_ok Â· auth_fail Â· joined Â· left Â· frame Â· audio
    â”‚                  participants(+spotlight_index/is_sharing_screen) Â· active_speaker Â· error
    â””â”€â”€ Named shared memory (ZoomObsPlugin_<uuid>) for I420 video + PCM audio frames

CoreVideoOAuthCallback  (thin helper binary â€” ships beside the plugin)
â”œâ”€â”€ Windows: CoreVideoOAuthCallback.exe â€” intercepts corevideo:// URI via registry;
â”‚            reads control-server port from OBS global config; POSTs oauth_callback command
â””â”€â”€ macOS:   CoreVideoOAuthCallback.app â€” registered with Launch Services for corevideo://;
             same forwarding behaviour
```

See the **[full documentation](https://corevideo.iamfatness.us/documentation/)** for all architecture diagrams including the ZoomEngineClient deep-dive, OAuth PKCE flow, assignment mode flows, auto-reconnect, hardware video acceleration, TCP + OSC API references, output profile format, and full IPC protocol reference.

## Project Structure

```
CoreVideo/
â”œâ”€â”€ CMakeLists.txt
â”œâ”€â”€ buildspec/
â”‚   â”œâ”€â”€ macos.cmake
â”‚   â””â”€â”€ windows.cmake
â”œâ”€â”€ cmake/
â”‚   â””â”€â”€ CoreVideoOAuthCallback-Info.plist.in  # macOS OAuth helper bundle plist
â”œâ”€â”€ data/locale/en-US.ini
â”œâ”€â”€ docs/                                     # GitHub Pages documentation
â”‚   â”œâ”€â”€ index.html
â”‚   â”œâ”€â”€ ZOOM_MARKETPLACE_OAUTH.md             # OAuth setup guide
â”‚   â””â”€â”€ policies/                             # Security & privacy policy documents
â”œâ”€â”€ engine/src/                               # ZoomObsEngine (owns ALL SDK access)
â”‚   â”œâ”€â”€ main.cpp                              # IPC loop, SDK auth/join/webinar, spotlight tracking
â”‚   â”œâ”€â”€ engine-video.cpp/h                    # IZoomSDKRenderer â†’ named shared memory (I420)
â”‚   â””â”€â”€ engine-audio.cpp/h                    # SDK audio â†’ named shared memory (PCM)
â””â”€â”€ src/                                      # OBS plugin (no SDK linkage)
    â”œâ”€â”€ plugin-main.cpp                       # Module load/unload, dock, Tools menu, SIGPIPE
    â”œâ”€â”€ zoom-source.*                         # Participant source: ShmRegion, AssignmentMode,
    â”‚                                         #   HwVideoPipeline, failover, hotkeys, placeholder
    â”œâ”€â”€ zoom-engine-client.*                  # IPC singleton: engine launch, spotlight/screenshare,
    â”‚                                         #   monitor thread, deferred join, roster callbacks
    â”œâ”€â”€ zoom-oauth.*                          # OAuth 2.0 PKCE: ZoomOAuthManager, ZAK fetch,
    â”‚                                         #   register_url_scheme, token refresh + DPAPI storage
    â”œâ”€â”€ oauth-callback-helper.cpp             # Windows: CoreVideoOAuthCallback.exe entry point
    â”œâ”€â”€ oauth-callback-helper-macos.mm        # macOS: CoreVideoOAuthCallback.app entry point
    â”œâ”€â”€ zoom-dock.*                           # Qt dockable join/leave/recovery control panel;
    â”‚                                         #   CvStatusDot, CvBanner, token-type selector
    â”œâ”€â”€ zoom-reconnect.*                      # Auto-reconnect with exponential back-off
    â”œâ”€â”€ zoom-types.h                          # MeetingState, AssignmentMode, MeetingKind,
    â”‚                                         #   RecoveryReason, ParticipantInfo, ZoomJoinAuthTokensâ€¦
    â”œâ”€â”€ cv-style.h                            # CoreVideo QSS stylesheet (dark theme, button roles)
    â”œâ”€â”€ cv-widgets.*                          # CvStatusDot (animated dot), CvBanner (notice strip)
    â”œâ”€â”€ hw-video-pipeline.*                   # FFmpeg I420â†’NV12 (CUDA/VAAPI/VideoToolbox/QSV)
    â”œâ”€â”€ zoom-audio-delegate.*                 # Mixed/isolated SDK audio â†’ OBS
    â”œâ”€â”€ zoom-audio-router.*                   # Central SDK audio fan-out
    â”œâ”€â”€ zoom-auth.*                           # JWT auth + observable auth state
    â”œâ”€â”€ zoom-meeting.*                        # Meeting state machine
    â”œâ”€â”€ zoom-participants.*                   # Roster, active speaker, spotlight callbacks
    â”œâ”€â”€ zoom-participant-audio-source.*       # Per-participant audio OBS source
    â”œâ”€â”€ zoom-interpretation-audio-source.*    # Language interpretation OBS source
    â”œâ”€â”€ zoom-video-delegate.*                 # Video frames, resolution, loss mode, preview
    â”œâ”€â”€ zoom-share-delegate.*                 # Screen share frames â†’ OBS
    â”œâ”€â”€ zoom-output-manager.*                 # Source registry + runtime reconfiguration
    â”œâ”€â”€ zoom-output-profile.*                 # Named JSON profile persistence
    â”œâ”€â”€ zoom-output-dialog.*                  # Qt Output Manager dialog
    â”œâ”€â”€ zoom-control-server.*                 # TCP JSON API (port 19870) + oauth_callback command
    â”œâ”€â”€ zoom-osc-server.*                     # UDP OSC API (port 19871)
    â”œâ”€â”€ zoom-settings.*                       # SDK key/secret/JWT + OAuth tokens + port persistence
    â”œâ”€â”€ zoom-settings-dialog.*                # Qt Settings dialog with OAuth section
    â”œâ”€â”€ zoom-credentials.h.in                 # Embedded SDK credentials (CMake-generated)
    â”œâ”€â”€ obs-zoom-version.h.in                 # Plugin version (CMake-generated)
    â”œâ”€â”€ engine-ipc.h                          # IPC constants + cross-platform helpers
    â””â”€â”€ obs-utils.*                           # OBS helper functions
```

## Security

See [SECURITY.md](SECURITY.md) for the vulnerability disclosure policy.

## License

See [LICENSE](LICENSE) for details.
