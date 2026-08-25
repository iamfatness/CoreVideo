#pragma once
//
// talkback-channel-owner.h — who a CreateChannel response belongs to.
//
// Two subsystems ask the Zoom SDK for talkback channels:
//
//   * the Milestone 1 PROBE, which creates one, sends a 3s tone, and destroys
//     it from tick() on its own driving thread;
//   * the talkback SESSION, which creates one and holds it open for as long
//     as a key is down.
//
// CreateChannel(1) does not return the channel id. It arrives later in
// onCreateChannelResponse, which says nothing about who asked. Route it wrong
// and either the probe adopts the session's channel (and destroys it three
// seconds later, mid-sentence) or the session adopts the probe's (which tick()
// then destroys underneath it). Both are silent on a live show.
//
// THE RULE: exactly one create may be outstanding at a time. That costs
// nothing, because both callers run on the engine's single command-loop
// thread -- CreateChannel has only ever been called from there
// (engine-talkback.cpp's probe(), reached from main.cpp's command loop), and
// this plan keeps it that way. A queue would buy nothing and would add a way
// for the two to interleave.
//
// Free of Qt / OBS / Zoom SDK dependencies so the routing can be pinned by a
// test with no engine and no meeting.
//
enum class TalkbackChannelOwner {
    // Nothing outstanding.
    None,
    // The Milestone 1 probe ladder.
    Probe,
    // The persistent talkback session.
    Session,
};

// May a subsystem issue CreateChannel right now?
inline bool talkback_may_request_create(TalkbackChannelOwner pending)
{
    return pending == TalkbackChannelOwner::None;
}

// Who owns the create response that just arrived, given what was outstanding.
//
// Returns None when nothing was outstanding: a late or duplicate response must
// be adopted by NOBODY and handled as a stray. The SDK can redeliver, and
// adopting a redelivered response would hand one subsystem a channel the other
// already owns.
//
// The caller clears its own pending state; this function does not mutate, so
// it stays a pure decision the test can drive exhaustively.
inline TalkbackChannelOwner talkback_claim_create(TalkbackChannelOwner pending)
{
    return pending;
}
