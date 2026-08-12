#pragma once
//
// engine-command.h — which branch a plugin→engine command line routes to,
// as pure logic (unit-tested by tests/engine-command-test.cpp).
//
// This exists because the engine's read loop used to identify commands by
// testing each token as a *substring of the whole JSON line*, in source order:
//
//     if      (line.find(IPC_CMD_SUBSCRIBE)   != npos) { ... }
//     else if (line.find(IPC_CMD_UNSUBSCRIBE) != npos) { ... }
//
// "unsubscribe" contains "subscribe", and the subscribe test came first, so
// every unsubscribe was routed into the *subscribe* branch. There it parsed a
// missing "participant_id" as 0 and re-registered the source as an audio
// target with participant 0, while share_engine.unsubscribe() and
// EngineAudio::remove() were never reached at all. A substring dispatch where
// one command name contains another is a bug generator; match the command
// exactly instead, so the order of the tests carries no meaning.
//
// The same substring rule also meant any command whose *payload* happened to
// contain a token — a display name of "Quitman", a JWT with "init" in its
// base64 — could be routed to the wrong branch. Reading the declared "cmd"
// field closes that too.
//
// Free of Qt / OBS / Zoom SDK dependencies so the routing table can be pinned
// by a test with no engine and no meeting.
//
#include "engine-ipc.h"  // IPC_CMD_* tokens

#include <string>

enum class IpcCommand {
    Unknown,
    Quit,
    Init,
    Join,
    Leave,
    StartMedia,
    StopMedia,
    SubscribeAudio,
    Subscribe,
    Unsubscribe,
};

// The value of the first top-level "cmd" string field, or "" when the line has
// none.
//
// A backslash consumes the character after it, so an escaped quote cannot end
// the value early. Unlike the engine's json_str(), the escaped character is
// KEPT rather than dropped: dropping it lets a forged value collapse into a
// real command name (json_str turns `unsub\"scribe` into `unsubscribe`).
// Keeping it yields `unsub"scribe`, which matches nothing. No command the
// plugin emits contains an escape, so this only ever differs on malformed or
// hostile input.
inline std::string ipc_command_name(const std::string &line)
{
    static const char kNeedle[] = "\"cmd\":\"";
    const std::size_t needle_len = sizeof(kNeedle) - 1;
    const std::size_t pos = line.find(kNeedle);
    if (pos == std::string::npos) return {};

    std::string result;
    for (std::size_t i = pos + needle_len; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\') {
            if (++i < line.size()) result += line[i];
            continue;
        }
        if (c == '"') break;
        result += c;
    }
    return result;
}

// Exact match on the declared command. Every command the plugin emits writes
// "cmd" as its first field (see ZoomEngineClient in src/zoom-engine-client.cpp),
// so an unrecognised line is genuinely unrecognised rather than merely
// unmatched, and is ignored by the caller.
inline IpcCommand ipc_command_of(const std::string &line)
{
    const std::string cmd = ipc_command_name(line);
    if (cmd == IPC_CMD_QUIT)            return IpcCommand::Quit;
    if (cmd == IPC_CMD_INIT)            return IpcCommand::Init;
    if (cmd == IPC_CMD_JOIN)            return IpcCommand::Join;
    if (cmd == IPC_CMD_LEAVE)           return IpcCommand::Leave;
    if (cmd == IPC_CMD_START_MEDIA)     return IpcCommand::StartMedia;
    if (cmd == IPC_CMD_STOP_MEDIA)      return IpcCommand::StopMedia;
    if (cmd == IPC_CMD_SUBSCRIBE_AUDIO) return IpcCommand::SubscribeAudio;
    if (cmd == IPC_CMD_SUBSCRIBE)       return IpcCommand::Subscribe;
    if (cmd == IPC_CMD_UNSUBSCRIBE)     return IpcCommand::Unsubscribe;
    return IpcCommand::Unknown;
}

// True when a subscribe carries the optional video-only flag, which suppresses
// the engine-side mixed-audio target for that source.
//
// Absent (the default) means "register an audio target", so every existing
// caller keeps its behavior. The CoreVideo Tiles source sets it: a tile wall is
// video-only, and without this each of its up-to-nine slots registered a target
// that received mixed meeting audio — one SHM write plus one {"cmd":"audio"}
// IPC line per audio buffer per tile, all of which the plugin parsed and threw
// away because the tiles source registers no audio callback.
//
// Deliberately NOT expressed as isolate_audio: that would set claimed_by_isolate
// for the participant in EngineAudio::onOneWayAudioRawDataReceived() and
// suppress an existing audience-audio source for the same person.
inline bool ipc_subscribe_is_video_only(const std::string &line)
{
    return line.find("\"video_only\":true") != std::string::npos;
}
