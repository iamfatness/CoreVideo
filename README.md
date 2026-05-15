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

CoreVideo integrates the Zoom Meeting SDK into OBS — no screen capture or virtual camera required. The SDK runs in a dedicated `ZoomObsEngine` child process; the OBS plugin communicates with it through a `ZoomEngineClient` singleton over cross-platform IPC (named pipes on Windows, Unix sockets on macOS/Linux) with frame data delivered through named shared memory. It receives raw I420 video and 48 kHz PCM audio from any meeting participant and pushes them into OBS as native sources. Two external control interfaces (TCP JSON + UDP OSC) and named output profiles enable full broadcast automation.

📖 **[Full Documentation & Architecture Diagrams →](https://iamfatness.github.io/CoreVideo/)**

---

## Features

- **Raw video capture** — I420 YUV → BGRA, selectable 360p / 720p / 1080p resolution
- **Video loss mode** — hold last frame or go black immediately when a feed drops
- **Raw audio capture** — 48 kHz PCM, mono or stereo, with per-participant isolation
- **Language interpretation audio** — dedicated OBS source for any Zoom interpretation channel
- **Screen share capture** — dedicated OBS source type that follows active meeting share
- **Per-participant audio sources** — standalone OBS audio source per meeting participant
- **Participant roster** — live list with video, mute, and talking state
- **Active speaker mode** — configurable sensitivity (ms) and hold-time (ms) debounce
- **TCP control API** — JSON server on `127.0.0.1:19870` for scripts and dashboards
- **OSC control API** — UDP OSC server on `127.0.0.1:19871` for lighting consoles and broadcast hardware
- **Output profiles** — save and load named participant-to-source mappings as JSON files
- **Output manager** — Qt dialog and API for viewing and reconfiguring all sources at runtime
- **Hardened security** — constant-time token comparison, validated IPC input, sanitised participant IDs
- **Cross-platform IPC** — named pipes on Windows, Unix domain sockets on macOS and Linux
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
    ├── ZoomEngineClient  ★  — singleton; owns IPC channels and engine process lifecycle;
    │                          tracks roster + active speaker from engine events;
    │                          dispatches per-source on_frame / on_audio callbacks
    ├── ZoomSource            — video + audio per participant; reads I420 + PCM from
    │                           ShmRegion; debounced active-speaker mode; 360p/720p/1080p
    ├── ZoomOutputManager     — central source registry for runtime reconfiguration
    ├── ZoomOutputProfile     — named JSON profile persistence
    ├── ZoomControlServer     — TCP JSON API on port 19870 (hardened token auth)
    └── zoom-types.h          — shared enums: MeetingState, VideoResolution, ParticipantInfo…

ZoomObsEngine  (separate child process — owns ALL Zoom SDK access)
├── Zoom SDK (auth, meeting join/leave, participant/speaker tracking, raw video + audio)
└── Communicates with plugin via:
    ├── JSON over named pipes (Windows) or Unix sockets (macOS/Linux)
    │   Plugin→Engine: init · join · leave · subscribe · unsubscribe · quit
    │   Engine→Plugin: ready · auth_ok · joined · left · frame · audio
    │                  participants · active_speaker · error
    └── Named shared memory (ZoomObsPlugin_<uuid>) for bulk frame data
```

See the **[full documentation](https://iamfatness.github.io/CoreVideo/)** for all architecture diagrams including the ZoomEngineClient deep-dive, video/audio pipelines, screen share pipeline, debounced active-speaker flow, TCP + OSC API references, output profile format, and full IPC protocol reference.

## Project Structure

```
CoreVideo/
├── CMakeLists.txt
├── buildspec/
│   ├── macos.cmake
│   └── windows.cmake
├── data/locale/en-US.ini
├── docs/                                   # GitHub Pages documentation
├── engine/src/                             # ZoomObsEngine process (owns ALL SDK access)
│   ├── main.cpp                            # IPC event loop + SDK init + roster/speaker tracking
│   ├── engine-video.cpp/h                  # IZoomSDKRenderer → shared memory
│   └── engine-audio.cpp/h                  # SDK audio → shared memory
└── src/                                    # OBS plugin (no SDK dependency)
    ├── plugin-main.cpp                     # Module load/unload, engine lifecycle
    ├── zoom-source.*                       # Main participant source (reads ShmRegion)
    ├── zoom-engine-client.*                # Singleton: IPC to engine, roster, callbacks
    ├── zoom-types.h                        # Shared enums + structs
    ├── zoom-output-manager.*               # Source registry
    ├── zoom-output-profile.*               # Named profile persistence
    ├── zoom-output-dialog.*                # Qt Output Manager dialog
    ├── zoom-control-server.*               # TCP JSON API (port 19870)
    ├── zoom-settings.*                     # Settings persistence
    ├── zoom-settings-dialog.*              # Qt Settings dialog
    ├── engine-ipc.h                        # IPC constants + cross-platform helpers
    └── obs-utils.*                         # OBS helper functions
```

## Security

See [SECURITY.md](SECURITY.md) for the vulnerability disclosure policy.

## License

See [LICENSE](LICENSE) for details.
