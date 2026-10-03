// The pinned FFmpeg archives are the ONLY authority for what CoreVideo will
// run as ISO FFmpeg (spec 2026-10-03). A blank field or a malformed hash ships
// a Download button that can never succeed, so pin completeness is checked here.
#include "ffmpeg-runtime-pins.h"

#include <cctype>
#include <cstring>
#include <iostream>

static int g_failures = 0;
static void check(bool ok, const char *what)
{
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}

static bool lower_hex64(const char *s)
{
    if (!s || std::strlen(s) != 64) return false;
    for (const char *p = s; *p; ++p)
        if (!(std::isdigit(static_cast<unsigned char>(*p)) || (*p >= 'a' && *p <= 'f')))
            return false;
    return true;
}

static void check_pin(const FfmpegRuntimePin &p, const char *name)
{
    std::cerr << "pin " << name << "\n";
    check(p.platform && *p.platform, "platform set");
    check(p.version && *p.version, "version set");
    check(p.host && *p.host, "host set");
    check(p.url && std::strncmp(p.url, "https://", 8) == 0, "url is https");
    check(lower_hex64(p.sha256), "sha256 is 64 lowercase hex chars");
    check(p.size_bytes > 1000000, "size is plausible");
    check(p.archive_exe && *p.archive_exe, "archive_exe set");
    check(p.exe_name && *p.exe_name, "exe_name set");
}

int main()
{
    check_pin(kFfmpegPinWindowsX64, "windows");
    check_pin(kFfmpegPinMacosArm64, "macos");
    check(std::strcmp(kFfmpegPinWindowsX64.version, kFfmpegPinMacosArm64.version) == 0,
          "both platforms pin the same FFmpeg version");
    check(std::strcmp(kFfmpegPinWindowsX64.exe_name, "ffmpeg.exe") == 0, "windows exe name");
    check(std::strcmp(kFfmpegPinMacosArm64.exe_name, "ffmpeg") == 0, "macos exe name");
    check(std::strcmp(kFfmpegLicenseDataFile, "ffmpeg/LICENSE-GPLv3.txt") == 0, "license data path");
#if defined(_WIN32)
    check(ffmpeg_runtime_pin_for_host() == &kFfmpegPinWindowsX64, "host pin is windows");
#endif
    if (g_failures) { std::cerr << g_failures << " failure(s)\n"; return 1; }
    std::cout << "ffmpeg runtime pins: all checks passed\n";
    return 0;
}
