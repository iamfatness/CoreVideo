# CoreVideo

**OBS Studio plugin for live Zoom meeting video, audio, and screen share capture.**

CoreVideo integrates the Zoom Meeting SDK directly into OBS — no screen capture or virtual camera required. It subscribes to raw video (I420 YUV) and audio (48 kHz PCM) from any meeting participant or the active speaker, converts the frames, and pushes them into OBS as first-class sources.

📖 **[Full Documentation & Architecture Diagrams →](https://iamfatness.github.io/CoreVideo/)**

---

## Features

- **Raw video capture** — I420 YUV frames converted to BGRA for OBS textures
- **Raw audio capture** — 48 kHz PCM with mono/stereo modes and per-participant isolation
- **Screen share capture** — dedicated OBS source type for any active meeting screen share
- **Per-participant audio sources** — standalone audio source type per meeting participant
- **Participant roster** — live list with video, mute, and talking state
- **Active speaker mode** — optionally track whoever is speaking instead of a fixed participant
- **TCP control API** — JSON server on `127.0.0.1:19870` for external automation (status, join, leave, list/assign outputs)
- **Output manager** — centrally reconfigure source assignments and audio modes at runtime
- **JWT authentication** — authenticates with your Zoom SDK key, secret, and JWT token
- **Settings dialog** — Qt GUI accessible from OBS → Tools → Zoom Plugin Settings
- **Cross-platform IPC** — named pipes on Windows, Unix domain sockets on macOS and Linux
- **Multi-platform** — Windows (x64/arm64), macOS (universal arm64 + x86_64), Linux

## Requirements

| Dependency | Version | Notes |
|---|---|---|
| OBS Studio | 30+ | Provides `libobs` and `obs-frontend-api` |
| CMake | 3.16+ | Build system |
| Qt | 6.x | Core + Network + Widgets |
| Zoom Meeting SDK | 5.x | Place in `third_party/zoom-sdk/` |
| C++ compiler | C++17 | MSVC 2022 (Windows) / Clang 14+ (macOS) / GCC 11+ (Linux) |
| Zoom Developer Account | — | Required for SDK key, secret & JWT token |

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

4. **Configure credentials** — open OBS → **Tools → Zoom Plugin Settings** and enter your SDK Key, SDK Secret, and JWT token.

5. **Add a source** — in OBS, add a **Zoom Participant** source, enter the meeting ID, and select a participant from the live roster.

## Control API

The plugin starts a TCP JSON server on `127.0.0.1:19870`. Send newline-delimited JSON:

```sh
# Check meeting status
echo '{"cmd":"status"}' | nc 127.0.0.1 19870

# List participants
echo '{"cmd":"list_participants"}' | nc 127.0.0.1 19870

# Reassign a source to a different participant at runtime
echo '{"cmd":"assign_output","source":"Zoom Participant 1","participant_id":123,"isolate_audio":true,"audio_channels":"stereo"}' | nc 127.0.0.1 19870
```

Available commands: `help`, `status`, `list_participants`, `list_outputs`, `assign_output`, `join`, `leave`.

## Architecture Overview

```
OBS Studio
└── obs-zoom-plugin
    ├── ZoomAuth              — SDK init & JWT authentication
    ├── ZoomMeeting           — meeting join / leave, state machine
    ├── ZoomParticipants      — roster & active-speaker tracking
    ├── ZoomAudioRouter       — central audio dispatch hub (sole SDK audio subscriber)
    ├── ZoomSource            — OBS source (video + mixed/isolated audio)
    ├── ZoomParticipantAudioSource — standalone per-participant audio source
    ├── ZoomShareDelegate     — screen share frames → OBS
    ├── ZoomOutputManager     — central registry for runtime source reconfiguration
    └── ZoomControlServer     — TCP JSON API on port 19870

ZoomObsEngine  (separate process, all platforms)
└── Hosts the Zoom SDK; communicates via:
    ├── JSON over named pipes (Windows) or Unix sockets (macOS/Linux)
    └── Named shared memory for video/audio frame data
```

See the **[full documentation](https://iamfatness.github.io/CoreVideo/)** for detailed architecture diagrams covering the audio router, video pipeline, audio pipeline, screen share pipeline, Windows IPC engine, control API reference, authentication flow, and meeting join sequence.

## Project Structure

```
CoreVideo/
├── CMakeLists.txt                  # Build config (plugin + engine, all platforms)
├── buildspec/
│   ├── macos.cmake                 # macOS universal binary settings
│   └── windows.cmake               # Windows MSVC settings
├── data/locale/en-US.ini           # UI string translations
├── docs/                           # GitHub Pages documentation
├── engine/src/                     # ZoomObsEngine (out-of-process SDK host)
└── src/                            # OBS plugin source
```

## License

See [LICENSE](LICENSE) for details.
