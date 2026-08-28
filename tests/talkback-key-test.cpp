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
//
// talkback_key_evaluate short-circuits: the audio-gap check runs before the
// renewal check, and either can return Close for its own reason. When a
// check sits behind an earlier one that can return the same verdict for a
// different reason, the test must make every earlier check pass cleanly --
// hold audio fresh at eval time unless the audio-gap check is the thing
// under test -- or the test proves nothing about the check it claims to
// isolate. Two blocks below (the reconnect latch and the second
// backwards-clock case) exist specifically because an earlier version of
// this file got that wrong.
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
    // moment, and the operator chose never to take it. BOTH last_audio_ms
    // and last_renewal_ms are held fresh AT EVAL TIME (T + 5000, matching
    // now) so this genuinely isolates the structural !engine_alive close --
    // not the audio-gap check or the renewal check, either of which would
    // independently return Close once its own timer had run out, masking a
    // deleted structural check exactly like the bug this replaced.
    {
        TalkbackKeyState s = healthy(T, TalkbackKeyMode::Latch);
        s.last_audio_ms   = T + 5000;
        s.last_renewal_ms = T + 5000;
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

    // ── A backwards clock must not close via the renewal check either ──────
    // The case above never reaches the renewal check: its stale audio alone
    // would return None from the audio-gap check regardless of the guard, so
    // it cannot prove the renewal check's own use of the guard. Hold audio
    // fresh at eval time and put ONLY last_renewal_ms in the future so
    // evaluation actually reaches the renewal check with a backwards delta.
    {
        TalkbackKeyState s = healthy(T, TalkbackKeyMode::PushToTalk);
        s.last_audio_ms   = T;
        s.last_renewal_ms = T + 5000;
        check(talkback_key_evaluate(s, T, true, true) == TalkbackKeyAction::None,
              "a backwards clock underflowed the renewal check and closed a "
              "healthy key");
    }

    // ── The engine's confirmed session state closes the key (final review,
    // C2) ──────────────────────────────────────────────────────────────────
    // The dead-man switch cannot see any of this: the tap keeps publishing
    // into the ring whether or not anything on the far end can hear it, so
    // every state below looks healthy to talkback_key_evaluate().
    {
        // Live with no reason: the normal open key. Nothing to do, grace
        // period irrelevant.
        check(!talkback_session_state_closes_key(true, false, false),
              "a live session closed the key");
        check(!talkback_session_state_closes_key(true, false, true),
              "a live session closed the key once the grace period elapsed");

        // Not live, no reason yet, inside the grace period: the engine has
        // simply not answered. Closing here would kill every key before its
        // own round trip could confirm it.
        check(!talkback_session_state_closes_key(false, false, false),
              "a key was closed before the engine had a chance to answer");

        // Not live, still silent, grace expired.
        check(talkback_session_state_closes_key(false, false, true),
              "a session that never answered at all was left open");

        // AN EXPLICIT FAILURE CLOSES IMMEDIATELY, inside the grace period.
        // This is the shape every report_session_state(false, reason) takes:
        // "layout_mismatch" from a half-applied install, "no_nomination",
        // "provisioning_incomplete" -- and, as of final-review C2,
        // "channels_destroyed": a session that WAS live whose channels a
        // failing nomination ladder tore down mid-sentence. That last one is
        // the whole plugin half of C2 -- the engine's report has to route
        // into the SAME close path, or the key stays open, the tally stays
        // red, and nothing reaches Zoom.
        check(talkback_session_state_closes_key(false, true, false),
              "AN EXPLICIT ENGINE FAILURE DID NOT CLOSE THE KEY -- the "
              "director is left believing they are on air");
        check(talkback_session_state_closes_key(false, true, true),
              "an explicit engine failure past the grace period did not close "
              "the key");
    }

    if (failures == 0)
        std::cout << "talkback-key: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
