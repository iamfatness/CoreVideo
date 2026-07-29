#include "cv-version-compare.h"

#include <iostream>

static int g_failures = 0;

static void expect(const char *name, bool condition)
{
    if (!condition) {
        std::cerr << "FAIL: " << name << "\n";
        ++g_failures;
    }
}

int main()
{
    // Basic ordering, with and without the "v" prefix.
    expect("v0.1.27 -> v0.1.28 is newer",
           cv_is_newer_version("v0.1.27", "v0.1.28"));
    expect("0.1.27 -> 0.1.28 is newer (no v prefix on either side)",
           cv_is_newer_version("0.1.27", "0.1.28"));
    expect("v0.1.27 -> 0.1.28 is newer (mixed prefix)",
           cv_is_newer_version("v0.1.27", "0.1.28"));

    // Equal versions are never "newer", regardless of "v" prefix.
    expect("v0.1.27 == v0.1.27 is not newer",
           !cv_is_newer_version("v0.1.27", "v0.1.27"));
    expect("v0.1.27 == 0.1.27 is not newer (prefix-insensitive equality)",
           !cv_is_newer_version("v0.1.27", "0.1.27"));

    // Current ahead of "latest" (should not happen in practice, but must
    // never report an update).
    expect("v0.1.28 -> v0.1.27 is not newer",
           !cv_is_newer_version("v0.1.28", "v0.1.27"));

    // Major/minor take priority over patch.
    expect("v0.1.99 -> v1.0.0 is newer (major bump)",
           cv_is_newer_version("v0.1.99", "v1.0.0"));
    expect("v0.9.9 -> v0.10.0 is newer (minor bump, not string-compared)",
           cv_is_newer_version("v0.9.9", "v0.10.0"));
    expect("v0.10.0 -> v0.9.9 is not newer (minor 10 > 9 numerically)",
           !cv_is_newer_version("v0.10.0", "v0.9.9"));

    // Pre-release handling: a release beats a pre-release of the same core
    // version; a pre-release never counts as newer than the release it
    // precedes.
    expect("v0.1.28-beta.1 -> v0.1.28 is newer (release beats prerelease)",
           cv_is_newer_version("v0.1.28-beta.1", "v0.1.28"));
    expect("v0.1.28 -> v0.1.28-beta.1 is not newer (prerelease behind release)",
           !cv_is_newer_version("v0.1.28", "v0.1.28-beta.1"));
    expect("v0.1.28-alpha -> v0.1.28-beta is newer (lexicographic fallback)",
           cv_is_newer_version("v0.1.28-alpha", "v0.1.28-beta"));

    // Malformed / empty input must fail closed (never crash, never claim an
    // update is available).
    expect("empty latest is never newer", !cv_is_newer_version("v0.1.27", ""));
    expect("garbage latest parses as 0.0.0, not newer",
           !cv_is_newer_version("v0.1.27", "not-a-version"));
    expect("empty current vs valid latest is newer",
           cv_is_newer_version("", "v0.0.1"));

    if (g_failures > 0) {
        std::cerr << g_failures << " version-compare test(s) failed\n";
        return 1;
    }
    std::cout << "All version-compare tests passed\n";
    return 0;
}
