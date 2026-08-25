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
// THE RULE: exactly one create may be outstanding at a time, tracked in
// EngineTalkback::m_pending_create (engine-talkback.h).
//
// THREADING -- read this before touching m_pending_create's synchronization.
// An earlier version of this comment claimed both CreateChannel callers run
// on the engine's single command-loop thread, so the field "costs nothing"
// and needs none. That was true until review-round R3 added a driving-thread
// writer, and is no longer true as written -- do not restore it. As of R3,
// m_pending_create has FOUR write sites, on TWO different threads:
//   * probe() and session_start() (engine-talkback.cpp) CLAIM it (None ->
//     Probe / None -> Session). Both run on the engine's command-loop
//     thread, which on Windows is also the SDK's message-pump thread. The
//     GATE (talkback_may_request_create) is checked BEFORE their
//     CreateChannel call; the CLAIM (the actual store of Probe/Session) only
//     happens AFTER that call returns SDKERR_SUCCESS. Do not conflate the
//     two: the field reads None for the whole span between the gate check
//     and the store, which is precisely the window this arbiter exists to
//     reason about -- a maintainer who assumes the claim happens before the
//     call will misjudge when m_pending_create actually becomes non-None.
//   * onCreateChannelResponse (same file) CLEARS it (-> None) when it
//     attributes a response to its owner. Also the command-loop thread --
//     SDK callbacks run there for the same message-pump reason as above.
//   * session_stop() (same file) also CLEARS it (-> None, only when the
//     pending owner is Session), in two places: the "nothing to tear down"
//     early branch and the main teardown path. Same command-loop thread as
//     every write site above.
//   * tick()'s AwaitingChannel-timeout handling (same file, R3 fix) ALSO
//     clears it (-> None, only when the pending owner is Probe), to stop a
//     swallowed CreateChannel response from wedging the arbiter forever.
//     This one runs on the PROBE'S OWN separate driving thread (see tick()'s
//     own top-of-function comment) -- genuinely concurrent with the other
//     three, not merely a different call site on the same thread. It is the
//     ONLY write site not on the command-loop thread, and is therefore the
//     entire reason this field needs synchronization at all.
// Because of that one driving-thread writer, m_pending_create is guarded by
// EngineTalkback's m_chan_mtx everywhere it is read or written -- copy the
// decision out under the lock, release, THEN call the SDK, same discipline
// as every other m_chan_mtx access in that class. If a future change moves
// tick()'s clear back onto the command-loop thread and removes the last
// driving-thread writer, this paragraph -- and the mutex requirement -- can
// be revisited, but do not strip the guarding on the strength of THIS
// comment's old claim; verify the thread each writer runs on first, and
// recount the write sites -- this paragraph has already been caught stale
// once by undercounting them. A queue instead of a single outstanding slot
// would buy nothing here and would add a way for the probe and the session
// to interleave.
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
