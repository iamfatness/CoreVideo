# CoreVideo

**OBS Studio plugin for live Zoom meeting video, audio, screen share, and language interpretation.**

CoreVideo integrates the Zoom Meeting SDK directly into OBS — no screen capture or virtual camera required. It receives raw I420 video and 48 kHz PCM audio from any meeting participant, converts the frames, and pushes them into OBS as native sources. Two external control interfaces (TCP JSON + UDP OSC) and named output profiles enable full broadcast automation.

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
| Zoom Meeting SDK | 5.x | Place in `third_party/zoom-sdk/` |
| C++ compiler | C++17 | MSVC 2022 / Clang 14+ / GCC 11+ |
| Zoom Developer Account | — | SDK key, secret & JWT token |

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

3. **Install into OBS**
   ```sh
   cmake --install build --prefix "/path/to/obs-studio"
   ```

4. **Configure credentials** — open OBS → **Tools → Zoom Plugin Settings** and enter your SDK Key, SDK Secret, JWT token, and optionally a control server token and custom ports.

5. **Add sources** — in OBS, add a **Zoom Participant** source (video + audio), **Zoom Participant Audio** source (audio only), **Zoom Share** source (screen share), or **Zoom Interpretation Audio** source (language channel).

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

When **Isolate Audio** is also enabled, every speaker switch immediately calls `set_isolated_user(new_pid)` so the audio track always follows the same participant as the video.

## Output Profiles

Named profiles save the full source-to-participant mapping to JSON files under:

```
obs-studio/plugin_config/obs-zoom-plugin/profiles/<name>.json
```

Use **OBS → Tools → Zoom Output Manager** to save, load, and delete profiles interactively, or call `ZoomOutputProfile::save() / load() / list() / remove()` from code.

## Architecture Overview

```
OBS Studio
└── obs-zoom-plugin
    ├── ZoomAuth              — JWT auth, observable state, auto re-auth on expiry
    ├── ZoomMeeting           — meeting state machine
    ├── ZoomParticipants      — roster, mute/video/talking state, active-speaker tracking
    ├── ZoomAudioRouter       — sole SDK audio subscriber; fans out to all sinks
    ├── ZoomSource            — video + audio per participant (360p/720p/1080p, loss mode,
    │                           debounced active-speaker, isolation, display name)
    ├── ZoomParticipantAudioSource — standalone per-participant audio OBS source
    ├── ZoomInterpAudioSource — language interpretation channel OBS source
    ├── ZoomShareDelegate     — screen share frames → OBS
    ├── ZoomOutputManager     — central source registry for runtime reconfiguration
    ├── ZoomOutputProfile     — named JSON profile persistence
    ├── ZoomControlServer     — TCP JSON API on port 19870 (hardened token auth)
    └── ZoomOscServer         — UDP OSC API on port 19871

ZoomObsEngine  (separate process, all platforms)
└── Hosts Zoom SDK; communicates via:
    ├── JSON over named pipes (Windows) or Unix sockets (macOS/Linux)
    └── Named shared memory for video/audio frame data
```

See the **[full documentation](https://iamfatness.github.io/CoreVideo/)** for all architecture diagrams including the audio router fan-out, video pipeline with loss modes and preview callback, screen share pipeline, debounced active-speaker flow, TCP + OSC API references, output profile format, and IPC protocol.

## Project Structure

```
CoreVideo/
├── CMakeLists.txt
├── buildspec/
│   ├── macos.cmake
│   └── windows.cmake
├── data/locale/en-US.ini
├── docs/                                   # GitHub Pages documentation
├── engine/src/                             # ZoomObsEngine (Windows, macOS, Linux)
└── src/                                    # OBS plugin source
    ├── zoom-source.*                       # Main participant source
    ├── zoom-auth.*                         # JWT auth
    ├── zoom-meeting.*                      # Meeting state machine
    ├── zoom-participants.*                 # Roster + active speaker
    ├── zoom-audio-router.*                 # Audio dispatch hub
    ├── zoom-audio-delegate.*               # Mixed / isolated audio
    ├── zoom-participant-audio-source.*     # Per-participant audio source
    ├── zoom-interpretation-audio-source.*  # Language interpretation source
    ├── zoom-video-delegate.*               # Video frames + resolution + loss mode
    ├── zoom-share-delegate.*               # Screen share source
    ├── zoom-output-manager.*               # Source registry
    ├── zoom-output-profile.*               # Named profile persistence
    ├── zoom-output-dialog.*                # Qt Output Manager dialog
    ├── zoom-control-server.*               # TCP JSON API
    ├── zoom-osc-server.*                   # UDP OSC API
    ├── zoom-settings.*                     # Settings persistence
    └── zoom-settings-dialog.*              # Qt Settings dialog
```

## Security

See [SECURITY.md](SECURITY.md) for the vulnerability disclosure policy.

## License

See [LICENSE](LICENSE) for details.
