#pragma once
#include <filesystem>

// A single runtime is shared by the modern entry and the OBS 32 compatibility
// entries. Only package-relative locations are considered; never search PATH.
inline std::filesystem::path corevideo_runtime_directory(const std::filesystem::path &module)
{
    namespace fs = std::filesystem;
    const auto base = module.parent_path();
    fs::path shared;
    if (base.filename() == "64bit" && base.parent_path().filename() == "bin" &&
        base.parent_path().parent_path().filename() == "obs-zoom-plugin")
        shared = base.parent_path().parent_path();
    else if (base.filename() == "64bit" && base.parent_path().filename() == "obs-plugins")
        shared = base.parent_path().parent_path() / "plugins/obs-zoom-plugin";
    if (!shared.empty()) {
        std::error_code ec;
        if (fs::is_regular_file(shared / "obs-zoom-plugin.dll", ec))
            return shared;
    }
    return base; // callers report missing helpers; no unrelated fallback
}
