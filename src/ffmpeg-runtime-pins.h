#pragma once
// The pinned upstream FFmpeg builds CoreVideo downloads for ISO recording when
// the operator asks (spec docs/superpowers/specs/2026-10-03-ffmpeg-runtime-
// download-design.md). Both are GPLv3 builds (--enable-gpl --enable-version3)
// that include libx264, the always-available last step of the ISO encoder
// fallback chain. The hash here is the ONLY authority: an upstream .sha256 is
// never fetched or trusted at runtime.
//
// To move to a newer FFmpeg: change both entries together (one version on
// both platforms), then run CoreVideoFfmpegRuntimeLiveTest on each platform.
// It downloads the real archive and fails if it no longer hashes to the pin.
// Pure: no Qt, no OBS.
#include <cstdint>

struct FfmpegRuntimePin {
    const char *platform;      // "windows-x64" / "macos-arm64"
    const char *version;       // shown to the operator
    const char *host;          // shown in the download prompt
    const char *url;
    const char *sha256;        // lowercase hex of the whole archive
    std::uint64_t size_bytes;  // exact archive size
    const char *archive_exe;   // path of the executable inside the zip
    const char *exe_name;      // file name installed under <root>/current
};

// gyan.dev GPL "essentials" build. Versioned GitHub releases are kept long
// term (7.1 from 2024 is still downloadable as of 2026-10-03).
inline constexpr FfmpegRuntimePin kFfmpegPinWindowsX64{
    "windows-x64",
    "9.0.2",
    "github.com (gyan.dev builds)",
    "https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip",
    "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba",
    114768076ULL,
    "ffmpeg-9.0.2-essentials_build/bin/ffmpeg.exe",
    "ffmpeg.exe",
};

// Martin Riedl static build, signed. Includes libx264 and h264_videotoolbox.
// The zip holds ONLY the binary (no license), hence kFfmpegLicenseDataFile.
inline constexpr FfmpegRuntimePin kFfmpegPinMacosArm64{
    "macos-arm64",
    "9.0.2",
    "ffmpeg.martin-riedl.de",
    "https://ffmpeg.martin-riedl.de/download/macos/arm64/1789931890_9.0.2/ffmpeg.zip",
    "c8ed4c4e6978a03c485edbfe4e0a5dc2380f8a30bba5150531b31b094492d924",
    28395699ULL,
    "ffmpeg",
    "ffmpeg",
};

// Canonical GPLv3 text shipped in the plugin's data dir and installed as
// LICENSE.txt beside the managed ffmpeg on every platform.
constexpr const char *kFfmpegLicenseDataFile = "ffmpeg/LICENSE-GPLv3.txt";

// The pin for the platform this binary was built for; nullptr where CoreVideo
// offers no managed FFmpeg (Intel macOS, Linux). Callers then fall back to
// PATH / a manually chosen ffmpeg.
inline const FfmpegRuntimePin *ffmpeg_runtime_pin_for_host()
{
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return &kFfmpegPinWindowsX64;
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return &kFfmpegPinMacosArm64;
#else
    return nullptr;
#endif
}
