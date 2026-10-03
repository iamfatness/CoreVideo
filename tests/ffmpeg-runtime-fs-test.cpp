// The managed-FFmpeg folder under the operator's OBS profile (spec
// 2026-10-03). The invariant every case here defends: a good install is never
// lost to a failed or interrupted update.
#include "ffmpeg-runtime-fs.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
static int g_failures = 0;
static void check(bool ok, const char *what)
{
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}
static void write(const fs::path &p, const std::string &body)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}
static std::string read(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static void fill(const fs::path &dir, const std::string &tag)
{
    write(dir / "ffmpeg.exe", tag);
    write(dir / "LICENSE.txt", "gpl");
    write(dir / "provenance.txt", "prov");
}

int main()
{
    // Profile path with spaces and non-ASCII characters must survive Windows filesystem encoding.
    const fs::path root = fs::temp_directory_path() /
        fs::u8path(u8"cv ffmpeg test José Müller") / "ffmpeg";
    fs::remove_all(root.parent_path());

    check(!cvff::install_complete(root, "ffmpeg.exe"), "nothing installed yet");

    // First install: staging -> current.
    fill(cvff::staging_dir(root), "v1");
    std::string err;
    check(cvff::swap_in_staging(root, &err), "first swap succeeds");
    check(cvff::install_complete(root, "ffmpeg.exe"), "complete after first swap");
    check(read(cvff::current_dir(root) / "ffmpeg.exe") == "v1", "v1 in place");
    check(!fs::exists(cvff::staging_dir(root)), "staging consumed");
    check(!fs::exists(cvff::previous_dir(root)), "no previous left behind");

    // Update: v2 replaces v1, old copy removed.
    fill(cvff::staging_dir(root), "v2");
    check(cvff::swap_in_staging(root, &err), "update swap succeeds");
    check(read(cvff::current_dir(root) / "ffmpeg.exe") == "v2", "v2 in place");
    check(!fs::exists(cvff::previous_dir(root)), "previous removed after update");

    // A swap with nothing staged fails AND leaves v2 intact.
    check(!cvff::swap_in_staging(root, &err), "swap without staging fails");
    check(!err.empty(), "failure explains itself");
    check(read(cvff::current_dir(root) / "ffmpeg.exe") == "v2", "v2 survives failed swap");

    // An incomplete current (license missing) is not an install.
    fs::remove(cvff::current_dir(root) / "LICENSE.txt");
    check(!cvff::install_complete(root, "ffmpeg.exe"), "missing license => incomplete");
    write(cvff::current_dir(root) / "LICENSE.txt", "gpl");

    // Leftovers from an interrupted run are cleaned while current survives.
    write(cvff::download_file(root), "partial");
    fill(cvff::staging_dir(root), "half");
    write(cvff::extract_dir(root) / "x" / "ffmpeg.exe", "junk");
    write(cvff::previous_dir(root) / "ffmpeg.exe", "stale");
    cvff::clean_leftovers(root);
    check(!fs::exists(cvff::download_file(root)), "download.part removed");
    check(!fs::exists(cvff::staging_dir(root)), "staging removed");
    check(!fs::exists(cvff::extract_dir(root)), "extract removed");
    check(!fs::exists(cvff::previous_dir(root)), "stale previous removed when current exists");
    check(read(cvff::current_dir(root) / "ffmpeg.exe") == "v2", "current untouched by cleanup");

    // A crash between current->previous and staging->current is recovered by restoring previous.
    fs::rename(cvff::current_dir(root), cvff::previous_dir(root));
    cvff::clean_leftovers(root);
    check(read(cvff::current_dir(root) / "ffmpeg.exe") == "v2", "previous restored after mid-swap crash");
    check(cvff::install_complete(root, "ffmpeg.exe"), "restored install complete");

    // swap_in_staging in crash state: only previous/ has the good install (no current/).
    // Staging "v3", swap directly without clean_leftovers first -> restore current from previous first.
    fs::rename(cvff::current_dir(root), cvff::previous_dir(root));
    fill(cvff::staging_dir(root), "v3");
    check(cvff::swap_in_staging(root, &err), "swap in crash state with staging succeeds");
    check(read(cvff::current_dir(root) / "ffmpeg.exe") == "v3", "v3 in place after crash recovery");
    check(!fs::exists(cvff::previous_dir(root)), "previous removed after swap");

    // swap_in_staging in crash state with empty staging: restore current from previous, fail on empty staging.
    fs::rename(cvff::current_dir(root), cvff::previous_dir(root));
    check(!cvff::swap_in_staging(root, &err), "swap in crash state with empty staging fails");
    check(read(cvff::current_dir(root) / "ffmpeg.exe") == "v3", "v3 restored from previous despite failed swap");
    check(cvff::install_complete(root, "ffmpeg.exe"), "restored install complete");

    // Remove refuses while the managed ffmpeg is in use.
    check(!cvff::remove_install(root, /*in_use=*/true, &err), "remove refused while in use");
    check(err.find("recording") != std::string::npos, "refusal mentions recording");
    check(cvff::install_complete(root, "ffmpeg.exe"), "refused remove leaves install");

    // remove_install in crash state: only previous/ present (no current/).
    fs::rename(cvff::current_dir(root), cvff::previous_dir(root));
    check(cvff::remove_install(root, false, &err), "remove in crash state succeeds");
    check(!fs::exists(cvff::current_dir(root)), "current gone after remove");
    check(!fs::exists(cvff::previous_dir(root)), "previous gone after remove (not restored)");

    // Remove with nothing installed is a no-op success.
    check(cvff::remove_install(root, false, &err), "remove with nothing installed succeeds");

    // clean_leftovers removes a leftover removing/ dir.
    write(cvff::removing_dir(root) / "x" / "ffmpeg.exe", "trash");
    cvff::clean_leftovers(root);
    check(!fs::exists(cvff::removing_dir(root)), "removing dir removed");

    fs::remove_all(root.parent_path());
    if (g_failures) { std::cerr << g_failures << " failure(s)\n"; return 1; }
    std::cout << "ffmpeg runtime fs: all checks passed\n";
    return 0;
}
