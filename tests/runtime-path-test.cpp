#include "corevideo-runtime-path.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("corevideo-path-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto modern = root / "plugins/obs-zoom-plugin";
    fs::create_directories(modern / "bin/64bit");
    fs::create_directories(root / "obs-plugins/64bit");
    std::ofstream(modern / "obs-zoom-plugin.dll").put('x');
    std::ofstream(modern / "CoreVideoOAuthCallback.exe").put('x');
    // An old helper must not steal the upgraded portable entry's runtime.
    std::ofstream(root / "obs-plugins/64bit/CoreVideoOAuthCallback.exe").put('x');
    int failures = 0;
    auto check = [&](const fs::path &entry, const fs::path &expected) {
        if (corevideo_runtime_directory(entry) != expected) { ++failures; std::cerr << entry << " resolved incorrectly\n"; }
    };
    check(modern / "obs-zoom-plugin.dll", modern);
    check(modern / "bin/64bit/obs-zoom-plugin.dll", modern);
    check(root / "obs-plugins/64bit/obs-zoom-plugin.dll", modern);
    check(root / "unrelated/bin/64bit/obs-zoom-plugin.dll", root / "unrelated/bin/64bit");
    fs::remove(modern / "CoreVideoOAuthCallback.exe");
    check(root / "obs-plugins/64bit/obs-zoom-plugin.dll", modern);
    fs::remove(modern / "obs-zoom-plugin.dll");
    check(root / "obs-plugins/64bit/obs-zoom-plugin.dll", root / "obs-plugins/64bit");
    fs::remove_all(root);
    return failures ? 1 : 0;
}

