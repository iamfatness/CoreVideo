#pragma once
//
// talkback-channel-owner.h — who a CreateChannel response belongs to.
//
// Three subsystems ask the Zoom SDK for talkback channels:
//
//   * the Milestone 1 PROBE, which creates one, sends a 3s tone, and destroys
//     it from tick() on its own driving thread;
//   * the talkback SESSION, which used to create one on the key press and
//     hold it open for as long as the key was down. It does not any more:
//     Task 3 (2026-08-25) made keying SELECT an already-provisioned channel,
//     so no call site claims this arbiter for Session. The enum value and its
//     transitions are still modelled and tested below, and this file still
//     describes them, because they are the record of what a create-on-key
//     subsystem has to do -- but do not read the Session arms as live code.
//     `grep -n "TalkbackChannelOwner::Session" engine/src/engine-talkback.cpp`
//     is the check, and it finds nothing today;
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
// EngineTalkback::m_pending_create (engine-talkback.h) -- ONE member holding
// the whole arbiter state (owner, both cancellation flags, and Nomination's
// generation), mutated ONLY by the pure transitions in this file.
//
// THREADING -- read this before touching m_pending_create's synchronization.
// An earlier version of this comment claimed both CreateChannel callers run
// on the engine's single command-loop thread, so the field "costs nothing"
// and needs no lock. That was true until review-round R3 added a
// driving-thread writer, and is no longer true -- do not restore it.
//
// This paragraph has been caught wrong about itself in FOUR consecutive
// rounds: twice by undercounting write sites, once by asserting a write site
// the F1 fix had removed, once by prescribing a grep that matched `==` as
// well as `=` (so following its own stated method yielded eleven "writes" and
// re-condemned it), and once by introducing a five-bullet list of WRITERS
// with the sentence "Plus two call sites that deliberately do NOT write it".
// Learn from the pattern rather than from the corrections: prose that
// enumerates code goes stale, so what follows enumerates as little as
// possible and says how to re-derive the rest.
//
// THE INVARIANT (verify this, not a list): every mutation of
// m_pending_create is a WHOLE-STATE store, under EngineTalkback::m_chan_mtx,
// of a state produced by one of the pure transitions below (issued /
// response / expire / cancel / new_ladder). There are no field-by-field
// writes and no partial updates -- that is deliberate, and it is fix round
// 5's answer to two defects that both came from the state being four
// separate members: a branch that returned before one of its updates (F1),
// and a mutation deleting one of four write-backs with the entire test suite
// still green.
//
// To re-derive the sites, run this over engine-talkback.cpp -- it is exact
// because of the invariant above. It was run, and its output counted, before
// the sentence after it was written; if you change this regex, do the same,
// because a prescription that does not produce the number beside it has
// itself been a finding in this file:
//
//     grep -nE "^[^/]*m_pending_create *= [^=]" engine/src/engine-talkback.cpp
//
// SEVEN stores, in SEVEN functions, on TWO threads (Task 3, 2026-08-25):
// probe(), expire_stale_pending_create_locked(), tick(),
// onCreateChannelResponse(), nominate(), nomination_create_next(), and
// nomination_reset(). It was ten in nine before Task 3 removed the session's
// CreateChannel and with it session_start()'s claim and session_stop()'s two
// stores. The `^[^/]*` prefix is what keeps commented-out and quoted
// occurrences out of the count -- and `= [^=]` is what keeps `==`
// comparisons out; an earlier version of this block prescribed a regex
// without either guard and told the reader to expect a number four higher
// than the writes. If your count differs from seven, the code changed; trust
// the grep, not this line.
//
// The distinctions that actually matter, none of which a count can carry:
//
//   * GATE then CLAIM, never together. probe() and nomination_create_next()
//     -- the two CreateChannel callers left -- check
//     talkback_may_request_create() BEFORE calling CreateChannel and store
//     the claim (via talkback_create_issued())
//     only AFTER it returns SDKERR_SUCCESS. The owner therefore reads None
//     for the whole span between the gate check and the store -- precisely
//     the window this arbiter exists to reason about. A maintainer who
//     assumes the claim happens before the SDK call will misjudge it.
//   * The response CLAIMS-then-CLEARS, and no branch may re-claim.
//     onCreateChannelResponse attributes the response, releases the owner,
//     check-and-clears that owner's cancellation flag and judges its
//     generation in ONE call (talkback_create_response()) before any branch
//     runs. Both halves of that sentence have been false and each cost a
//     defect: round 3's Stale branch re-claimed the owner after the clear,
//     and round 4's branches did their own check-and-clear, which the Stale
//     branch's early return jumped over (F1 -- the next nomination then
//     destroyed its own first channel and refused every later one for the
//     rest of the meeting). If you add a branch here, it inherits the
//     already-completed transition; do not give it one of its own.
//   * TEARDOWN CANCELS, it does not forget. nomination_reset() stores a
//     talkback_cancel() -- setting that owner's cancellation flag and
//     LEAVING the owner claimed. session_stop()'s "nothing to tear down"
//     early branch did the same for Session until Task 3; it is named here
//     because the rule is what matters, not the count of call sites.
//     Writing None there was the F1/C1 CRITICAL,
//     twice (once for Session, then reintroduced for Nomination): the
//     CreateChannel had already gone to Zoom, so forgetting it did not
//     cancel it, and the eventual response was claimed by nobody, matched no
//     tracked channel, and wedged onto m_stray_channels -- which nothing
//     drains without a probe's driving thread, so has_pending_work() read
//     true forever and gated the top of both nominate() and session_start().
//   * ONE store is not on the command-loop thread: tick()'s
//     AwaitingChannel-timeout clear (R3 fix), which runs on the probe's own
//     driving thread. It is the entire reason this state needs a mutex at
//     all. Every other store is command-loop -- the two claimers, the
//     response callback (the SDK's message pump IS the command loop on
//     Windows), nomination_reset(),
//     expire_stale_pending_create_locked() (lazily, from inside the gate
//     check its callers already take) and nominate().
//
// Because of that one driving-thread store, m_pending_create is guarded by
// m_chan_mtx everywhere it is read or written -- copy the decision out under
// the lock, release, THEN call the SDK, same discipline as every other
// m_chan_mtx access in that class. If a future change moves tick()'s clear
// back onto the command-loop thread and removes the last driving-thread
// writer, this paragraph -- and the mutex requirement -- can be revisited,
// but do not strip the guarding on the strength of THIS comment; verify the
// thread each writer runs on first. A queue instead of a single outstanding
// slot would buy nothing here and would add a way for the probe, the session
// and nomination to interleave -- fix round 3 proved that the expensive way,
// by tracking outstanding creates in a FIFO one layer up (see "Generation
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
    //
    // Fix round 4 asserted here that this verdict was unreachable with an
    // owner of Nomination. It was reachable, and a Major (F1) lived behind
    // that assertion for a round -- so no reachability claim is made now, in
    // either direction. What holds regardless: only a POSITIVELY superseded
    // generation lands here. An ambiguous response is `Unexpected` and fails
    // open. Do not widen what routes here on the theory that a rarely-taken
    // check must be idle -- with no correlation id from Zoom, guessing which
    // response is stale destroys channels the ladder is legitimately waiting
    // on, which is a live-show outage, while guessing the other way leaks one
    // channel out of sixteen.
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

// ── Cancellation / disposition (Task 2 fix round 2, N1) ─────────────────────
//
// talkback_may_request_create / talkback_claim_create above answer "is a
// create outstanding, and whose is it". These answer what sits on top of
// that: an owner can be CANCELLED while its create is still in flight
// (nomination_reset() today; session_stop()'s early branch until Task 3 --
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
// `nomination_cancelled` stayed true after
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
//
// Fix round 5 folded the generation state in here as a fourth field, for the
// same reason the two flags were bundled in round 2 and one round harder:
// the engine used to mirror these four values into four separate members and
// write each back by hand, so "claim the arbiter" and "stamp the generation"
// were two adjacent assignments. A re-review mutation deleted just the
// generation one and the ENTIRE suite stayed green -- that is the shape that
// shipped round 3's Critical and the shape round 5's Major (F1) lived in.
// One struct, transitioned by the functions below and stored by the engine as
// ONE member, is what makes "claimed but not stamped" and "judged but not
// written back" unrepresentable rather than merely tested for.
struct TalkbackCreateState {
    TalkbackChannelOwner owner = TalkbackChannelOwner::None;
    bool session_cancelled = false;
    bool nomination_cancelled = false;
    // Nomination's create identity. Meaningless for Probe/Session, which have
    // no ladder to confuse a response with; see the generation section above.
    TalkbackGenerationState generation;
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
// any -- the pure decision behind nomination_reset() (and, until Task 3,
// session_stop()'s early branch). Deliberately does NOT clear `state.owner`: the
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
// expire_stale_pending_create_locked() (which only ever sees a stale
// Nomination) and behind tick()'s AwaitingChannel timeout. It was also
// session_stop()'s main teardown until Task 3. Probe has no cancellation flag and no ladder, so
// for Probe this is exactly "release the owner", which is all that timeout
// needs -- it stores this transition rather than writing the field so that
// the invariant "every mutation is a whole-state store of a transition's
// result" has no exceptions to remember. Clears the owner AND that
// owner's own cancellation flag TOGETHER: this symmetry is the entire fix for
// N1. For Nomination it ALSO bumps the generation, so the abandoned create's
// eventual response is not confused with whatever the next ladder issues --
// one transition rather than a clear here and a bump on the line after it,
// for the reason on TalkbackCreateState. A no-op call (owner already None)
// returns `state` unchanged.
inline TalkbackCreateState talkback_expire(TalkbackCreateState state)
{
    if (state.owner == TalkbackChannelOwner::Session) {
        state.session_cancelled = false;
    } else if (state.owner == TalkbackChannelOwner::Nomination) {
        state.nomination_cancelled = false;
        state.generation = talkback_generation_bump(state.generation);
    }
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
// step -- the pure decision behind the check-and-clear that
// talkback_create_response() performs before any owner branch runs
// (`cancelled = m_*_create_cancelled; m_*_create_cancelled = false;` in the
// shape it had when it still lived in those branches). Fix round 2 routed
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

// ── The two whole transitions the engine actually performs (fix round 5) ────
//
// Everything above is a piece of a transition. These two ARE the transitions,
// and engine-talkback.cpp calls nothing else on this state: it reads the
// state out under m_chan_mtx, calls one of these, and stores the returned
// state back as a single assignment.
//
// That is the entire point, and it is worth stating plainly because the
// previous shape looked fine. Round 4 had the engine perform "claim the
// arbiter" and "stamp the generation" as two adjacent assignments in one lock
// scope, and "release the arbiter", "check-and-clear the cancellation flag"
// and "judge the generation" as three. A re-review mutation deleted ONE of
// those assignments -- the response-side generation write-back -- and the
// whole 64-test suite stayed green, because the composition lived in the
// engine where no test can see it while the pieces lived here where every
// test can. Composing them here moves the composition into the layer the
// tests reach; leaving the engine one indivisible store means the mutation
// that survived cannot be written at all, rather than being written and
// caught.
//
// The engine still owns everything that is not a state transition: the SDK
// calls, the deadlines, the reports, the queue and table bookkeeping.

// A CreateChannel has just returned SDKERR_SUCCESS: claim the arbiter for
// `owner` and, for Nomination, stamp the create with the current generation.
// One operation, so a caller cannot claim without stamping -- the two used to
// be separate lines in nomination_create_next() and either could be deleted
// on its own.
//
// Caller must have found the gate open (talkback_may_request_create) in the
// SAME lock scope it calls this from; this function does not re-check,
// because a claim that loses a race has no safe local answer -- the SDK call
// has already happened by then.
inline TalkbackCreateState talkback_create_issued(TalkbackCreateState state,
                                                  TalkbackChannelOwner owner)
{
    state.owner = owner;
    if (owner == TalkbackChannelOwner::Nomination)
        state.generation = talkback_generation_issue(state.generation);
    return state;
}

// A create response has arrived: attribute it, release the arbiter,
// check-and-clear that owner's cancellation flag, and judge its generation --
// in one indivisible step, because every regression this feature has produced
// came from one of those four escaping. Round 5's Major (F1) was the newest:
// the Stale branch returned BEFORE the check-and-clear, orphaning
// the nomination cancellation flag, which then destroyed the next nomination's
// first channel and left already_provisioned set for the rest of the meeting.
// A branch cannot skip what the caller has already done before any branch
// runs.
//
// `cancelled` is that owner's OWN flag as it stood at claim time (Probe has
// none, so it reads false); feed it to talkback_create_disposition().
// `freshness` is NotNomination for every owner but Nomination.
struct TalkbackCreateResponse {
    TalkbackChannelOwner      owner;
    bool                      cancelled;
    TalkbackResponseFreshness freshness;
    TalkbackCreateState       next;
};
inline TalkbackCreateResponse talkback_create_response(TalkbackCreateState state)
{
    const TalkbackChannelOwner owner = talkback_claim_create(state.owner);

    const TalkbackCreateCheckResult check = talkback_check_and_clear_cancelled(state, owner);
    state = check.next;

    const TalkbackResponseCheck fresh = talkback_generation_on_response(state.generation, owner);
    state.generation = fresh.next;

    // Release LAST, so the two reads above see the state as it was at claim
    // time -- and unconditionally for a real owner, with no branch left that
    // could re-claim it (round 3's Stale branch did, and this file's THREADING
    // section documents "claims-then-clears regardless of what the branch
    // does" as load-bearing).
    if (owner != TalkbackChannelOwner::None) state.owner = TalkbackChannelOwner::None;

    return TalkbackCreateResponse{owner, check.cancelled, fresh.freshness, state};
}

// A fresh nomination ladder is starting: everything issued before now is
// superseded. Refuses to bump while a create is still outstanding, because
// bumping then is precisely how a response for a create the ladder still owns
// becomes judgeable as Stale -- that manufactured state was half of round 5's
// Major (nominate() bumped with no arbiter check, reachable through
// nominate -> leave -> nominate inside the create deadline). nominate() also
// refuses in that case; this is the half that cannot be forgotten at a call
// site.
inline TalkbackCreateState talkback_new_ladder(TalkbackCreateState state)
{
    if (state.owner != TalkbackChannelOwner::None) return state;
    state.generation = talkback_generation_bump(state.generation);
    return state;
}
