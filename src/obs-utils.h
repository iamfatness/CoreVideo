#pragma once
#include <string>

namespace zoom_join_utils {

struct ParsedJoin {
    std::string meeting_id; // numeric only; empty if input could not be parsed
    std::string passcode;   // empty if not present in input
    std::string on_behalf_token;
    std::string user_zak;
    std::string app_privilege_token;
};

// Accepts either a raw meeting ID (digits, optionally with spaces/dashes) or
// a Zoom join URL (https://zoom.us/j/123?pwd=abc, .../wc/123/join, .../s/123,
// zoommtg://zoom.us/join?confno=123&pwd=abc, etc.) and extracts the numeric
// meeting ID plus the passcode if one is present in the query string.
//
// Returns {meeting_id="", passcode=""} when the input cannot be interpreted as
// either form.
ParsedJoin parse_join_input(const std::string &input);

// True if `s`, after stripping spaces and dashes, is a non-empty string of
// decimal digits. Convenience helper for UI-side validation.
bool is_valid_meeting_id(const std::string &s);

} // namespace zoom_join_utils
