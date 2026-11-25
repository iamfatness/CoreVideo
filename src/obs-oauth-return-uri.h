#pragma once

#include <string_view>

inline constexpr char kObsOAuthReturnUri[] = "corevideo-obs://oauth/callback";

inline constexpr std::string_view obs_oauth_return_uri(std::string_view saved)
{
    // The old default is also CoreVideo Pro's callback. Migrate copies saved
    // by earlier OBS releases so an upgrade immediately uses its own scheme.
    if (saved.empty() || saved == "corevideo://oauth/callback")
        return kObsOAuthReturnUri;
    return saved;
}
