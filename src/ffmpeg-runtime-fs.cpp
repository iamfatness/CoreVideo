#include "ffmpeg-runtime-fs.h"

namespace fs = std::filesystem;

namespace cvff {

namespace {

// Recover from a mid-swap crash: if current/ doesn't exist but previous/ does,
// restore previous back to current. This may fail, but we do not treat failure
// as an error condition since the good install is already lost if rename fails.
static void restore_interrupted_swap(const path &root)
{
    std::error_code ec;
    if (!fs::exists(current_dir(root), ec) && fs::exists(previous_dir(root), ec))
        fs::rename(previous_dir(root), current_dir(root), ec);
}

}  // namespace

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
    fs::remove_all(removing_dir(root), ec);
    // A crash between the two renames in swap_in_staging leaves the good
    // install in previous/ and no current/. Restore it -- never delete it.
    if (!fs::exists(current_dir(root), ec) && fs::exists(previous_dir(root), ec))
        restore_interrupted_swap(root);
    else
        fs::remove_all(previous_dir(root), ec);
}

bool swap_in_staging(const path &root, std::string *error)
{
    std::error_code ec;
    // Recover from a mid-swap crash first: if current/ is missing but previous/
    // exists, restore it to current. This is the crash-safe invariant and must
    // happen before we check staging, so a crashed state is recovered even if
    // the subsequent swap fails.
    restore_interrupted_swap(root);

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
            if (back) {
                if (error) *error = "Could not install the new FFmpeg: " + why + ". The previous FFmpeg could not be put back: " + back.message();
                return false;
            }
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

    // Atomically rename current/ to trash. If rename fails, current/ is untouched.
    const path trash = removing_dir(root);
    fs::remove_all(trash, ec);  // Clear any leftover trash from a previous partial removal
    fs::rename(current_dir(root), trash, ec);
    if (ec) {
        // current/ doesn't exist is not an error (remove of nothing is a no-op success)
        if (fs::exists(current_dir(root), ec)) {
            if (error) *error = "Could not remove FFmpeg: " + ec.message();
            return false;
        }
    }

    // Clean up the trash and all other leftovers, but don't run the restore rule.
    std::error_code ec2;
    fs::remove(download_file(root), ec2);
    fs::remove_all(staging_dir(root), ec2);
    fs::remove_all(extract_dir(root), ec2);
    fs::remove_all(previous_dir(root), ec2);  // Never restore previous in remove_install
    fs::remove_all(trash, ec2);  // Ignore errors cleaning trash

    return true;
}

}  // namespace cvff
