// tests/talkback-command-test.cpp
//
// Pins src/talkback-command.h's talkback_command_for(), the pure extraction
// of main-macos.mm's talkback command dispatch (macOS talkback port, Task 3,
// 2026-09-05). This is a SECOND home for the same assertions the task-3
// brief asks to add to tests/engine-talkback-select-test.cpp -- deliberately,
// not redundantly: that target is gated at the CMakeLists level on
// `WIN32 AND <real Zoom SDK headers present>` (it links the real
// engine/src/engine-talkback.cpp, which needs the SDK's C++ interfaces), so
// it does not exist in a macOS build tree at all -- confirmed by `ctest -N`
// registering no such test here. talkback_command_for() itself has no SDK,
// Qt, or OBS dependency whatsoever (same bar every other pure-header test in
// this suite clears), so it does not need to share that gate, and a
// platform-agnostic test is what lets THIS task's own mutation-proof step
// actually run on the machine doing the port. The assertions below are kept
// byte-for-byte identical to the ones added to
// tests/engine-talkback-select-test.cpp so the two never drift.
#include "talkback-command.h"

#include <iostream>
#include <string>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

int main()
{
    // "talkback_start" contains no other command as a substring, but
    // "talkback_stop" and "talkback_start" share the "talkback_st" prefix and
    // both contain "talkback_". Dispatch must land each on its own handler.
    check(talkback_command_for(R"({"cmd":"talkback_stop"})") == TalkbackCmd::Stop,
          "talkback_stop was routed somewhere else");
    check(talkback_command_for(R"({"cmd":"talkback_start"})") == TalkbackCmd::Start,
          "talkback_start was routed somewhere else");
    check(talkback_command_for(R"({"cmd":"talkback_close"})") == TalkbackCmd::Close,
          "talkback_close was routed somewhere else");

    // The other four commands and the None case, so an implementation that
    // special-cases only the three pairs named above (and gets the rest
    // wrong, or gets them right by accident) cannot pass silently.
    check(talkback_command_for(R"({"cmd":"talkback_probe","participant":"Sarah Muller"})") ==
              TalkbackCmd::Probe,
          "talkback_probe was routed somewhere else");
    check(talkback_command_for(R"({"cmd":"talkback_open","region":"r","rate":48000,"channels":1})") ==
              TalkbackCmd::Open,
          "talkback_open was routed somewhere else");
    check(talkback_command_for(R"({"cmd":"talkback_audio"})") == TalkbackCmd::Audio,
          "talkback_audio was routed somewhere else");
    check(talkback_command_for(R"({"cmd":"talkback_nominate","nominees":[]})") ==
              TalkbackCmd::Nominate,
          "talkback_nominate was routed somewhere else");
    check(talkback_command_for(R"({"cmd":"join"})") == TalkbackCmd::None,
          "an unrelated command was misrouted to a talkback handler");
    check(talkback_command_for("") == TalkbackCmd::None,
          "an empty line was misrouted to a talkback handler");

    if (failures == 0)
        std::cout << "talkback-command: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
