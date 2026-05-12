#pragma once
#include "zoom-output-manager.h"
#include <string>
#include <vector>

// Manages named configuration profiles for output assignments.
// Profiles are persisted as individual JSON files under the OBS plugin config dir.
namespace ZoomOutputProfile {

std::vector<std::string>    list();
bool                        save(const std::string &name,
                                 const std::vector<ZoomOutputInfo> &outputs);
std::vector<ZoomOutputInfo> load(const std::string &name);
bool                        remove(const std::string &name);

} // namespace ZoomOutputProfile
