#pragma once
// Header-only, dependency-free semver-ish comparison used by the update
// check (CvUpdateChecker) to decide whether a GitHub release tag is newer
// than the version this build reports. Kept free of Qt/OBS includes so it
// can be unit tested directly (see tests/version-compare-test.cpp), the
// same pattern zoom-output-health.h uses for CoreVideoOutputHealthTest.

#include <cstdlib>
#include <string>
#include <vector>

struct CvSemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease;  // text after the first '-'; empty for a release
};

// Parses a tag like "v0.1.27", "0.1.27", or "v0.1.28-beta.1". A leading
// 'v'/'V' is stripped. Missing numeric components default to 0, and
// non-numeric components parse as 0 rather than throwing, so a malformed
// string from the network never crashes the check - it just compares as
// low/equal and the update banner stays hidden.
inline CvSemVer cv_parse_version(const std::string &raw)
{
    CvSemVer v;
    std::string s = raw;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
        s.erase(0, 1);

    const size_t dash = s.find('-');
    const std::string core = (dash == std::string::npos) ? s : s.substr(0, dash);
    if (dash != std::string::npos)
        v.prerelease = s.substr(dash + 1);

    std::vector<int> parts;
    size_t start = 0;
    while (start <= core.size()) {
        const size_t dot = core.find('.', start);
        const std::string token = core.substr(
            start, dot == std::string::npos ? std::string::npos : dot - start);
        parts.push_back(token.empty() ? 0 : std::atoi(token.c_str()));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    if (parts.size() > 0) v.major = parts[0];
    if (parts.size() > 1) v.minor = parts[1];
    if (parts.size() > 2) v.patch = parts[2];
    return v;
}

// Returns true only if `latest_raw` is a strictly newer version than
// `current_raw`. Equal versions, older versions, and unparsable/empty
// strings all return false (fail closed - no banner rather than a bogus one).
inline bool cv_is_newer_version(const std::string &current_raw,
                                const std::string &latest_raw)
{
    if (latest_raw.empty())
        return false;

    const CvSemVer current = cv_parse_version(current_raw);
    const CvSemVer latest = cv_parse_version(latest_raw);

    if (latest.major != current.major) return latest.major > current.major;
    if (latest.minor != current.minor) return latest.minor > current.minor;
    if (latest.patch != current.patch) return latest.patch > current.patch;

    // Same major.minor.patch. Per SemVer precedence, a release outranks a
    // pre-release of the same core version.
    if (current.prerelease.empty())
        return false;  // current is already a release (or equal prerelease-less tag)
    if (latest.prerelease.empty())
        return true;   // latest dropped the pre-release suffix - it's newer
    return latest.prerelease > current.prerelease;  // lexicographic fallback
}
