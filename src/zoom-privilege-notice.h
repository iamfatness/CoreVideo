#pragma once

#include <string>

// Host permission grants restart media automatically. Repeated pending
// reports describe the same episode and must produce identical notice text.
inline bool zoom_privilege_already_requested(const std::string &detail)
{
    return detail.find("already requested") != std::string::npos;
}

inline const char *zoom_privilege_notice_first_request()
{
    return "Waiting for the host to allow recording. Media will start automatically when permission is granted.";
}
inline const char *zoom_privilege_notice_still_pending()
{
    return zoom_privilege_notice_first_request();
}
inline std::string zoom_privilege_notice_text(const std::string &)
{
    return zoom_privilege_notice_first_request();
}

// These are raw_media_state wire values, distinct from terminal SDK failures.
inline std::string zoom_raw_media_state_notice(const std::string &state, const std::string &reason)
{
    if (state == "denied")
        return "Recording permission denied. Ask the host to allow recording; media will start automatically when granted.";
    if (state == "waiting_permission") {
        if (reason == "privilege_request_timeout")
            return "Recording permission request timed out. Ask the host to allow recording. Media will start automatically when permission is granted.";
        return zoom_privilege_notice_first_request();
    }
    if (state == "recovering") return "Recovering Zoom media. Waiting for the meeting and recording permission.";
    if (state == "starting") return "Starting Zoom media automatically.";
    return {};
}
