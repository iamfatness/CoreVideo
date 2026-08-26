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
// paragraph has ALREADY been caught stale THREE times by miscounting write
// sites (once pre-Task-2; once in Task 2's own first pass, which also
// asserted a write site -- session_stop()'s early branch clearing the field
// -- that the F1 fix had removed; and once in fix round 3, which said SEVEN
// while round 3's own new Stale branch had made it EIGHT). Do not trust the
// count below either without re-deriving it from the actual code; the
// discipline that matters is "verify the thread each writer runs on, from
// the code, every time this paragraph is touched," not the specific number
// that follows.
//
// As of Task 2 fix round 4 (2026-08-25), SEVEN write STATEMENTS, in SEVEN
// distinct functions, write m_pending_create, on TWO different threads --
// re-counted in round 4 with `grep -nE "^[^/]*m_pending_create +="` over
// engine-talkback.cpp, naming the enclosing function for each hit, not by
// counting bullets in this paragraph. Use that regex, not the literal
// `m_pending_create = `: expire_stale_pending_create_locked()'s write is
// column-aligned (`m_pending_create           = state.owner;`) and a
// fixed-space grep silently misses it, which is one way a previous count
// went wrong. (An older version of the count, "five", counted bullets -- one
// of which named three functions -- and separately misnamed nominate() as a
// writer when only the function it calls, nomination_create_next(), actually
// assigns the field.) Plus two call sites that deliberately do NOT write it
// despite looking like they should:
//   * probe() and session_start() (engine-talkback.cpp) CLAIM it (None ->
//     Probe / None -> Session). nomination_create_next() (same file) CLAIMS
//     it the same way (None -> Nomination) -- called from nominate() (which
//     itself never writes the field) for the plan's first channel, and again
//     from onCreateChannelResponse's Nomination branch for every channel
//     still queued after that. All three claimers run on the engine's
//     command-loop thread, which on
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
//     (adopts it, destroys it as cancelled, destroys it as stale, or reports
//     a failure) -- the clear happens before any of those branches run, and
//     no branch re-claims it afterwards. That last clause is load-bearing
//     and was FALSE for one round: round 3's Stale branch re-claimed the
//     owner (m_pending_create = Nomination) after the clear, so a
//     maintainer reasoning about the arbiter from this sentence got the
//     wrong answer. Round 4 removed that re-claim -- if you add a branch
//     here that writes this field, this sentence and the count above are
//     both wrong again. Also the command-loop thread -- SDK callbacks run
//     there for the same message-pump reason as above.
//   * session_stop()'s MAIN teardown path (same file) CLEARS it (-> None,
//     only when the pending owner is Session and there is a channel/session
//     to actually tear down). Command-loop thread, same as every write site
//     above.
//   * tick()'s AwaitingChannel-timeout handling (same file, R3 fix) ALSO
//     clears it (-> None, only when the pending owner is Probe), to stop a
//     swallowed CreateChannel response from wedging the arbiter forever.
//     This one runs on the PROBE'S OWN separate driving thread (see tick()'s
//     own top-of-function comment) -- genuinely concurrent with the other
//     six, not merely a different call site on the same thread. It is the
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
// nomination to interleave -- fix round 3 proved that the expensive way, by
// tracking outstanding creates in a FIFO one layer up (see "Generation
// tracking" below) and wedging the feature permanently the first time an
// entry went unmatched.
//
// Free of Qt / OBS / Zoom SDK dependencies so the routing can be pinned by a
// test with no engine and no meeting.
//
#include <cstdint>

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

// ── Cancellation / disposition (Task 2 fix round 2, N1) ─────────────────────
//
// talkback_may_request_create / talkback_claim_create above answer "is a
// create outstanding, and whose is it". These answer what sits on top of
// that: Session and Nomination can each be CANCELLED while their create is
// still in flight (session_stop()'s early branch, nomination_reset() --
// both run when the caller wants to tear down but the CreateChannel already
// went to Zoom and cannot be un-sent) without losing track of whose it was,
// can EXPIRE unanswered (expire_stale_pending_create_locked(), the same
// self-heal tick()'s AwaitingChannel timeout gives Probe), and the eventual
// response must be given a DISPOSITION -- adopt it, or destroy it because it
// was cancelled.
//
// engine-talkback.cpp calls these instead of re-deriving the transitions
// inline, specifically because inlining them separately per owner is
// exactly how they diverged: fix round 1 gave Session and Nomination each
// their own cancellation flag and their own copy of the expiry logic, and
// the Nomination copy forgot to clear its flag (N1, fix round 2) --
// `m_nomination_create_cancelled` stayed true after
// `expire_stale_pending_create_locked()` forgot the owner it belonged to,
// so it silently misapplied to the NEXT Nomination create, destroying a
// brand-new channel instead of adopting it. Routing both owners' expiry
// through the ONE `talkback_expire()` below closes that specific asymmetry.
//
// Fix round 2 shipped with each flag's OTHER clearer -- the check-and-clear
// at the top of onCreateChannelResponse's Session and Nomination branches --
// still written as two hand-rolled per-owner copies, an overclaim the
// round-2 re-review caught: "no second per-owner copy" was true for expiry
// and false for this site. `talkback_check_and_clear_cancelled()` below
// closes that gap the same way `talkback_expire()` closed the first one, so
// there is now genuinely no per-owner copy of EITHER clearer left in
// engine-talkback.cpp for an edit to apply to only one arm of.
//
// Bundles both owners' cancellation flags together (rather than two
// separate bool parameters) because that is the actual shape of the bug:
// N1 was a state that should not have been reachable -- Nomination pending
// with a stale `nomination_cancelled` still true -- and a struct makes that
// state constructible and inspectable in a test the same way the real
// engine's three member variables are, rather than requiring two calls that
// could themselves be called out of sync.
struct TalkbackCreateState {
    TalkbackChannelOwner owner = TalkbackChannelOwner::None;
    bool session_cancelled = false;
    bool nomination_cancelled = false;
};

// What a create response should do once its owner is known.
enum class TalkbackCreateDisposition {
    // Nothing was outstanding for this response -- an untracked stray.
    Stray,
    // The owner that made this create cancelled it before the response
    // arrived -- destroy the channel, do not adopt it.
    DestroyCancelled,
    // Ordinary case: adopt the channel for whichever owner made the create.
    Claim,
};

// Records a cancellation for the create currently pending for `owner`, if
// any -- the pure decision behind session_stop()'s early branch and
// nomination_reset(). Deliberately does NOT clear `state.owner`: the
// create already went to Zoom, so forgetting it here (rather than
// cancelling it) is the exact F1/C1 bug -- the eventual response would be
// claimed by nobody and wedge onto a stray queue nothing drains. No-op
// (state returned unchanged) if `owner` is not the one currently pending --
// there is nothing outstanding to cancel.
inline TalkbackCreateState talkback_cancel(TalkbackCreateState state, TalkbackChannelOwner owner)
{
    if (state.owner != owner) return state;
    if (owner == TalkbackChannelOwner::Session) state.session_cancelled = true;
    else if (owner == TalkbackChannelOwner::Nomination) state.nomination_cancelled = true;
    return state;
}

// Forgets whichever create is currently pending -- the pure decision behind
// expire_stale_pending_create_locked() (Session or Nomination only; Probe's
// timeout is tick()'s separate AwaitingChannel mechanism and never reaches
// this function). Clears the owner AND that owner's own cancellation flag
// TOGETHER: this symmetry is the entire fix for N1. A no-op call (owner is
// already None) returns `state` unchanged.
inline TalkbackCreateState talkback_expire(TalkbackCreateState state)
{
    if (state.owner == TalkbackChannelOwner::Session) state.session_cancelled = false;
    else if (state.owner == TalkbackChannelOwner::Nomination) state.nomination_cancelled = false;
    state.owner = TalkbackChannelOwner::None;
    return state;
}

// What onCreateChannelResponse should do with a response, given `owner`
// (whichever owner talkback_claim_create() just returned for it) and
// `cancelled` (that SAME owner's own cancellation flag at the moment of the
// claim -- Probe has none, so its caller always passes false; Probe's
// timeout disposition is tick()'s separate mechanism, not this one).
inline TalkbackCreateDisposition talkback_create_disposition(TalkbackChannelOwner owner, bool cancelled)
{
    if (owner == TalkbackChannelOwner::None) return TalkbackCreateDisposition::Stray;
    if (cancelled) return TalkbackCreateDisposition::DestroyCancelled;
    return TalkbackCreateDisposition::Claim;
}

// Reads and clears whichever cancellation flag belongs to `owner`, in one
// step -- the pure decision behind the check-and-clear at the top of
// onCreateChannelResponse's Session and Nomination branches (`cancelled =
// m_*_create_cancelled; m_*_create_cancelled = false;`). Fix round 2 routed
// the SETTER (talkback_cancel()) and the EXPIRE clearer (talkback_expire())
// through shared functions but left this, the RESPONSE clearer, as two
// hand-written per-owner copies -- the round-2 re-review named this an
// overclaim in this file's own header comment above. Routing it through one
// function here closes the same class of gap N1 was, before an edit to only
// one owner's copy has the chance to reopen it.
struct TalkbackCreateCheckResult {
    bool cancelled;
    TalkbackCreateState next;
};
inline TalkbackCreateCheckResult talkback_check_and_clear_cancelled(TalkbackCreateState state,
                                                                    TalkbackChannelOwner owner)
{
    const bool cancelled =
        owner == TalkbackChannelOwner::Session   ? state.session_cancelled :
        owner == TalkbackChannelOwner::Nomination ? state.nomination_cancelled :
                                                     false;
    if (owner == TalkbackChannelOwner::Session) state.session_cancelled = false;
    else if (owner == TalkbackChannelOwner::Nomination) state.nomination_cancelled = false;
    return TalkbackCreateCheckResult{cancelled, state};
}

// ── Generation tracking (Task 2 fix round 3; REBUILT in fix round 4) ────────
//
// C1/N1 fixed the CANCELLATION half of "a create outstanding across
// Leave()/expiry must not be misattributed". This is the other half, found
// by the round-2 re-review: a create that merely EXPIRES -- nobody
// cancelled it, its response is just slow, or genuinely lost -- can still
// have that response arrive AFTER a fresh nomination has re-armed the SAME
// owner and issued a SECOND CreateChannel. `onCreateChannelResponse` carries
// no id correlating it to which CreateChannel call produced it, so `owner`
// alone cannot tell "the response I am currently waiting for" apart from "a
// response for a create I gave up on". A flag cannot answer this either --
// N1 was a flag surviving too long; this is an IDENTITY problem, not a
// binary one. src/shm-generation.h solves the same shape of problem (a
// stale SHM reader must not be mistaken for a current one) with a
// monotonically increasing generation carried alongside the state and
// checked when the ambiguous event resolves; this follows that precedent.
//
// WHY THIS IS ONE SCALAR AND NOT A QUEUE -- read this before "improving" it.
// Fix round 3 shipped the same generation idea carried in a FIFO: one push
// per successful CreateChannel, one pop per response that reached the
// Nomination branch. That modelled a state THE ARBITER FORBIDS -- more than
// one create outstanding at a time -- and it turned into a permanent feature
// wedge the first time a push went unmatched, which two entirely ordinary
// paths do:
//   * a response that is never delivered at all -- the exact case
//     m_nomination_create_deadline exists for (engine-talkback.h says so in
//     as many words), and
//   * a late response arriving while the owner is None/Probe/Session, which
//     never reaches the Nomination branch and so never pops.
// One orphaned entry left the FIFO permanently off by one; because `current`
// is bumped between ladders, every later response then compared an OLDER
// entry, read Stale, destroyed the channel Zoom had just created for it,
// provisioned zero channels, and repeated for the life of the process --
// reachable by `nominate -> swallowed response -> key press (expiry) ->
// nominate`. That was strictly worse than the rare, transient
// misattribution it replaced. The lesson is not "the counter was wrong": it
// is that a container whose entries are added and removed on different
// paths can DESYNCHRONISE, and a single slot cannot.
//
// So: one scalar. `outstanding` says whether a create is outstanding at all,
// `outstanding_generation` says which generation issued it, and every issue
// OVERWRITES both. There is nothing to keep in step, so nothing can fall out
// of step, and a CURRENT response can never be judged Stale -- the stamp is
// written by the very create whose response this is, and only a bump (which
// only ever happens with the owner released) can separate them.
//
// FAIL OPEN, deliberately. Where the state cannot explain a response
// (`Unexpected`), engine-talkback.cpp treats it as Current and lets the
// ladder keep moving rather than destroying anything: a wrongly-KEPT channel
// costs one leaked channel out of the meeting's 16, while a wrongly-
// DESTROYED one costs the operator the whole talkback feature mid-show. Only
// `Stale` -- a create we positively know we gave up on -- destroys, and it
// destroys ONLY the channel that response names, touching no current state
// and never advancing the ladder.
//
// The residual this accepts, knowingly: when create A's response is never
// delivered, is expired, and a later ladder's create B overwrites the stamp,
// A's response (if it ever does arrive, with the owner re-claimed by B) is
// indistinguishable from B's and is adopted as B's. No scheme can do better
// -- Zoom gives no correlation id -- and the cost is bounded and self-
// limiting: the ladder may end up with one extra create in flight, whose
// response finds m_nomination_pending empty and is destroyed down the
// `channel_untracked` path. That is the round-2 Major, and accepting it is
// the deliberate price of never wedging.
//
// No wraparound handling: realistic usage is dozens of nominate() calls and
// expiries in a show, not the four billion `current` would need to wrap --
// unlike src/shm-generation.h's counter (bumped per resubscribe, which CAN
// run into the tens of thousands over a long process lifetime), saturating
// this one would be solving a problem this feature does not have. The
// comparison is `==`/`!=`, never `<`, so even a wrap could only cost one
// misjudged response rather than inverting the ordering forever.
struct TalkbackGenerationState {
    // Bumped by a fresh nominate() and by a Nomination expiry.
    uint32_t current = 0;
    // Is exactly one Nomination create outstanding right now, and under
    // which generation was it issued? A bool rather than a sentinel
    // generation value so 0 never has to double as "none" -- the first
    // create a process ever issues is stamped 0.
    bool     outstanding = false;
    uint32_t outstanding_generation = 0;
};

enum class TalkbackResponseFreshness {
    // A create IS recorded as outstanding, but it was issued under a
    // generation the ladder has since moved past -- this response belongs to
    // a create we gave up on. The ONLY verdict that destroys.
    Stale,
    // The outstanding create's generation matches `current`: this is the
    // response the ladder is actually waiting on.
    Current,
    // Nothing was recorded as outstanding at all -- e.g. a redelivered
    // response for a create that already resolved. Ambiguous, so it fails
    // OPEN: engine-talkback.cpp treats this exactly like `Current`. See the
    // FAIL OPEN paragraph above for why that is the cheaper mistake.
    Unexpected,
    // The response was not claimed by Nomination at all (owner was None,
    // Probe or Session), so this state has no opinion about it and -- the
    // load-bearing half -- was NOT mutated. Round 3's FIFO desynchronised
    // permanently precisely because this case silently skipped its pop; a
    // scalar has nothing to skip, and this value exists so a test can say so
    // out loud.
    NotNomination,
};

// Bumps the generation: everything issued before this instant is now stale.
// The pure decision behind BOTH of engine-talkback.cpp's bump sites --
// nominate() (a fresh ladder must not be confused with an older one) and
// expire_stale_pending_create_locked()'s Nomination arm (an abandoned create
// must not be confused with whatever comes after it). One function for both
// because they are the same transition; naming them separately would be the
// per-owner-copy shape this file's own history keeps getting caught by.
//
// Deliberately does NOT clear `outstanding`: leaving the abandoned create's
// stamp in place is what makes ITS response read Stale if it arrives before
// anything else is issued. The stamp is overwritten (not queued behind) by
// the next talkback_generation_issue(), so it can never accumulate.
inline TalkbackGenerationState talkback_generation_bump(TalkbackGenerationState state)
{
    ++state.current;
    return state;
}

// Records that a create was just issued under the current generation -- the
// pure decision behind nomination_create_next() stamping a CreateChannel
// call right after it returns SDKERR_SUCCESS. Overwrites unconditionally:
// the arbiter's promise is that only ONE create is outstanding at a time, so
// anything already stamped here belonged to a create that was abandoned, and
// there is nothing to reconcile with it. This unconditional overwrite is the
// whole reason a desynchronisation is not expressible.
inline TalkbackGenerationState talkback_generation_issue(TalkbackGenerationState state)
{
    state.outstanding = true;
    state.outstanding_generation = state.current;
    return state;
}

// Judges a create response, given the owner the arbiter just claimed it for.
// Takes `owner` (rather than being called only from inside the Nomination
// branch) so that "a response arrived under some OTHER owner" is a case this
// pure function answers -- and a test can pin -- instead of a case the engine
// expresses by not calling anything. That silent non-call was half of the
// round-3 Critical.
struct TalkbackResponseCheck {
    TalkbackResponseFreshness freshness;
    TalkbackGenerationState next;
};
inline TalkbackResponseCheck talkback_generation_on_response(TalkbackGenerationState state,
                                                             TalkbackChannelOwner owner)
{
    if (owner != TalkbackChannelOwner::Nomination)
        return TalkbackResponseCheck{TalkbackResponseFreshness::NotNomination, state};
    if (!state.outstanding)
        return TalkbackResponseCheck{TalkbackResponseFreshness::Unexpected, state};
    if (state.outstanding_generation != state.current) {
        // Stale: leave the state exactly as it is. The caller destroys the
        // channel this response names and returns without advancing
        // anything, so the stamp must survive -- a second late response for
        // the same abandoned create must reach the same verdict.
        return TalkbackResponseCheck{TalkbackResponseFreshness::Stale, state};
    }
    // Current: the create we were waiting on has now resolved, so nothing is
    // outstanding until the next issue.
    state.outstanding = false;
    return TalkbackResponseCheck{TalkbackResponseFreshness::Current, state};
}
