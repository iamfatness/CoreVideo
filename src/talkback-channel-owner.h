#pragma once
//
// talkback-channel-owner.h — who a CreateChannel response belongs to.
//
// Three subsystems ask the Zoom SDK for talkback channels:
//
//   * the Milestone 1 PROBE, which creates one, sends a 3s tone, and destroys
//     it from tick() on its own driving thread;
//   * the talkback SESSION, which creates one and holds it open for as long
//     as a key is down;
//   * NOMINATION (Task 2, 2026-08-25), which creates every channel
//     talkback_plan() decides on for a nominated talent list, one at a time,
//     at nomination time rather than at key time -- see src/talkback-plan.h
//     and engine-talkback.h's nominate().
//
// CreateChannel(1) does not return the channel id. It arrives later in
// onCreateChannelResponse, which says nothing about who asked. Route it wrong
// and the probe can adopt the session's or nomination's channel (and destroy
// it three seconds later, mid-sentence or mid-provisioning), or the session
// or nomination can adopt the probe's (which tick() then destroys underneath
// it). All three misroutings are silent on a live show.
//
// THE RULE: exactly one create may be outstanding at a time, tracked in
// EngineTalkback::m_pending_create (engine-talkback.h).
//
// THREADING -- read this before touching m_pending_create's synchronization.
// An earlier version of this comment claimed both CreateChannel callers run
// on the engine's single command-loop thread, so the field "costs nothing"
// and needs none. That was true until review-round R3 added a driving-thread
// writer, and is no longer true as written -- do not restore it. This
// paragraph has ALREADY been caught stale twice by undercounting write sites
// (once pre-Task-2, once in Task 2's own first pass, which also asserted a
// write site -- session_stop()'s early branch clearing the field -- that the
// F1 fix had removed). Do not trust the count below either without
// re-deriving it from the actual code; the discipline that matters is
// "verify the thread each writer runs on, from the code, every time this
// paragraph is touched," not the specific number that follows.
//
// As of Task 2 fix round 1 (2026-08-25), five call sites WRITE
// m_pending_create, on TWO different threads, plus two call sites that
// deliberately do NOT write it despite looking like they should:
//   * probe(), session_start(), and nominate()/nomination_create_next()
//     (engine-talkback.cpp) CLAIM it (None -> Probe / None -> Session / None
//     -> Nomination). All run on the engine's command-loop thread, which on
//     Windows is also the SDK's message-pump thread. The GATE
//     (talkback_may_request_create) is checked BEFORE their CreateChannel
//     call; the CLAIM (the actual store of Probe/Session/Nomination) only
//     happens AFTER that call returns SDKERR_SUCCESS. Do not conflate the
//     two: the field reads None for the whole span between the gate check
//     and the store, which is precisely the window this arbiter exists to
//     reason about -- a maintainer who assumes the claim happens before the
//     call will misjudge when m_pending_create actually becomes non-None.
//   * onCreateChannelResponse (same file) CLAIMS-then-CLEARS it (-> None) in
//     the same lock scope, for whichever owner is currently pending,
//     regardless of what that owner's branch then does with the response
//     (adopts it, destroys it as cancelled, or reports a failure) -- the
//     clear happens before any of those branches run. Also the command-loop
//     thread -- SDK callbacks run there for the same message-pump reason as
//     above.
//   * session_stop()'s MAIN teardown path (same file) CLEARS it (-> None,
//     only when the pending owner is Session and there is a channel/session
//     to actually tear down). Command-loop thread, same as every write site
//     above.
//   * tick()'s AwaitingChannel-timeout handling (same file, R3 fix) ALSO
//     clears it (-> None, only when the pending owner is Probe), to stop a
//     swallowed CreateChannel response from wedging the arbiter forever.
//     This one runs on the PROBE'S OWN separate driving thread (see tick()'s
//     own top-of-function comment) -- genuinely concurrent with the other
//     four, not merely a different call site on the same thread. It is the
//     ONLY write site not on the command-loop thread, and is therefore the
//     entire reason this field needs synchronization at all.
//   * expire_stale_pending_create_locked() (same file, extended for Task 2)
//     ALSO clears it (-> None, for a stale Session OR a stale Nomination),
//     lazily, from inside the gate check every claimer above already takes
//     under m_chan_mtx -- the same self-healing tick()'s timeout gives
//     Probe, given to Session and Nomination without a second thread or
//     timer. Command-loop thread, same as its callers.
//
// The two call sites that deliberately do NOT write m_pending_create, even
// though a naive read of "this is the teardown path" would expect them to:
//   * session_stop()'s "nothing to tear down" early branch, when the pending
//     owner is Session. Writing None here was the F1 CRITICAL bug: the
//     CreateChannel had already gone to Zoom, and clearing the arbiter's
//     record of it did not cancel that request -- the eventual response
//     would be claimed by nobody, match no tracked channel, and wedge onto
//     m_stray_channels forever. The fix sets m_session_create_cancelled
//     instead and leaves the owner AS Session, so onCreateChannelResponse
//     still routes the response to the Session branch, which destroys it.
//   * nomination_reset() (same file, Task 2 fix round 1 -- the ORIGINAL
//     version of this function DID write None here unconditionally, which
//     was the Critical finding of fix round 1: the unfixed F1 bug,
//     reintroduced for Nomination). Now mirrors session_stop()'s early
//     branch exactly: sets m_nomination_create_cancelled instead of writing
//     m_pending_create, for the same reason.
// Because of the one driving-thread writer, m_pending_create is guarded by
// EngineTalkback's m_chan_mtx everywhere it is read or written -- copy the
// decision out under the lock, release, THEN call the SDK, same discipline
// as every other m_chan_mtx access in that class. If a future change moves
// tick()'s clear back onto the command-loop thread and removes the last
// driving-thread writer, this paragraph -- and the mutex requirement -- can
// be revisited, but do not strip the guarding on the strength of THIS
// comment's old claim; verify the thread each writer runs on first, and
// recount the write sites. A queue instead of a single outstanding slot
// would buy nothing here and would add a way for the probe, the session, and
// nomination to interleave.
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
    // Pre-provisioning a nominated talent list's channels (Task 2).
    Nomination,
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
