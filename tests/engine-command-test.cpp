// Unit tests for the plugin->engine command routing in engine-command.h.
//
// The regression these exist for: the engine's read loop identified commands by
// substring, testing "subscribe" before "unsubscribe". Because "unsubscribe"
// contains "subscribe", every unsubscribe was handled by the subscribe branch —
// it re-registered a mixed-audio target with participant_id 0 and never reached
// share_engine.unsubscribe() or EngineAudio::remove().
//
// Every line below is a verbatim command as ZoomEngineClient emits it
// (src/zoom-engine-client.cpp), so this file doubles as the wire-format record.

#include "engine-command.h"

#include <iostream>
#include <string>

static int g_failures = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

static void routes(const std::string &line, IpcCommand expected, const char *what)
{
    const IpcCommand actual = ipc_command_of(line);
    if (actual != expected) {
        std::cerr << "FAIL: " << what << " -- routed to "
                  << static_cast<int>(actual) << ", expected "
                  << static_cast<int>(expected) << "\n  line: " << line << "\n";
        ++g_failures;
    }
}

int main()
{
    // ── The bug this file exists for ────────────────────────────────────────
    const std::string unsubscribe =
        R"({"cmd":"unsubscribe","source_uuid":"tile_1_0"})";
    routes(unsubscribe, IpcCommand::Unsubscribe, "unsubscribe routes to Unsubscribe");
    check(ipc_command_of(unsubscribe) != IpcCommand::Subscribe,
          "unsubscribe must NOT route to the subscribe branch");
    check(ipc_command_of(unsubscribe) != IpcCommand::SubscribeAudio,
          "unsubscribe must NOT route to the subscribe_audio branch");

    // ── Every command the plugin emits, verbatim ────────────────────────────
    routes(R"({"cmd":"quit"})", IpcCommand::Quit, "quit");
    routes(R"({"cmd":"init","jwt":"header.payload.sig"})", IpcCommand::Init,
           "init with jwt");
    routes(R"({"cmd":"init","public_app_key":"abc123"})", IpcCommand::Init,
           "init with public app key");
    routes(R"({"cmd":"join","meeting_id":"123","passcode":"","display_name":"OBS"})",
           IpcCommand::Join, "join");
    routes(R"({"cmd":"leave"})", IpcCommand::Leave, "leave");
    routes(R"({"cmd":"start_media"})", IpcCommand::StartMedia, "start_media");
    routes(R"({"cmd":"stop_media"})", IpcCommand::StopMedia, "stop_media");

    // subscribe_audio must not be swallowed by the shorter "subscribe", and
    // subscribe must not be swallowed by the longer "subscribe_audio".
    routes(R"({"cmd":"subscribe_audio","source_uuid":"a1","participant_id":7,"isolate_audio":true,"audience_audio":false})",
           IpcCommand::SubscribeAudio, "subscribe_audio");
    routes(R"({"cmd":"subscribe","source_uuid":"v1","participant_id":7,"resolution":1,"isolate_audio":false,"audience_audio":false})",
           IpcCommand::Subscribe, "subscribe (participant video)");
    routes(R"({"cmd":"subscribe","source_uuid":"s1","mode":"screenshare"})",
           IpcCommand::Subscribe, "subscribe (screenshare mode)");
    routes(R"({"cmd":"subscribe","source_uuid":"p1","mode":"spotlight","slot":2})",
           IpcCommand::Subscribe, "subscribe (spotlight mode)");

    // ── A payload that merely contains a token routes by its declared cmd ───
    // The old substring dispatch sent these to the wrong branch entirely.
    routes(R"({"cmd":"join","meeting_id":"1","display_name":"Quitman"})",
           IpcCommand::Join, "display name containing 'quit' still joins");
    routes(R"({"cmd":"join","meeting_id":"1","display_name":"Initech"})",
           IpcCommand::Join, "display name containing 'init' still joins");
    routes(R"({"cmd":"unsubscribe","source_uuid":"subscribe_audio_1"})",
           IpcCommand::Unsubscribe, "uuid containing a token still unsubscribes");

    // ── Unrecognised / malformed lines are ignored, not misrouted ───────────
    routes(R"({"cmd":"ping"})", IpcCommand::Unknown, "unknown command");
    routes("", IpcCommand::Unknown, "empty line");
    routes("{}", IpcCommand::Unknown, "no cmd field");
    routes(R"({"source_uuid":"unsubscribe"})", IpcCommand::Unknown,
           "a value naming a command is not a command");

    // ── Name extraction ─────────────────────────────────────────────────────
    check(ipc_command_name(R"({"cmd":"subscribe","x":1})") == "subscribe",
          "ipc_command_name reads the cmd value");
    check(ipc_command_name(R"({"cmd":"","x":1})").empty(),
          "empty cmd value yields empty name");
    // An escaped quote inside the value must not terminate it early, and the
    // escaped character must be KEPT. Dropping it (as the engine's json_str
    // does) would let a forged value collapse into a real command name.
    check(ipc_command_name(R"({"cmd":"a\"b"})") == "a\"b",
          "escaped quote is kept, not treated as the terminator");
    check(ipc_command_of(R"({"cmd":"unsub\"scribe"})") == IpcCommand::Unknown,
          "an escaped value must not collapse into a real command name");

    // ── The optional video-only subscribe flag ──────────────────────────────
    check(!ipc_subscribe_is_video_only(
              R"({"cmd":"subscribe","source_uuid":"v1","participant_id":7,"resolution":1,"isolate_audio":false,"audience_audio":false})"),
          "video_only defaults to false when the field is absent");
    check(!ipc_subscribe_is_video_only(
              R"({"cmd":"subscribe","source_uuid":"v1","video_only":false})"),
          "video_only:false is not video-only");
    check(ipc_subscribe_is_video_only(
              R"({"cmd":"subscribe","source_uuid":"tile_1_0","participant_id":7,"resolution":1,"isolate_audio":false,"audience_audio":false,"video_only":true})"),
          "video_only:true is detected on a tiles subscribe");

    // --- Talkback probe routes exactly, and does not collide ---
    routes(R"({"cmd":"talkback_probe","participant":"Sarah Muller"})",
           IpcCommand::TalkbackProbe,
           "talkback_probe did not route to IpcCommand::TalkbackProbe");
    // A display name or payload containing the token must not route.
    routes(R"({"cmd":"join","display_name":"talkback_probe"})",
           IpcCommand::Join,
           "a payload containing 'talkback_probe' hijacked the join branch");
    // Guard the substring family the same way the existing commands are guarded.
    routes(R"({"cmd":"talkback_probe_extra"})",
           IpcCommand::Unknown,
           "a longer command starting with talkback_probe matched it");

    // ── Talkback audio-path commands route exactly ──────────────────────────
    check(ipc_command_of(R"({"cmd":"talkback_open","region":"X","rate":48000})") ==
              IpcCommand::TalkbackOpen,
          "talkback_open did not route to IpcCommand::TalkbackOpen");
    check(ipc_command_of(R"({"cmd":"talkback_audio"})") == IpcCommand::TalkbackAudio,
          "talkback_audio did not route to IpcCommand::TalkbackAudio");
    check(ipc_command_of(R"({"cmd":"talkback_close"})") == IpcCommand::TalkbackClose,
          "talkback_close did not route to IpcCommand::TalkbackClose");
    // The family shares a prefix with talkback_probe; exact match must keep
    // them apart, the way it keeps unsubscribe out of the subscribe branch.
    check(ipc_command_of(R"({"cmd":"talkback_probe"})") == IpcCommand::TalkbackProbe,
          "talkback_probe was hijacked by a sibling talkback_* command");
    check(ipc_command_of(R"({"cmd":"talkback_open_extra"})") == IpcCommand::Unknown,
          "a longer command starting with talkback_open matched it");

    // ── Talkback session commands route exactly ─────────────────────────────
    check(ipc_command_of(R"({"cmd":"talkback_start","participant":"Sarah Muller"})") ==
              IpcCommand::TalkbackStart,
          "talkback_start did not route to IpcCommand::TalkbackStart");
    check(ipc_command_of(R"({"cmd":"talkback_stop"})") == IpcCommand::TalkbackStop,
          "talkback_stop did not route to IpcCommand::TalkbackStop");
    // The talkback_* family now has seven members sharing a prefix. Exact
    // match must keep every one of them apart.
    check(ipc_command_of(R"({"cmd":"talkback_start_extra"})") == IpcCommand::Unknown,
          "a longer command starting with talkback_start matched it");
    check(ipc_command_of(R"({"cmd":"talkback_stop_extra"})") == IpcCommand::Unknown,
          "a longer command starting with talkback_stop matched it");

    // ── Talkback nominate (pre-provisioning) routes exactly ─────────────────
    check(ipc_command_of(
              R"({"cmd":"talkback_nominate","nominees":["Sarah Muller","Luis Ortiz"]})") ==
              IpcCommand::TalkbackNominate,
          "talkback_nominate did not route to IpcCommand::TalkbackNominate");
    // Shares the talkback_ prefix with all six siblings above; exact match
    // must keep it apart the same way it keeps every one of them apart.
    check(ipc_command_of(R"({"cmd":"talkback_nominate_extra"})") == IpcCommand::Unknown,
          "a longer command starting with talkback_nominate matched it");

    if (g_failures > 0) {
        std::cerr << "engine-command: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "engine-command: all tests passed\n";
    return 0;
}
