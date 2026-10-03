#pragma once
// Decisions behind the managed FFmpeg (spec 2026-10-03): which ffmpeg the ISO
// recorder runs, which archive entries the installer may touch, when a
// download is oversized, and what provenance.txt says. Pure: the filesystem
// and PATH are injected so every branch is host-testable.
#include "ffmpeg-runtime-pins.h"

#include <cctype>
#include <cstdint>
#include <functional>
#include <string>

enum class FfmpegSource { Explicit, Managed, Path, Homebrew, None };

struct FfmpegResolveInputs {
    std::string configured;   // dock field / saved setting / API ffmpeg_path
    std::string managed_exe;  // <root>/current/<exe_name>, "" if no pin
    bool managed_complete = false;
    bool macos = false;
    std::function<bool(const std::string &)> file_exists;
    std::function<std::string(const std::string &)> find_on_path;  // "" = not found
};

struct FfmpegResolution {
    std::string path;
    FfmpegSource source = FfmpegSource::None;
    // The operator chose a path that no longer exists, and resolution fell
    // through to something else. The dock must say so: silently using a
    // different binary is how "my FFmpeg setting is ignored" reports start.
    bool configured_missing = false;
};

inline std::string ffmpeg_trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// The settings default ("ffmpeg") and its Windows spelling mean "nothing
// chosen" -- resolution may pick the managed copy over them.
inline bool ffmpeg_is_default_name(const std::string &configured)
{
    const std::string t = ffmpeg_trim(configured);
    return t.empty() || t == "ffmpeg" || t == "ffmpeg.exe";
}

inline bool ffmpeg_is_relative_name(const std::string &p)
{
    return p.find('/') == std::string::npos && p.find('\\') == std::string::npos &&
           !(p.size() >= 2 && p[1] == ':');
}

inline FfmpegResolution ffmpeg_resolve(const FfmpegResolveInputs &in)
{
    FfmpegResolution r;
    const std::string configured = ffmpeg_trim(in.configured);
    const bool managed_ok =
        in.managed_complete && !in.managed_exe.empty() && in.file_exists(in.managed_exe);

    if (!ffmpeg_is_default_name(configured)) {
        if (managed_ok && configured == in.managed_exe) {
            r.path = in.managed_exe;
            r.source = FfmpegSource::Managed;
            return r;
        }
        const std::string found = ffmpeg_is_relative_name(configured)
            ? in.find_on_path(configured)
            : (in.file_exists(configured) ? configured : std::string());
        if (!found.empty()) {
            r.path = found;
            r.source = FfmpegSource::Explicit;
            return r;
        }
        r.configured_missing = true;
    }
    if (managed_ok) {
        r.path = in.managed_exe;
        r.source = FfmpegSource::Managed;
        return r;
    }
    const std::string on_path = in.find_on_path("ffmpeg");
    if (!on_path.empty()) {
        r.path = on_path;
        r.source = FfmpegSource::Path;
        return r;
    }
    if (in.macos) {
        // OBS launched from the Dock does not inherit the shell's PATH, so
        // Homebrew's ffmpeg is invisible to findExecutable even when installed.
        for (const char *candidate : {"/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"}) {
            if (in.file_exists(candidate)) {
                r.path = candidate;
                r.source = FfmpegSource::Homebrew;
                return r;
            }
        }
    }
    return r;
}

// True when extracting `entry` cannot write outside the staging root.
// Rejects absolute paths, UNC paths, parent-directory segments (..), control
// characters (which enable hiding payloads), NTFS alternate data streams (:),
// and dot/space-only segments (Windows normalizes ".. " and ". " as ".." and ".").
inline bool ffmpeg_archive_entry_safe(const std::string &entry)
{
    if (entry.empty()) return false;
    // Reject absolute paths (Unix and Windows).
    if (entry[0] == '/' || entry[0] == '\\') return false;
    // Reject drive letters and UNC paths at the start.
    if (entry.size() >= 2 && entry[1] == ':') return false;

    // Reject control characters (0x00–0x1F, 0x7F) and NTFS alternate data
    // streams (:). These can be used to escape the archive root.
    for (unsigned char c : entry) {
        if (c < 0x20 || c == 0x7F || c == ':') return false;
    }

    // Segment-by-segment check: reject "..", UNC-like starts, and dot/space
    // segments (Windows normalizes them, e.g., ".. " → "..", ". " → ".").
    size_t start = 0;
    while (start <= entry.size()) {
        size_t end = entry.find_first_of("/\\", start);
        if (end == std::string::npos) end = entry.size();

        const size_t len = end - start;
        const std::string segment = entry.substr(start, len);

        // Reject ".." exactly.
        if (len == 2 && segment == "..") return false;

        // Reject segments that are only '.' and ' ' chars, except "." alone.
        if (len > 0) {
            bool all_dot_space = true;
            for (char c : segment) {
                if (c != '.' && c != ' ') {
                    all_dot_space = false;
                    break;
                }
            }
            // If all chars are dots and spaces, reject unless it's exactly ".".
            if (all_dot_space && !(len == 1 && segment == ".")) return false;
        }

        start = end + 1;
    }
    return true;
}

// Abort a download that runs more than 10% past the pinned size.
inline bool ffmpeg_download_size_ok(std::uint64_t received, std::uint64_t expected)
{
    return received <= expected + expected / 10;
}

inline std::string ffmpeg_provenance_text(const FfmpegRuntimePin &pin,
                                          const std::string &utc_iso8601)
{
    std::string t;
    t += "FFmpeg " + std::string(pin.version) + " (" + pin.platform + ")\n";
    t += "Source: " + std::string(pin.url) + "\n";
    t += "SHA-256: " + std::string(pin.sha256) + "\n";
    t += "Installed: " + utc_iso8601 + "\n";
    t += "License: GPLv3 (see LICENSE.txt)\n";
    t += "Downloaded at the operator's request directly from upstream; "
         "not redistributed by CoreVideo.\n";
    return t;
}
