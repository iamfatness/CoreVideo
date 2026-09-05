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
// macOS talkback port, Task 2b (2026-09-05): zoom_sdk.h is the Windows/Linux
// C++ SDK header and does not exist on macOS (pure Objective-C SDK, no
// zoom_sdk.h at all) -- gated here, the ONE place this file needs it, rather
// than only at zchar_to_utf8()'s own declaration below, because the
// #include itself is what fails first ("'zoom_sdk.h' file not found"),
// before the compiler ever reaches a type it defines. This was previously
// unconditional because every past includer of this header (main.cpp,
// engine-talkback.cpp, engine-talkback-sdk-win.h) was Windows/Linux-only;
// engine-talkback.cpp joining the macOS ZoomObsEngine target this task is
// the first thing that includes this header on Apple at all. json_str()/
// json_escape() below have no Zoom dependency and are unaffected either way.
#if !defined(__APPLE__)
#include <zoom_sdk.h>
#endif

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

// macOS talkback port, Task 2b: not declared at all on Apple -- zchar_t (the
// type this function's own signature needs) comes from the same zoom_sdk.h
// gated out above, and nothing on macOS calls this function anyway
// (main-macos.mm has its own to_utf8(NSString*); the seam's macOS adapters --
// TalkbackMacSdk, TalkbackMacHost -- convert NSString straight to std::string
// with no zchar_t in sight, by design).
#if !defined(__APPLE__)
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
#endif
