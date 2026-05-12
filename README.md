# CoreVideo

**OBS Studio plugin for live Zoom meeting video and audio capture.**

CoreVideo integrates the Zoom Meeting SDK directly into OBS — no screen capture or virtual camera required. It subscribes to raw video (I420 YUV) and audio (48 kHz PCM) from any meeting participant or the active speaker, converts the frames, and pushes them into OBS as a first-class source.

📖 **[Full Documentation & Architecture Diagrams →](https://iamfatness.github.io/CoreVideo/)**

---

## Features

- **Raw video capture** — I420 YUV frames from the Zoom SDK converted to BGRA for OBS
- **Raw audio capture** — 48 kHz PCM with mono/stereo modes
- **Participant roster** — live list of meeting participants with active-speaker detection
- **Auto-follow speaker** — optionally track whoever is speaking instead of a fixed participant
- **JWT authentication** — authenticates with your Zoom SDK key, secret, and JWT token
- **Settings dialog** — Qt GUI accessible from OBS → Tools → Zoom Plugin Settings
- **Multi-platform** — Windows (x64) and macOS (universal arm64 + x86_64)
- **IPC engine (Windows)** — `ZoomObsEngine.exe` hosts the SDK in a separate process, communicating via named pipes

## Requirements

| Dependency | Version | Notes |
|---|---|---|
| OBS Studio | 30+ | Provides `libobs` and `obs-frontend-api` |
| CMake | 3.16+ | Build system |
| Qt | 6.x | Core + Widgets |
| Zoom Meeting SDK | 5.x | Place in `third_party/zoom-sdk/` |
| C++ compiler | C++17 | MSVC 2022 (Windows) / Clang 14+ (macOS) |
| Zoom Developer Account | — | Required for SDK key, secret & JWT token |

## Quick Start

1. **Get the Zoom SDK** — download from the [Zoom Developer Portal](https://developers.zoom.us/docs/meeting-sdk/releases/) and place it at `third_party/zoom-sdk/`.

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

5. **Add a source** — in OBS, add a **Zoom Participant** source, enter the meeting ID and passcode, and select a participant from the live roster.

## Architecture Overview

```
OBS Studio
└── obs-zoom-plugin
    ├── ZoomAuth          — SDK init & JWT authentication
    ├── ZoomMeeting       — meeting join / leave
    ├── ZoomParticipants  — roster & active-speaker tracking
    ├── ZoomSource        — OBS source implementation
    ├── VideoDelegate     — I420 → BGRA conversion → obs_source_output_video()
    └── AudioDelegate     — 48kHz PCM → obs_source_output_audio()

ZoomObsEngine.exe (Windows only)
└── Communicates with the plugin via named pipes (JSON) + shared memory (frames)
```

See the **[full documentation](https://iamfatness.github.io/CoreVideo/)** for detailed architecture diagrams covering the video pipeline, audio pipeline, Windows IPC engine, authentication flow, and meeting join sequence.

## Project Structure

```
CoreVideo/
├── CMakeLists.txt          # Top-level build config
├── buildspec/
│   ├── macos.cmake         # macOS universal binary settings
│   └── windows.cmake       # Windows MSVC settings
├── data/locale/en-US.ini   # UI string translations
├── docs/                   # GitHub Pages documentation
├── engine/src/             # Windows engine process (ZoomObsEngine.exe)
└── src/                    # OBS plugin source
```

## License

See [LICENSE](LICENSE) for details.
