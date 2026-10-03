// Which FFmpeg the ISO recorder runs, and which archive entries the installer
// will touch (spec 2026-10-03). The resolution order is what makes a Download
// button "just work" without overriding an operator's own choice.
#include "ffmpeg-runtime-plan.h"

#include <iostream>
#include <set>
#include <string>

static int g_failures = 0;
static void check(bool ok, const char *what)
{
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}

static FfmpegResolveInputs base(std::set<std::string> files, std::string on_path = "")
{
    FfmpegResolveInputs in;
    in.file_exists = [files](const std::string &p) { return files.count(p) != 0; };
    in.find_on_path = [on_path](const std::string &) { return on_path; };
    return in;
}

int main()
{
    const std::string managed = "/home/u/cfg/ffmpeg/current/ffmpeg";

    // Default names mean "not chosen by the operator".
    check(ffmpeg_is_default_name(""), "empty is default");
    check(ffmpeg_is_default_name("ffmpeg"), "ffmpeg is default");
    check(ffmpeg_is_default_name("ffmpeg.exe"), "ffmpeg.exe is default");
    check(ffmpeg_is_default_name("  ffmpeg  "), "trimmed default");
    check(!ffmpeg_is_default_name("/opt/ff/ffmpeg"), "absolute path is a choice");

    // 1. Explicit operator path that exists wins over everything.
    {
        auto in = base({"/opt/ff/ffmpeg", managed}, "/usr/bin/ffmpeg");
        in.configured = "/opt/ff/ffmpeg";
        in.managed_exe = managed; in.managed_complete = true;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Explicit && r.path == "/opt/ff/ffmpeg", "explicit wins");
        check(!r.configured_missing, "explicit present is not missing");
    }
    // An explicit path equal to the managed exe reports Managed (the dock
    // writes the managed path into the field after a download).
    {
        auto in = base({managed});
        in.configured = managed; in.managed_exe = managed; in.managed_complete = true;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Managed && r.path == managed, "configured==managed reports Managed");
    }
    // REVIEW FOCUS 1: a saved path that vanished falls through AND says so.
    {
        auto in = base({managed});
        in.configured = "D:/tools/ffmpeg.exe";
        in.managed_exe = managed; in.managed_complete = true;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Managed && r.path == managed, "missing explicit falls to managed");
        check(r.configured_missing, "missing explicit is reported");
    }
    // 2. Managed beats PATH when nothing explicit is set.
    {
        auto in = base({managed}, "/usr/bin/ffmpeg");
        in.configured = "ffmpeg"; in.managed_exe = managed; in.managed_complete = true;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Managed, "managed beats PATH");
    }
    // An incomplete managed install (no license/provenance) does not count.
    {
        auto in = base({managed}, "/usr/bin/ffmpeg");
        in.configured = "ffmpeg"; in.managed_exe = managed; in.managed_complete = false;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Path && r.path == "/usr/bin/ffmpeg", "incomplete managed is skipped");
    }
    // 3. PATH.
    {
        auto in = base({}, "C:/ffmpeg/bin/ffmpeg.exe");
        in.configured = "ffmpeg";
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Path && r.path == "C:/ffmpeg/bin/ffmpeg.exe", "PATH found");
    }
    // A relative non-default name is looked up on PATH and counts as Explicit.
    {
        auto in = base({}, "/usr/bin/ffmpeg7");
        in.configured = "ffmpeg7";
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Explicit && r.path == "/usr/bin/ffmpeg7", "relative explicit via PATH");
    }
    // 4. macOS Homebrew fallback, Apple Silicon prefix first, only on macOS.
    {
        auto in = base({"/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"});
        in.configured = "ffmpeg"; in.macos = true;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Homebrew && r.path == "/opt/homebrew/bin/ffmpeg", "homebrew arm prefix first");
    }
    {
        auto in = base({"/usr/local/bin/ffmpeg"});
        in.configured = "ffmpeg"; in.macos = true;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::Homebrew && r.path == "/usr/local/bin/ffmpeg", "homebrew intel prefix");
    }
    {
        auto in = base({"/opt/homebrew/bin/ffmpeg"});
        in.configured = "ffmpeg"; in.macos = false;
        const auto r = ffmpeg_resolve(in);
        check(r.source == FfmpegSource::None && r.path.empty(), "no homebrew lookup off macOS");
    }

    // Archive-entry safety: anything that could land outside the staging root.
    check(ffmpeg_archive_entry_safe("ffmpeg"), "plain entry ok");
    check(ffmpeg_archive_entry_safe("ffmpeg-9.0.2-essentials_build/bin/ffmpeg.exe"), "nested entry ok");
    check(ffmpeg_archive_entry_safe("a/b/"), "directory entry ok");
    check(!ffmpeg_archive_entry_safe(""), "empty rejected");
    check(!ffmpeg_archive_entry_safe("/etc/passwd"), "absolute rejected");
    check(!ffmpeg_archive_entry_safe("\\Windows\\x.dll"), "backslash-rooted rejected");
    check(!ffmpeg_archive_entry_safe("C:/x.exe"), "drive letter rejected");
    check(!ffmpeg_archive_entry_safe("c:x.exe"), "drive-relative rejected");
    check(!ffmpeg_archive_entry_safe("../x"), "parent segment rejected");
    check(!ffmpeg_archive_entry_safe("a/../../x"), "nested parent rejected");
    check(!ffmpeg_archive_entry_safe("a\\..\\x"), "backslash parent rejected");
    check(ffmpeg_archive_entry_safe("a/..b/x"), "'..b' is a name, not a parent");
    // Additional security tests: parent at start, end, UNC paths, dot/space normalization.
    check(!ffmpeg_archive_entry_safe(".."), "parent alone rejected");
    check(!ffmpeg_archive_entry_safe("a/.."), "parent at end rejected");
    check(!ffmpeg_archive_entry_safe("a\\.."), "parent with backslash rejected");
    check(!ffmpeg_archive_entry_safe("\\\\server\\share\\x"), "UNC path rejected");
    check(!ffmpeg_archive_entry_safe("//server/x"), "forward-slash UNC rejected");
    check(ffmpeg_archive_entry_safe("a/./b"), "lone dot segment accepted");
    check(!ffmpeg_archive_entry_safe("..."), "three dots rejected");
    check(!ffmpeg_archive_entry_safe(".. "), "dot-dot-space rejected");
    check(!ffmpeg_archive_entry_safe("a/... /x"), "dot-space segment rejected");
    check(!ffmpeg_archive_entry_safe("ffmpeg.exe:stream"), "NTFS ADS rejected");
    check(!ffmpeg_archive_entry_safe(std::string("..\0/x", 5)), "NUL char rejected");
    // Additional space/dot segment tests for Windows normalization.
    check(!ffmpeg_archive_entry_safe(" ."), "space-dot segment rejected");
    check(!ffmpeg_archive_entry_safe(". "), "dot-space segment (single) rejected");
    check(!ffmpeg_archive_entry_safe(" "), "space-only segment rejected");
    check(!ffmpeg_archive_entry_safe("a/ /b"), "space-only in path rejected");
    check(!ffmpeg_archive_entry_safe(std::string("a\x7f" "b", 3)), "0x7F control char rejected");
    check(!ffmpeg_archive_entry_safe("a\nb"), "newline control char rejected");
    check(ffmpeg_archive_entry_safe("."), "lone dot accepted");
    check(ffmpeg_archive_entry_safe("a.b"), "dot in name accepted");

    // Download size rule: abort past expected + 10%.
    check(ffmpeg_download_size_ok(100, 100), "exact ok");
    check(ffmpeg_download_size_ok(110, 100), "+10% ok");
    check(!ffmpeg_download_size_ok(111, 100), "over +10% aborts");

    // Provenance text names source, hash, version, time and the redistribution note.
    {
        const std::string t = ffmpeg_provenance_text(kFfmpegPinWindowsX64, "2026-10-03T12:00:00Z");
        check(t.find(kFfmpegPinWindowsX64.url) != std::string::npos, "provenance has url");
        check(t.find(kFfmpegPinWindowsX64.sha256) != std::string::npos, "provenance has hash");
        check(t.find("9.0.2") != std::string::npos, "provenance has version");
        check(t.find("2026-10-03T12:00:00Z") != std::string::npos, "provenance has time");
        check(t.find("not redistributed by CoreVideo") != std::string::npos, "provenance has redistribution note");
    }

    if (g_failures) { std::cerr << g_failures << " failure(s)\n"; return 1; }
    std::cout << "ffmpeg runtime plan: all checks passed\n";
    return 0;
}
