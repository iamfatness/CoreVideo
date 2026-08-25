#pragma once
//
// talkback-key.h — when a talkback key may stay open, and every way it closes.
//
// Extracted from Qt and libobs so both failure directions can be tested
// without a meeting, the same treatment src/join-watchdog.h and
// src/director-handover.h get, and for the same reason: both directions are
// invisible until they happen on a live show.
//
// THE DESIGN IS A DEAD-MAN SWITCH. While a key is open the OBS tap delivers
// buffers continuously -- including silence, because an active OBS source
// calls back whether or not anyone is talking. So the ring's own traffic IS
// the liveness signal, and the key closes on a gap. Nothing has to NOTICE a
// failure:
//
//   OBS quits / plugin crashes  -> buffers stop -> gap expires
//   Engine restarts             -> channel died with the process
//   Pipe drops                  -> notify edge stops -> gap expires
//   Source removed or inactive  -> buffers stop -> gap expires
//   Meeting rejoin               -> channel and membership are meeting-scoped
//
// THE ONE FAILURE THE GAP CANNOT CATCH is a lost button release while the
// socket stays healthy: buffers keep flowing, so the gap never fires and the
// director is live without knowing. Renewals therefore run in the opposite
// direction too -- a key opened over the control API stays open only while
// the controller keeps asserting it. That is a LIVENESS renewal, not a
// maximum-open-time cap: a deliberate latch may stay open all day as long as
// something keeps saying it is still wanted. Surfaces whose release is
// in-process and reliable (the OBS hotkey) set needs_renewal = false.
//
#include <cstdint>

enum class TalkbackKeyMode {
    // Audio flows only while the control is held.
    PushToTalk,
    // Tap on, tap off. Never survives a reconnect -- see the tests.
    Latch,
};

enum class TalkbackKeyAction {
    // Nothing to do.
    None,
    // The key should be opened (reserved for the caller's open path).
    Open,
    // Close it now.
    Close,
};

// A gap longer than this means the audio path is gone. A few buffer periods:
// OBS delivers ~1024 frames (~21ms at 48kHz) per callback, so 250ms is roughly
// a dozen missed callbacks -- long enough not to trip on ordinary scheduling
// jitter, short enough that a dead key is not audible as a held-open channel.
constexpr uint64_t kTalkbackAudioGapMs = 250;

// How often a renewing controller must re-assert an open key...
constexpr uint64_t kTalkbackRenewalMs = 500;
// ...and how many it may miss before the key closes. Two gives ~1s, which
// tolerates one dropped packet without tolerating a dropped operator.
constexpr uint64_t kTalkbackRenewalsMissed = 2;

struct TalkbackKeyState {
    bool            open;
    TalkbackKeyMode mode;
    // Monotonic ms when audio last arrived in the ring.
    uint64_t        last_audio_ms;
    // Monotonic ms when the controller last re-asserted this key.
    uint64_t        last_renewal_ms;
    // False for surfaces whose release is in-process and reliable.
    bool            needs_renewal;
};

// Guard every subtraction: a clock that went backwards must not underflow into
// an enormous elapsed time and close a healthy key. join-watchdog.h carries
// the same guard for the same reason.
inline uint64_t talkback_elapsed_ms(uint64_t now_ms, uint64_t then_ms)
{
    return now_ms <= then_ms ? 0 : now_ms - then_ms;
}

inline TalkbackKeyAction talkback_key_evaluate(const TalkbackKeyState &s,
                                               uint64_t now_ms,
                                               bool engine_alive,
                                               bool in_meeting)
{
    // A closed key is never reopened here and never double-closed. Opening is
    // an operator action, not something a periodic evaluation may decide.
    if (!s.open) return TalkbackKeyAction::None;

    // Structural closes: no channel can exist without these, so nothing else
    // needs checking.
    if (!engine_alive || !in_meeting) return TalkbackKeyAction::Close;

    // The dead-man switch.
    if (talkback_elapsed_ms(now_ms, s.last_audio_ms) > kTalkbackAudioGapMs)
        return TalkbackKeyAction::Close;

    // The lost-release backstop.
    if (s.needs_renewal &&
        talkback_elapsed_ms(now_ms, s.last_renewal_ms) >
            kTalkbackRenewalMs * kTalkbackRenewalsMissed)
        return TalkbackKeyAction::Close;

    return TalkbackKeyAction::None;
}
