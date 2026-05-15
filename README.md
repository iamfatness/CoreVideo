# CoreVideo

> **LICENSING REQUIREMENT — READ BEFORE USE**
>
> Capturing raw video and audio from Zoom meetings via the Meeting SDK requires
> a **Zoom Enhanced Media license** (or equivalent raw-data-enabled license) on
> the Zoom account that is hosting or joining the meeting. Operating without
> this license violates the [Zoom Marketplace developer agreement](https://marketplace.zoom.us/docs/api-reference/developer-agreement)
> and will cause the SDK to return a raw-data permission error at runtime.
>
> Contact your Zoom account representative or visit
> [Zoom Plans](https://zoom.us/pricing) to verify your entitlements before
> deploying this plugin in a production environment.

**OBS Studio plugin for live Zoom meeting video, audio, screen share, and language interpretation.**

CoreVideo integrates the Zoom Meeting SDK into OBS — no screen capture or virtual camera required. The SDK runs in a dedicated `ZoomObsEngine` child process; the OBS plugin communicates with it through a `ZoomEngineClient` singleton over cross-platform IPC (named pipes on Windows, Unix sockets on macOS/Linux) with frame data delivered through named shared memory. A built-in dockable control panel manages joining, and a `ZoomReconnectManager` handles automatic recovery after crashes or disconnects.

📖 **[Full Documentation & Architecture Diagrams →](https://iamfatness.github.io/CoreVideo/)**

---

## Features

- **Raw video capture** — I420 YUV, selectable 360p / 720p / 1080p resolution
- **Hardware-accelerated video** — optional FFmpeg I420→NV12 conversion via CUDA, VAAPI, VideoToolbox, or QSV (`-DCOREVIDEO_HW_ACCEL=ON`)
- **Video loss mode** — hold last frame or show black when a feed drops; shows color-bar placeholder before first frame
- **Raw audio capture** — 48 kHz PCM, mono or stereo, with per-participant audio isolation and mixer routing
- **Assignment modes** — each source independently follows: a fixed participant, the active speaker, a ZoomISO-style spotlight slot (1…N), or the active screen share
- **Failover participant** — configure a secondary participant that activates automatically when the primary leaves
- **Active speaker mode** — configurable sensitivity + hold-time debounce, liveness guard, supersede logic
- **Spotlight / ZoomISO** — subscribe a source to spotlight slot N; engine resolves which participant is spotlighted
- **Screen share capture** — source subscribes to the active meeting screen-share feed
- **Language interpretation audio** — dedicated OBS source for any Zoom interpretation channel
- **Per-participant audio sources** — standalone OBS audio source per meeting participant
- **Webinar support** — join Zoom Webinars using the dedicated SDK entry point (Webinar checkbox in control dock)
- **Participant roster** — live list with video, mute, talking, host, co-host, raised hand, spotlight slot, and screen-sharing state
- **Control dock** — dockable Qt panel with state indicator, join/leave, recovery countdown, and inline output table
- **Auto-reconnect** — exponential back-off recovery after engine crash, network drop, or unexpected disconnect
- **OBS hotkeys** — per-source hotkeys to enable/disable active speaker mode
- **TCP control API** — JSON server on `127.0.0.1:19870` for scripts and dashboards
- **OSC control API** — UDP OSC server on `127.0.0.1:19871` for lighting consoles and broadcast hardware
- **Output profiles** — save and load named participant-to-source mappings as JSON files
- **Output manager** — Qt dialog and API for viewing and reconfiguring all sources at runtime
- **JWT generation** — CoreVideo generates Meeting SDK JWTs locally from key+secret; manual override available
- **SDK 5.17.x and 7.x** — auto-detects flat and subfolder header layouts
- **Hardened security** — constant-time token comparison, validated IPC input, sanitised participant IDs, SIGPIPE handling
- **Multi-platform** — Windows (x64/arm64), macOS (universal arm64 + x86_64), Linux

## Requirements

| Dependency | Version | Notes |
|---|---|---|
| OBS Studio | 30+ | `libobs` + `obs-frontend-api` |
| CMake | 3.16+ | Build system |
| Qt | 6.x | Core + Network + Widgets |
| Zoom Meeting SDK | **5.17.x / 7.x** | Place in `third_party/zoom-sdk/`. Windows builds support the older flat header layout and the newer 7.x subfolder header layout. |
| C++ compiler | C++17 | MSVC 2022 / Clang 14+ / GCC 11+ |
| Zoom Developer Account | — | Meeting SDK key + secret. A manual SDK JWT may be supplied as an override. |

## Quick Start

1. **Get the Zoom SDK** — download from the [Zoom Developer Portal](https://developers.zoom.us/docs/meeting-sdk/releases/) and place it at `third_party/zoom-sdk/`. CMake auto-detects x64/arm64/x86 sub-layouts on Windows.

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

3. **Install into OBS**
   ```sh
   cmake --install build --prefix "/path/to/obs-studio"
   ```

4. **Configure credentials** — open OBS → **Tools → Zoom Plugin Settings** and enter your SDK Key and SDK Secret. CoreVideo generates a short-lived Meeting SDK JWT locally when joining. The JWT Token field is optional and overrides generated tokens when set. You may also set a control server token and custom ports.

5. **Join once, then assign outputs** — use the CoreVideo dock or the TCP/OSC control APIs to join the meeting once per OBS session. Then add **Zoom Participant**, **Zoom Participant Audio**, **Zoom Share**, or **Zoom Interpretation Audio** sources and assign them to participants or dynamic roles.

## Control APIs

### TCP JSON (port 19870)

```sh
# Meeting status
echo '{"cmd":"status"}' | nc 127.0.0.1 19870

# List participants with video/mute/talking state
echo '{"cmd":"list_participants"}' | nc 127.0.0.1 19870

# Reassign source to participant at runtime
echo '{"cmd":"assign_output","source":"Zoom Participant 1","participant_id":123,"isolate_audio":true,"audio_channels":"stereo"}' | nc 127.0.0.1 19870
```

Commands: `help`, `status`, `list_participants`, `list_outputs`, `assign_output`, `join`, `leave`.

### UDP OSC (port 19871)

| Address | Type tags | Action |
|---|---|---|
| `/zoom/status` | — | Reply: meeting state + active speaker |
| `/zoom/list_participants` | — | Reply: one `/zoom/participant` per user |
| `/zoom/list_outputs` | — | Reply: one `/zoom/output` per source |
| `/zoom/join` | `,sss` | meeting_id, passcode, display_name |
| `/zoom/leave` | — | Leave meeting |
| `/zoom/assign_output` | `,si[i]` | source, participant_id, [active_speaker] |
| `/zoom/assign_output/active_speaker` | `,s` | source |
| `/zoom/isolate_audio` | `,si` | source, 0\|1 |

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

- **Liveness flag** — a `shared_ptr<atomic<bool>>` captured in every in-flight lambda ensures deferred callbacks bail safely if the source is destroyed before the timer fires.
- **Supersede logic** — a new candidate replaces the pending one, restarting the sensitivity clock. Stale callbacks silently discard themselves.
- **Final verification** — before committing a switch the code re-checks that the candidate is still the active speaker, so no switch fires for someone who stopped talking during the hold window.
- **UI-thread commitment** — all state mutations run on the OBS UI thread via `obs_queue_task`, preventing data races with the properties panel.

### Audio isolation interaction

When **Isolate Audio** is also enabled, every speaker switch sends an updated `subscribe` command to the engine with the new participant ID and `isolate_audio=true`, so the audio track always follows the same participant as the video.

## Output Profiles

Named profiles save the full source-to-participant mapping to JSON files under:

```
obs-studio/plugin_config/obs-zoom-plugin/profiles/<name>.json
```

Use **OBS → Tools → Zoom Output Manager** to save, load, and delete profiles interactively, or call `ZoomOutputProfile::save() / load() / list() / remove()` from code.

## Architecture Overview

```
OBS Studio
└── obs-zoom-plugin  (no Zoom SDK dependency)
    ├── ZoomDock              — dockable Qt panel: join/leave, state indicator,
    │                           recovery countdown, inline output table
    ├── ZoomEngineClient  ★  — IPC singleton: launches engine, owns pipes/sockets,
    │                           tracks roster/speaker, dispatches frame callbacks,
    │                           subscribe_spotlight / subscribe_screenshare
    ├── ZoomReconnectManager  — exponential back-off recovery after crash/disconnect;
    │                           stores session credentials for re-join
    ├── ZoomSource            — per-source: reads I420+PCM from ShmRegion,
    │                           AssignmentMode (Participant/ActiveSpeaker/Spotlight/ScreenShare),
    │                           failover_participant_id, HwVideoPipeline, OBS hotkeys
    ├── HwVideoPipeline       — optional FFmpeg I420→NV12 (CUDA/VAAPI/VideoToolbox/QSV)
    ├── ZoomAudioRouter       — SDK audio fan-out to all registered sinks
    ├── ZoomOutputManager     — central source registry for runtime reconfiguration
    ├── ZoomOutputProfile     — named JSON profile persistence
    ├── ZoomControlServer     — TCP JSON API on port 19870 (hardened token auth)
    ├── ZoomOscServer         — UDP OSC API on port 19871
    └── zoom-types.h          — MeetingState, AssignmentMode, MeetingKind,
                                RecoveryReason, ParticipantInfo, VideoResolution…

ZoomObsEngine  (separate child process — owns ALL Zoom SDK access)
├── Zoom SDK 5.17+/7.x (auth, meeting+webinar join, participant/spotlight tracking,
│                        raw video/audio capture)
└── Communicates with plugin via:
    ├── JSON over named pipes (Windows) or Unix sockets (macOS/Linux)
    │   Plugin→Engine: init · join(kind) · leave · subscribe · subscribe_spotlight
    │                  subscribe_screenshare · unsubscribe · quit
    │   Engine→Plugin: ready · auth_ok · auth_fail · joined · left · frame · audio
    │                  participants(+spotlight_index/is_sharing_screen) · active_speaker · error
    └── Named shared memory (ZoomObsPlugin_<uuid>) for I420 video + PCM audio frames
```

See the **[full documentation](https://iamfatness.github.io/CoreVideo/)** for all architecture diagrams including the ZoomEngineClient deep-dive, assignment mode flows, auto-reconnect, hardware video acceleration, TCP + OSC API references, output profile format, and full IPC protocol reference.

## Project Structure

```
CoreVideo/
├── CMakeLists.txt
├── buildspec/
│   ├── macos.cmake
│   └── windows.cmake
├── data/locale/en-US.ini
├── docs/                                   # GitHub Pages documentation
├── engine/src/                             # ZoomObsEngine (owns ALL SDK access)
│   ├── main.cpp                            # IPC loop, SDK auth/join/webinar, spotlight tracking
│   ├── engine-video.cpp/h                  # IZoomSDKRenderer → named shared memory (I420)
│   └── engine-audio.cpp/h                  # SDK audio → named shared memory (PCM)
└── src/                                    # OBS plugin (no SDK linkage)
    ├── plugin-main.cpp                     # Module load/unload, dock, Tools menu, SIGPIPE
    ├── zoom-source.*                       # Participant source: ShmRegion, AssignmentMode,
    │                                       #   HwVideoPipeline, failover, hotkeys, placeholder
    ├── zoom-engine-client.*                # IPC singleton: engine launch, spotlight/screenshare,
    │                                       #   monitor thread, deferred join, roster callbacks
    ├── zoom-dock.*                         # Qt dockable join/leave/recovery control panel
    ├── zoom-reconnect.*                    # Auto-reconnect with exponential back-off
    ├── zoom-types.h                        # MeetingState, AssignmentMode, MeetingKind,
    │                                       #   RecoveryReason, ParticipantInfo, enums…
    ├── hw-video-pipeline.*                 # FFmpeg I420→NV12 (CUDA/VAAPI/VideoToolbox/QSV)
    ├── zoom-audio-delegate.*               # Mixed/isolated SDK audio → OBS
    ├── zoom-audio-router.*                 # Central SDK audio fan-out
    ├── zoom-auth.*                         # JWT auth + observable auth state
    ├── zoom-meeting.*                      # Meeting state machine
    ├── zoom-participants.*                 # Roster, active speaker, spotlight callbacks
    ├── zoom-participant-audio-source.*     # Per-participant audio OBS source
    ├── zoom-interpretation-audio-source.*  # Language interpretation OBS source
    ├── zoom-video-delegate.*               # Video frames, resolution, loss mode, preview
    ├── zoom-share-delegate.*               # Screen share frames → OBS
    ├── zoom-output-manager.*               # Source registry + runtime reconfiguration
    ├── zoom-output-profile.*               # Named JSON profile persistence
    ├── zoom-output-dialog.*                # Qt Output Manager dialog
    ├── zoom-control-server.*               # TCP JSON API (port 19870)
    ├── zoom-osc-server.*                   # UDP OSC API (port 19871)
    ├── zoom-settings.*                     # SDK key/secret/JWT + port persistence
    ├── zoom-settings-dialog.*              # Qt Settings dialog
    ├── zoom-credentials.h.in               # Embedded SDK credentials (CMake-generated)
    ├── obs-zoom-version.h.in               # Plugin version (CMake-generated)
    ├── engine-ipc.h                        # IPC constants + cross-platform helpers
    └── obs-utils.*                         # OBS helper functions
```

## Security

See [SECURITY.md](SECURITY.md) for the vulnerability disclosure policy.

## License

See [LICENSE](LICENSE) for details.
