#include "zoom-control-parse.h"

#include <iostream>
#include <limits>

static int g_failures = 0;

static void expect(const char *name, bool cond)
{
    if (!cond) {
        std::cerr << name << ": failed\n";
        ++g_failures;
    }
}

// ── ct_equal ─────────────────────────────────────────────────────────────

static void test_ct_equal()
{
    expect("ct_equal: identical strings", ct_equal("secret-token", "secret-token"));
    expect("ct_equal: both empty", ct_equal("", ""));
    expect("ct_equal: different content same length", !ct_equal("aaaaaaaa", "aaaaaaab"));
    expect("ct_equal: different lengths", !ct_equal("short", "much-longer-token"));
    expect("ct_equal: empty vs non-empty", !ct_equal("", "token"));
    expect("ct_equal: case sensitive", !ct_equal("Token", "token"));
    expect("ct_equal: one-char difference at the end",
           !ct_equal("abcdefgh", "abcdefgi"));
}

// ── json_to_uint32 ───────────────────────────────────────────────────────

static void test_json_to_uint32()
{
    uint32_t out = 0;

    {
        QJsonObject obj{{"n", 42}};
        expect("json_to_uint32: valid integer", json_to_uint32(obj, "n", out));
        expect("json_to_uint32: valid integer value", out == 42);
    }
    {
        QJsonObject obj{{"n", 0}};
        expect("json_to_uint32: zero is valid", json_to_uint32(obj, "n", out));
        expect("json_to_uint32: zero value", out == 0);
    }
    {
        QJsonObject obj{{"n", -1}};
        expect("json_to_uint32: negative is rejected", !json_to_uint32(obj, "n", out));
    }
    {
        QJsonObject obj{{"n", 1.5}};
        expect("json_to_uint32: fractional is rejected", !json_to_uint32(obj, "n", out));
    }
    {
        QJsonObject obj{{"n", "42"}};
        expect("json_to_uint32: string value is rejected", !json_to_uint32(obj, "n", out));
    }
    {
        QJsonObject obj;
        expect("json_to_uint32: missing key is rejected", !json_to_uint32(obj, "n", out));
    }
    {
        const double too_big =
            static_cast<double>(std::numeric_limits<uint32_t>::max()) + 1024.0;
        QJsonObject obj{{"n", too_big}};
        expect("json_to_uint32: above uint32 max is rejected",
               !json_to_uint32(obj, "n", out));
    }
    {
        QJsonObject obj{{"n", static_cast<double>(std::numeric_limits<uint32_t>::max())}};
        expect("json_to_uint32: exactly uint32 max is accepted",
               json_to_uint32(obj, "n", out));
        expect("json_to_uint32: uint32 max value",
               out == std::numeric_limits<uint32_t>::max());
    }
    {
        QJsonObject obj{{"n", QJsonValue(QJsonValue::Null)}};
        expect("json_to_uint32: null value is rejected", !json_to_uint32(obj, "n", out));
    }
}

// ── video_resolution_from_json ──────────────────────────────────────────

static void test_video_resolution_from_json()
{
    expect("video_resolution: missing defaults to 720p",
           video_resolution_from_json(QJsonObject{}) == VideoResolution::P720);
    expect("video_resolution: '360p'",
           video_resolution_from_json(QJsonObject{{"video_resolution", "360p"}}) ==
               VideoResolution::P360);
    expect("video_resolution: '360'",
           video_resolution_from_json(QJsonObject{{"video_resolution", "360"}}) ==
               VideoResolution::P360);
    expect("video_resolution: '1080p'",
           video_resolution_from_json(QJsonObject{{"video_resolution", "1080p"}}) ==
               VideoResolution::P1080);
    expect("video_resolution: '1080'",
           video_resolution_from_json(QJsonObject{{"video_resolution", "1080"}}) ==
               VideoResolution::P1080);
    expect("video_resolution: '720p' explicit",
           video_resolution_from_json(QJsonObject{{"video_resolution", "720p"}}) ==
               VideoResolution::P720);
    expect("video_resolution: unrecognized value defaults to 720p",
           video_resolution_from_json(QJsonObject{{"video_resolution", "bogus"}}) ==
               VideoResolution::P720);
}

// ── meeting_state_to_string ──────────────────────────────────────────────

static void test_meeting_state_to_string()
{
    expect("meeting_state: idle", meeting_state_to_string(MeetingState::Idle) == "idle");
    expect("meeting_state: joining",
           meeting_state_to_string(MeetingState::Joining) == "joining");
    expect("meeting_state: in_meeting",
           meeting_state_to_string(MeetingState::InMeeting) == "in_meeting");
    expect("meeting_state: leaving",
           meeting_state_to_string(MeetingState::Leaving) == "leaving");
    expect("meeting_state: recovering",
           meeting_state_to_string(MeetingState::Recovering) == "recovering");
    expect("meeting_state: failed", meeting_state_to_string(MeetingState::Failed) == "failed");
    expect("meeting_state: out-of-range value falls back to unknown",
           meeting_state_to_string(static_cast<MeetingState>(99)) == "unknown");
}

// ── known_control_commands / is_known_control_command ────────────────────

static void test_known_commands()
{
    expect("known commands: help", is_known_control_command("help"));
    expect("known commands: status", is_known_control_command("status"));
    expect("known commands: join", is_known_control_command("join"));
    expect("known commands: oauth_callback", is_known_control_command("oauth_callback"));
    expect("known commands: recovery_cancel", is_known_control_command("recovery_cancel"));
    expect("known commands: list_audio_sources",
           is_known_control_command("list_audio_sources"));
    expect("known commands: talkback_probe",
           is_known_control_command("talkback_probe"));
    expect("unknown command is rejected", !is_known_control_command("totally_bogus_cmd"));
    expect("empty command is rejected", !is_known_control_command(""));
    expect("known commands are case sensitive", !is_known_control_command("HELP"));
    // Intentional regression guard -- known_control_commands() is a fixed
    // literal list, so cppcheck correctly proves the count is 21 today.
    // That's the point of this assertion: it forces this test to be updated
    // whenever a command is added or removed.
    expect("known command list has no duplicates and no gaps",
           // cppcheck-suppress knownConditionTrueFalse
           known_control_commands().size() == 21);
}

// ── parse_control_request: JSON validity + auth ──────────────────────────

static void test_parse_control_request()
{
    // Valid JSON, no token configured: always authorized.
    {
        const auto r = parse_control_request(R"({"cmd":"status"})", "");
        expect("valid json, no token: no error", r.error == ControlRequestError::None);
        expect("valid json, no token: cmd extracted", r.cmd == "status");
    }

    // Invalid JSON syntax.
    {
        const auto r = parse_control_request(R"({"cmd":)", "");
        expect("malformed json is rejected", r.error == ControlRequestError::InvalidJson);
    }

    // Empty line.
    {
        const auto r = parse_control_request(QByteArray(""), "");
        expect("empty line is rejected", r.error == ControlRequestError::InvalidJson);
    }

    // Valid JSON but not an object (array).
    {
        const auto r = parse_control_request(R"(["status"])", "");
        expect("json array (not object) is rejected",
               r.error == ControlRequestError::InvalidJson);
    }

    // Valid JSON but not an object (bare number).
    {
        const auto r = parse_control_request(R"(42)", "");
        expect("bare json number is rejected", r.error == ControlRequestError::InvalidJson);
    }

    // Object with trailing garbage is still malformed per strict JSON.
    {
        const auto r = parse_control_request(R"({"cmd":"status"} trailing)", "");
        expect("trailing garbage after object is rejected",
               r.error == ControlRequestError::InvalidJson);
    }

    // Token configured, correct token supplied.
    {
        const auto r = parse_control_request(
            R"({"cmd":"status","token":"s3cret"})", "s3cret");
        expect("correct token is authorized", r.error == ControlRequestError::None);
    }

    // Token configured, wrong token supplied.
    {
        const auto r = parse_control_request(
            R"({"cmd":"status","token":"wrong"})", "s3cret");
        expect("wrong token is unauthorized", r.error == ControlRequestError::Unauthorized);
    }

    // Token configured, token field missing entirely.
    {
        const auto r = parse_control_request(R"({"cmd":"status"})", "s3cret");
        expect("missing token field is unauthorized",
               r.error == ControlRequestError::Unauthorized);
    }

    // Token configured, but request is oauth_callback: bypasses auth even
    // with a wrong/missing token, mirroring handle_line()'s special case.
    {
        const auto r = parse_control_request(
            R"({"cmd":"oauth_callback","url":"https://example.test"})", "s3cret");
        expect("oauth_callback bypasses token check", r.error == ControlRequestError::None);
        expect("oauth_callback cmd is preserved", r.cmd == "oauth_callback");
    }
    {
        const auto r = parse_control_request(
            R"({"cmd":"oauth_callback","token":"wrong"})", "s3cret");
        expect("oauth_callback bypasses token check even with a wrong token",
               r.error == ControlRequestError::None);
    }

    // No token configured: even a garbage "token" field is ignored.
    {
        const auto r = parse_control_request(
            R"({"cmd":"status","token":"anything"})", "");
        expect("no server token configured: request always authorized",
               r.error == ControlRequestError::None);
    }

    // Missing "cmd" field: parses fine, cmd is empty (dispatch would treat
    // this as an unknown command).
    {
        const auto r = parse_control_request(R"({"foo":"bar"})", "");
        expect("missing cmd field still parses", r.error == ControlRequestError::None);
        expect("missing cmd field yields empty cmd", r.cmd.isEmpty());
    }

    // Boundary: token equal length, differs only in the last byte.
    {
        const auto r = parse_control_request(
            R"({"cmd":"status","token":"s3cretX"})", "s3crety");
        expect("same-length differing token is unauthorized",
               r.error == ControlRequestError::Unauthorized);
    }
}

int main()
{
    test_ct_equal();
    test_json_to_uint32();
    test_video_resolution_from_json();
    test_meeting_state_to_string();
    test_known_commands();
    test_parse_control_request();

    return g_failures == 0 ? 0 : 1;
}
