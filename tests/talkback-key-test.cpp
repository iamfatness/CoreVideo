// tests/talkback-key-test.cpp
// When a talkback key stays open, and every way it must close.
//
// Talkback is the one feature where a bug is heard by people who are not in
// the control room. A key stuck open is a director's private remark going to
// talent -- or, if the source is also on a program bus, to the audience. So
// this pins the CLOSING direction exhaustively: every failure in the spec's
// table must produce Close, and only a genuinely healthy key stays open.
//
// The design is a dead-man switch: audio arriving IS the liveness signal, so
// no code path has to notice a failure for the key to close. These tests are
// what keep that property true as the surrounding code changes.
#include "talkback-key.h"

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static TalkbackKeyState healthy(uint64_t now, TalkbackKeyMode mode)
{
    TalkbackKeyState s{};
    s.open            = true;
    s.mode            = mode;
    s.last_audio_ms   = now;
    s.last_renewal_ms = now;
    s.needs_renewal   = true;
    return s;
}

int main()
{
    constexpr uint64_t T = 100000;

    // ── A healthy key stays open, in both modes ────────────────────────────
    check(talkback_key_evaluate(healthy(T, TalkbackKeyMode::PushToTalk), T, true, true) ==
              TalkbackKeyAction::None,
          "a healthy push-to-talk key was closed");
    check(talkback_key_evaluate(healthy(T, TalkbackKeyMode::Latch), T, true, true) ==
              TalkbackKeyAction::None,
          "a healthy latched key was closed");

    // ── The audio gap: buffers stopping closes the key ─────────────────────
    // This is the dead-man switch. OBS delivers buffers continuously while a
    // source is active -- including silence -- so a gap means the tap, the
    // plugin, or the pipe is gone.
    {
        TalkbackKeyState s = healthy(T, TalkbackKeyMode::Latch);
        check(talkback_key_evaluate(s, T + kTalkbackAudioGapMs - 1, true, true) ==
                  TalkbackKeyAction::None,
              "the key closed while still inside the audio gap window");
        check(talkback_key_evaluate(s, T + kTalkbackAudioGapMs + 1, true, true) ==
                  TalkbackKeyAction::Close,
              "the key stayed open past the audio gap -- the dead-man switch failed");
    }

    // ── A lost button release closes via the renewal, not the gap ──────────
    // Buffers keep flowing when the release is lost but the socket is healthy,
    // so the gap can never fire. The controller must keep asserting the key.
    {
        TalkbackKeyState s = healthy(T, TalkbackKeyMode::PushToTalk);
        s.last_audio_ms = T + 10000;            // audio is fine
        s.last_renewal_ms = T;                  // renewals stopped
        const uint64_t missed = kTalkbackRenewalMs * kTalkbackRenewalsMissed;
        check(talkback_key_evaluate(s, T + missed - 1, true, true) ==
                  TalkbackKeyAction::None,
              "the key closed before the renewal grace elapsed");
        check(talkback_key_evaluate(s, T + missed + 1, true, true) ==
                  TalkbackKeyAction::Close,
              "a lost release left the key open -- the director is live and "
              "does not know it");
    }

    // ── A key that does not require renewal is not closed by renewal age ───
    // The OBS hotkey's release is in-process and reliable, so it opts out.
    {
        TalkbackKeyState s = healthy(T, TalkbackKeyMode::PushToTalk);
        s.needs_renewal   = false;
        s.last_renewal_ms = T;           // renewal is stale -- must not matter
        s.last_audio_ms   = T + 60000;   // audio stays fresh at eval time
        check(talkback_key_evaluate(s, T + 60000, true, true) ==
                  TalkbackKeyAction::None,
              "a key that does not require renewal was closed by renewal age");
    }

    // ── Every other failure in the spec's table closes it ──────────────────
    {
        TalkbackKeyState s = healthy(T, TalkbackKeyMode::Latch);
        check(talkback_key_evaluate(s, T, false, true) == TalkbackKeyAction::Close,
              "the engine dying did not close the key");
        check(talkback_key_evaluate(s, T, true, false) == TalkbackKeyAction::Close,
              "leaving the meeting did not close the key");
        check(talkback_key_evaluate(s, T, false, false) == TalkbackKeyAction::Close,
              "engine death plus meeting loss did not close the key");
    }

    // ── A latch does NOT survive a reconnect ──────────────────────────────
    // Explicit spec requirement. Restoring a latch on reconnect is the risky
    // moment, and the operator chose never to take it.
    {
        TalkbackKeyState s = healthy(T, TalkbackKeyMode::Latch);
        s.last_audio_ms = T;
        check(talkback_key_evaluate(s, T + 5000, false, true) ==
                  TalkbackKeyAction::Close,
              "a latch survived the engine going away");
    }

    // ── A closed key is never spuriously reopened ─────────────────────────
    {
        TalkbackKeyState s{};
        s.open = false;
        check(talkback_key_evaluate(s, T, true, true) == TalkbackKeyAction::None,
              "a closed key was reopened by evaluate()");
        check(talkback_key_evaluate(s, T + 10 * T, false, false) ==
                  TalkbackKeyAction::None,
              "a closed key produced Close, which would double-close");
    }

    // ── A backwards clock must not close a healthy key ────────────────────
    // The same guard join-watchdog.h carries: an underflowed subtraction
    // becomes an enormous elapsed time.
    {
        TalkbackKeyState s = healthy(T + 5000, TalkbackKeyMode::Latch);
        check(talkback_key_evaluate(s, T, true, true) == TalkbackKeyAction::None,
              "a backwards clock underflowed and closed a healthy key");
    }

    if (failures == 0)
        std::cout << "talkback-key: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
