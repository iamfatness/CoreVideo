#include "obs-utils.h"

#include <cctype>
#include <cstring>

namespace zoom_join_utils {

namespace {

std::string strip_whitespace(const std::string &s)
{
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

bool all_digits(const std::string &s)
{
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

std::string lower_ascii(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_value(s[i + 1]);
            const int lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}

std::string read_digit_run(const std::string &s, size_t &pos)
{
    std::string out;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
        out.push_back(s[pos++]);
    return out;
}

} // namespace

bool is_valid_meeting_id(const std::string &s)
{
    const std::string stripped = strip_whitespace(s);
    std::string digits;
    digits.reserve(stripped.size());
    for (char c : stripped) {
        if (c == ' ' || c == '-') continue;
        if (c < '0' || c > '9') return false;
        digits.push_back(c);
    }
    return !digits.empty();
}

ParsedJoin parse_join_input(const std::string &input)
{
    ParsedJoin out;
    const std::string s = strip_whitespace(input);
    if (s.empty()) return out;

    // No scheme separator → treat as raw meeting ID (allow spaces and dashes).
    if (s.find("://") == std::string::npos) {
        std::string digits;
        digits.reserve(s.size());
        for (char c : s) {
            if (c == ' ' || c == '-') continue;
            if (c < '0' || c > '9') return out;
            digits.push_back(c);
        }
        out.meeting_id = std::move(digits);
        return out;
    }

    // URL form: look for the meeting ID in known path markers first.
    static const char *kPathMarkers[] = {"/j/", "/wc/", "/s/", "/meeting/"};
    for (const char *marker : kPathMarkers) {
        size_t pos = s.find(marker);
        if (pos == std::string::npos) continue;
        pos += std::strlen(marker);
        std::string digits = read_digit_run(s, pos);
        if (!digits.empty()) {
            out.meeting_id = std::move(digits);
            break;
        }
    }

    // Walk the query string once for both the passcode and (as a fallback)
    // confno-style meeting IDs.
    const size_t qpos = s.find('?');
    if (qpos != std::string::npos) {
        size_t pos = qpos + 1;
        while (pos < s.size()) {
            const size_t amp = s.find('&', pos);
            const size_t end = (amp == std::string::npos) ? s.size() : amp;
            const size_t eq  = s.find('=', pos);
            if (eq != std::string::npos && eq < end) {
                const std::string key = lower_ascii(url_decode(s.substr(pos, eq - pos)));
                const std::string val = url_decode(s.substr(eq + 1, end - eq - 1));
                if (key == "pwd" || key == "password" || key == "passcode") {
                    out.passcode = val;
                } else if (out.meeting_id.empty() &&
                           (key == "confno" || key == "meeting_id" || key == "mn")) {
                    if (all_digits(val)) out.meeting_id = val;
                } else if (key == "obf" || key == "on_behalf_token" ||
                           key == "onbehalftoken") {
                    out.on_behalf_token = val;
                } else if (key == "zak" || key == "user_zak" || key == "userzak") {
                    out.user_zak = val;
                } else if (key == "app_privilege_token" ||
                           key == "appprivilegetoken" ||
                           key == "apptoken" || key == "app_token") {
                    out.app_privilege_token = val;
                }
            }
            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
    }

    return out;
}

} // namespace zoom_join_utils
