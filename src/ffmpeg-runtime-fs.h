#pragma once
// The managed-FFmpeg folder layout (spec 2026-10-03):
//   <root>/current/{<exe>, LICENSE.txt, provenance.txt}   the live install
//   <root>/staging/   a verified candidate, swapped in atomically by rename
//   <root>/previous/  the old install during a swap (restored on failure)
//   <root>/extract/   raw unzip output, discarded
//   <root>/download.part
//   <root>/removing/  trash dir during atomic removal (leftover from partial failure)
// std::filesystem only, so it is host-testable with temp folders.
#include <filesystem>
#include <string>

namespace cvff {
using path = std::filesystem::path;

inline path current_dir(const path &root) { return root / "current"; }
inline path staging_dir(const path &root) { return root / "staging"; }
inline path previous_dir(const path &root) { return root / "previous"; }
inline path extract_dir(const path &root) { return root / "extract"; }
inline path download_file(const path &root) { return root / "download.part"; }
inline path removing_dir(const path &root) { return root / "removing"; }

bool install_complete(const path &root, const std::string &exe_name);
void clean_leftovers(const path &root);
bool swap_in_staging(const path &root, std::string *error);
bool remove_install(const path &root, bool in_use, std::string *error);
}
