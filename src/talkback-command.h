#pragma once
// talkback-command.h -- pure classification of the seven talkback IPC
// commands (macOS talkback port, Task 3, 2026-09-05).
//
// main-macos.mm's reader loop dispatches every command by SUBSTRING match
// (`line.find(IPC_CMD_...)`) against the whole raw JSON line, the same
// convention the rest of that loop already uses -- and that loop's own
// comment on the unsubscribe/subscribe pair documents the STYLE of bug this
// file exists to make pinnable: a substring match whose ORDER silently
// misroutes a command, because a shorter command name is a prefix or
// substring of a longer one. Both Majors this feature has shipped
// (engine-talkback.cpp's own review history) lived in wiring shaped exactly
// like that -- decisions inlined where no host test could reach them.
// Extracting the match into this pure function is what makes it reachable
// by tests/engine-talkback-select-test.cpp (Windows-gated, since it links
// the real engine-talkback.cpp) and by tests/talkback-command-test.cpp (this
// repo's own platform-agnostic pin -- see that file for why a second test
// target exists for the identical assertions).
//
// Checked exhaustively (all 7*6 ordered pairs): none of these seven command
// literals is a substring of another, unlike "subscribe"/"unsubscribe" in
// engine-ipc.h, which genuinely do collide. So today, for these seven
// specifically, dispatch order does not change behavior. The order below is
// kept "more specific first" anyway -- longer names before their shorter
// relatives, mirroring the existing loop's convention -- so that an EIGHTH
// talkback command added later which DOES collide with one of these (e.g.
// something starting "talkback_start_") fails safe by construction rather
// than by luck of insertion order, and so this function stays a faithful
// extraction of what the reader loop would write inline rather than a
// rewrite. See task-3-report.md for the full divergence note: the mutation
// the brief describes (swapping the Start/Stop branches) does not kill any
// assertion here, precisely because the two strings do not collide -- a
// finding worth keeping, not a test that failed to do its job.
#include "engine-ipc.h"
#include <string>

enum class TalkbackCmd {
    None,
    Nominate,
    Probe,
    Audio,
    Close,
    Open,
    Stop,
    Start,
};

inline TalkbackCmd talkback_command_for(const std::string &line)
{
    if (line.find(IPC_CMD_TALKBACK_NOMINATE) != std::string::npos) return TalkbackCmd::Nominate;
    if (line.find(IPC_CMD_TALKBACK_PROBE) != std::string::npos) return TalkbackCmd::Probe;
    if (line.find(IPC_CMD_TALKBACK_AUDIO) != std::string::npos) return TalkbackCmd::Audio;
    if (line.find(IPC_CMD_TALKBACK_CLOSE) != std::string::npos) return TalkbackCmd::Close;
    if (line.find(IPC_CMD_TALKBACK_OPEN) != std::string::npos) return TalkbackCmd::Open;
    if (line.find(IPC_CMD_TALKBACK_STOP) != std::string::npos) return TalkbackCmd::Stop;
    if (line.find(IPC_CMD_TALKBACK_START) != std::string::npos) return TalkbackCmd::Start;
    return TalkbackCmd::None;
}
