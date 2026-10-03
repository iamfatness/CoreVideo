# FFmpeg runtime download for ISO recording — design

Date: 2026-10-03. Status: approved in conversation, awaiting spec review.

## Problem

ISO recording runs an external `ffmpeg` program, and neither the Windows nor
the macOS package ships one. (The bundled `avcodec`/`avfilter` libraries are
only for the plugin's own video processing.) Operators must find, download and
point CoreVideo at an FFmpeg build themselves. On macOS this is broken outright:

- The Browse filter is `FFmpeg (ffmpeg.exe ffmpeg)`.
- The default `ffmpeg` is resolved on PATH, but OBS started from the Dock does
  not inherit the shell's PATH, so Homebrew's `/opt/homebrew/bin/ffmpeg` is not
  found even when it is installed.
- The encoder menu offers only NVIDIA, Intel and AMD encoders. There is no Apple
  VideoToolbox option, and the fallback chain has no macOS hardware step.

## Goal

An operator enables ISO recording and it works, on Windows and Apple Silicon
macOS, with no separate FFmpeg install and no path to type. CoreVideo fetches a
pinned FFmpeg build when the operator asks for it, verifies it, unpacks it into
its own per-user folder and uses it.

## Decisions (owner, 2026-10-03)

| Question | Decision |
|---|---|
| Platforms | Windows x64 and macOS Apple Silicon |
| When | In the app, on demand; offered the first time ISO recording needs it. Never during install, never silently. |
| Variant | GPL build, which keeps libx264 as the always-available last step of the encoder fallback chain |
| Approach | Plugin-native downloader in C++, sharing one code path across both platforms |

## Upstream sources (pinned)

Both pinned builds are FFmpeg 9.0.2.

| Platform | Archive | Notes |
|---|---|---|
| Windows x64 | `https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip` (114,768,076 bytes) | gyan.dev GPL "essentials" build. Versioned GitHub releases; older versions back to 7.1 (2024) are still downloadable. SHA-256 is computed and pinned during implementation. |
| macOS arm64 | `https://ffmpeg.martin-riedl.de/download/macos/arm64/1789931890_9.0.2/ffmpeg.zip` | Martin Riedl static build, signed. Includes `libx264` and `h264_videotoolbox`. Published SHA-256 `c8ed4c4e6978a03c485edbfe4e0a5dc2380f8a30bba5150531b31b094492d924`. |

The hash compiled into CoreVideo is the authority. An upstream `.sha256` file is
never trusted at runtime.

## Components

### `src/ffmpeg-runtime-pins.h` (pure; no Qt, no OBS)

One `FfmpegRuntimePin` per platform:

- URL, SHA-256 and expected byte size;
- path of the `ffmpeg` executable inside the archive;
- path(s) of the license file(s);
- version label (`9.0.2`), host name for UI copy, and approximate download size.

This file is the only thing edited to move to a newer FFmpeg.

### `src/ffmpeg-runtime-plan.h` (pure; tested)

- **Resolution order** for the FFmpeg the ISO recorder uses:
  1. an explicit operator path (anything other than the default `ffmpeg`);
  2. the CoreVideo-managed install;
  3. PATH;
  4. on macOS only, `/opt/homebrew/bin/ffmpeg` then `/usr/local/bin/ffmpeg`.

  It returns the chosen path and a source tag (`Explicit` / `Managed` / `Path` /
  `Homebrew` / `None`) for the status line.
- **Archive-entry safety.** An entry is rejected if it is absolute, has a drive
  letter, contains a `..` segment, or would resolve outside the staging root.
- **Install-state rules.** A managed install counts only when the executable,
  the license and `provenance.txt` are all present.

### `src/ffmpeg-runtime-installer.{h,cpp}` (Qt)

A process-wide singleton (`QObject`, Meyers pattern like `CvUpdateChecker`). It
emits `progress(bytes, total)`, `finished(ok, message)` and `state_changed()`.
Steps:

1. Download with `QNetworkAccessManager` to a temp file, following redirects.
   Abort if the size exceeds the pin by more than 10%. Cancel is supported.
2. Verify SHA-256 (`QCryptographicHash`) against the pin. On a mismatch, delete
   the temp file and change nothing.
3. Extract into a fresh staging folder with the OS tool, through `QProcess` on
   the UI thread. `QProcess` is banned only on media threads, per CLAUDE.md.
   - Windows: `%SystemRoot%\System32\tar.exe -xf` (bsdtar, present on Windows
     10 1803+ and 11).
   - macOS: `/usr/bin/ditto -x -k`.

   Before extracting, list the entries (`tar -tf`, or `zipinfo -1` on macOS) and
   apply the safety rule.
4. Copy only the pinned executable and license file(s) into
   `<install>/staging`, `chmod +x` it on macOS, and run `ffmpeg -version` from
   there to prove it starts.
5. Write `provenance.txt`: source URL, SHA-256, version, UTC timestamp, and the
   statement "downloaded at the operator's request directly from upstream; not
   redistributed by CoreVideo".
6. Swap in: rename the existing `current` to `previous`, rename `staging` to
   `current`, then delete `previous`. A failure at any step leaves the existing
   `current` untouched.

**Install root:** `obs_module_config_path("ffmpeg")`, which is per-user and
needs no elevation:

- Windows: `%APPDATA%\obs-studio\plugin_config\obs-zoom-plugin\ffmpeg\`
- macOS: `~/Library/Application Support/obs-studio/plugin_config/obs-zoom-plugin/ffmpeg/`

The executable lives at `ffmpeg/current/ffmpeg(.exe)`.

**Remove** deletes `ffmpeg/current` and clears a stored path that pointed at it.

## Operator experience (ISO Recorder dock)

- **Status line:**
  - "Managed FFmpeg 9.0.2" when the managed copy is in use;
  - "Using ffmpeg at `<path>`" when the operator set one or it was found;
  - an amber "FFmpeg not found" when there is none.
- **Buttons:** **Download FFmpeg (~115 MB)** sits beside Browse and Test.
  - While downloading it becomes a progress bar with Cancel.
  - With a managed install present it becomes **Remove**.
- **First use:** pressing Start ISO with no FFmpeg resolvable shows one dialog:
  "ISO recording needs FFmpeg. Download it now (~115 MB from `<host>`)?"
  with **Download** / **Choose existing…** / **Cancel**. Nothing downloads
  without that click.
- **After a successful download:** the path field shows the managed path, Test
  runs automatically, and the status reports the encoders found.
- **Errors are one plain line, and nothing changes.** Examples:
  - "Download failed: no internet connection."
  - "Download failed: file didn't match the expected checksum. Nothing was changed."
  - "Not enough disk space."
- The control API and OSC paths use the same resolution order. A missing FFmpeg
  error tells them to use the dock's Download button.

## macOS corrections (ship with this feature)

- **Browse filter:** `ffmpeg` on macOS; `ffmpeg.exe` on Windows only.
- **Homebrew lookup:** handled by the resolution order above.
- **Encoder menu on macOS:** "Apple VideoToolbox – H.264" (`h264_videotoolbox`)
  and "CPU – x264". The NVIDIA, Intel and AMD entries are Windows-only.
- **Encoder plan:**
  - `IsoEncoderAvailability` gains `videotoolbox`.
  - On macOS, "auto" picks VideoToolbox when it is available.
  - `iso_demote_encoder("h264_videotoolbox")` returns `libx264`.
  - The Windows chain is unchanged.

## Testing

- **`CoreVideoFfmpegRuntimePlan`** (host, no network):
  - the resolution order, including the macOS Homebrew fallback (behind a
    platform flag so it is testable on any host);
  - rejection of absolute, drive-letter, `..` and root-escaping entries;
  - install-state completeness;
  - hash-mismatch and oversize handling;
  - a swap that fails part-way leaves the old install intact.
- **`CoreVideoFfmpegRuntimePins`:** every field is populated, the hashes are 64
  hex characters, and both pins carry the same version.
- **`CoreVideoIsoEncoderPlan`** (extended): macOS "auto" prefers VideoToolbox,
  VideoToolbox demotes to x264, and the Windows behavior is unchanged.
- **`CoreVideoFfmpegRuntimeLiveTest`** (manual, not in CI): downloads the real
  pinned archive for the host platform and checks it hashes to the pin. Run it
  whenever a pin changes.
- **Live acceptance before release:**
  - Windows: Download from the dock, Test, record a short two-feed ISO.
  - Owner's Mac: the same, plus Gatekeeper accepts the downloaded binary,
    VideoToolbox is listed, and a VideoToolbox ISO records.

## Documentation

- `docs/policies/privacy-policy.md`: names `github.com` (gyan.dev builds) and
  `ffmpeg.martin-riedl.de`, contacted only when the operator clicks Download.
- A third-party notice: FFmpeg is GPL, downloaded by the user directly from
  upstream, and not redistributed by CoreVideo.
- The site's ISO recording docs, plus a CLAUDE.md entry for the pins file and
  how to move to a new FFmpeg.

## Out of scope

- Intel Macs (the macOS build is Apple Silicon only).
- Downloading during install.
- Automatic FFmpeg updates.
- Removing the managed copy on uninstall. **Remove** in the dock covers it; an
  elevated uninstaller cannot reliably reach every user's profile.
