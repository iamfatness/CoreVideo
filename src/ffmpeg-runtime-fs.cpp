#include "ffmpeg-runtime-fs.h"

namespace fs = std::filesystem;

namespace cvff {

bool install_complete(const path &root, const std::string &exe_name)
{
    std::error_code ec;
    const path cur = current_dir(root);
    return fs::is_regular_file(cur / exe_name, ec) &&
           fs::is_regular_file(cur / "LICENSE.txt", ec) &&
           fs::is_regular_file(cur / "provenance.txt", ec);
}

void clean_leftovers(const path &root)
{
    std::error_code ec;
    fs::remove(download_file(root), ec);
    fs::remove_all(staging_dir(root), ec);
    fs::remove_all(extract_dir(root), ec);
    // A crash between the two renames in swap_in_staging leaves the good
    // install in previous/ and no current/. Restore it -- never delete it.
    if (!fs::exists(current_dir(root), ec) && fs::exists(previous_dir(root), ec))
        fs::rename(previous_dir(root), current_dir(root), ec);
    else
        fs::remove_all(previous_dir(root), ec);
}

bool swap_in_staging(const path &root, std::string *error)
{
    std::error_code ec;
    if (!fs::is_directory(staging_dir(root), ec)) {
        if (error) *error = "Nothing staged to install.";
        return false;
    }
    fs::remove_all(previous_dir(root), ec);
    const bool had_current = fs::exists(current_dir(root), ec);
    if (had_current) {
        fs::rename(current_dir(root), previous_dir(root), ec);
        if (ec) {
            if (error) *error = "Could not move the existing FFmpeg aside: " + ec.message();
            return false;
        }
    }
    fs::rename(staging_dir(root), current_dir(root), ec);
    if (ec) {
        const std::string why = ec.message();
        if (had_current) {
            std::error_code back;
            fs::rename(previous_dir(root), current_dir(root), back);
        }
        if (error) *error = "Could not install the new FFmpeg: " + why;
        return false;
    }
    fs::remove_all(previous_dir(root), ec);
    return true;
}

bool remove_install(const path &root, bool in_use, std::string *error)
{
    if (in_use) {
        if (error) *error = "Stop ISO recording before removing FFmpeg.";
        return false;
    }
    std::error_code ec;
    fs::remove_all(current_dir(root), ec);
    if (ec) {
        if (error) *error = "Could not remove FFmpeg: " + ec.message();
        return false;
    }
    clean_leftovers(root);
    return true;
}

}  // namespace cvff
