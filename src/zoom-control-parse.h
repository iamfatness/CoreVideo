#pragma once

// Pure TCP control-server request parsing, factored out of
// zoom-control-server.cpp so it can be unit tested on the host without OBS,
// Qt networking, or the Zoom SDK. No behavior change from the original code
// in ZoomControlServer::handle_line() -- this header is a verbatim move of
// the JSON-parse / token-auth front end plus a handful of small pure
// helpers, marked `inline` so it can be included from multiple translation
// units (the plugin .cpp and the standalone test executable).

#include "zoom-types.h"
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

// Timing-safe string equality — prevents token leakage via timing side-channel.
// noinline prevents the compiler from inlining this and then optimising away
// the constant-time loop when the result is observable from context.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
inline bool ct_equal(const std::string &a, const std::string &b)
{
    volatile size_t diff = a.size() ^ b.size();
    const size_t max_len = std::max(a.size(), b.size());
    for (size_t i = 0; i < max_len; ++i) {
        const uint8_t ca = i < a.size() ? static_cast<uint8_t>(a[i]) : 0;
        const uint8_t cb = i < b.size() ? static_cast<uint8_t>(b[i]) : 0;
        diff |= static_cast<size_t>(ca ^ cb);
    }
    return diff == 0;
}

// Extracts obj[key] as a uint32_t. Rejects non-numeric values, negative
// values, non-finite values (NaN/Infinity), values above UINT32_MAX, and
// non-integral values (e.g. 1.5).
inline bool json_to_uint32(const QJsonObject &obj, const char *key, uint32_t &out)
{
    const QJsonValue value = obj.value(key);
    if (!value.isDouble()) return false;

    const double raw = value.toDouble(-1);
    if (!std::isfinite(raw) || raw < 0 ||
        raw > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
        std::floor(raw) != raw) {
        return false;
    }

    out = static_cast<uint32_t>(raw);
    return true;
}

// Maps the "video_resolution" field of a JSON request to VideoResolution.
// Unrecognized or missing values default to 720p.
inline VideoResolution video_resolution_from_json(const QJsonObject &req)
{
    const QString value = req.value("video_resolution").toString("720p");
    if (value == "360p" || value == "360") return VideoResolution::P360;
    if (value == "1080p" || value == "1080") return VideoResolution::P1080;
    return VideoResolution::P720;
}

inline QString meeting_state_to_string(MeetingState state)
{
    switch (state) {
    case MeetingState::Idle:       return "idle";
    case MeetingState::Joining:    return "joining";
    case MeetingState::InMeeting:  return "in_meeting";
    case MeetingState::Leaving:    return "leaving";
    case MeetingState::Recovering: return "recovering";
    case MeetingState::Failed:     return "failed";
    }
    return "unknown";
}

// The full set of commands dispatched by ZoomControlServer::handle_line().
// Kept in sync manually with the "help" response and the if-chain in
// handle_line(); used here only to give the "unknown command" acceptance
// criterion something pure to test against.
inline const std::array<const char *, 20> &known_control_commands()
{
    static const std::array<const char *, 20> kCommands = {
        "help", "status", "list_participants", "list_outputs",
        "list_audio_sources",
        "assign_output", "assign_output_ex",
        "recover_stale_outputs", "upgrade_low_quality_outputs",
        "speaker_director_status", "speaker_director_configure",
        "speaker_director_take", "speaker_director_release",
        "join", "leave", "start_engine", "stop_engine",
        "oauth_callback", "subscribe_events", "recovery_cancel",
    };
    return kCommands;
}

inline bool is_known_control_command(const QString &cmd)
{
    for (const char *known : known_control_commands()) {
        if (cmd == QString::fromUtf8(known)) return true;
    }
    return false;
}

enum class ControlRequestError {
    None,
    InvalidJson,
    Unauthorized,
};

struct ParsedControlRequest {
    ControlRequestError error = ControlRequestError::None;
    QJsonObject request; // valid only when error == None
    QString cmd;         // "" when request has no "cmd" field
};

// Parses one control-server line into JSON and validates the auth token,
// mirroring the front of ZoomControlServer::handle_line() before command
// dispatch. `token` is the server's configured auth token; an empty token
// means auth is disabled. "oauth_callback" is always exempt from the token
// check (it is itself part of establishing auth).
inline ParsedControlRequest parse_control_request(const QByteArray &line,
                                                   const std::string &token)
{
    ParsedControlRequest result;

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = ControlRequestError::InvalidJson;
        return result;
    }

    result.request = doc.object();
    result.cmd = result.request.value("cmd").toString();

    if (!token.empty() && result.cmd != "oauth_callback") {
        const std::string provided =
            result.request.value("token").toString().toStdString();
        if (!ct_equal(provided, token)) {
            result.error = ControlRequestError::Unauthorized;
            return result;
        }
    }

    return result;
}
