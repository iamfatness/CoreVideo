#pragma once
//
// engine-json.h — minimal JSON field extraction / escaping, no external
// dependency. Originally private `static` helpers in main.cpp; extracted so
// other engine translation units (engine-talkback.cpp) can build E2P lines
// without a second, inevitably-divergent copy of a JSON escaper.
//
// windows.h must come before zoom_sdk.h: zoom_sdk_def.h uses HWND without
// including it itself.
#if defined(WIN32)
#include <windows.h>
#endif
#include <zoom_sdk.h>

#include <string>

inline std::string json_str(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string result;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\') {
            if (pos < json.size()) pos++; // skip escaped character
            continue;
        }
        if (c == '"') break;
        result += c;
    }
    return result;
}

inline std::string json_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

inline std::string zchar_to_utf8(const zchar_t *name)
{
    if (!name) return {};
#if defined(WIN32)
    int len = WideCharToMultiByte(CP_UTF8, 0, name, -1,
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    if (!out.empty()) {
        WideCharToMultiByte(CP_UTF8, 0, name, -1, &out[0], len,
                            nullptr, nullptr);
    }
    return out;
#else
    return name;
#endif
}
