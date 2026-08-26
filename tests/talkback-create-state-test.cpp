// tests/talkback-create-state-test.cpp
//
// Pins the cancellation/ownership decisions engine-talkback.cpp makes around
// an outstanding CreateChannel: what session_stop()'s early branch and
// nomination_reset() do when they must tear down while a create is still in
// flight (talkback_cancel()), what expire_stale_pending_create_locked() does
// when a create's response never arrives at all (talkback_expire()), and
// what onCreateChannelResponse does with a response once its owner is known
// (talkback_create_disposition()), and -- fix round 4 -- how a response is
// matched to the create it belongs to across expiries, swallowed responses
// and other owners' responses (talkback_generation_*). See
// src/talkback-channel-owner.h.
//
// Why this file exists, in one sentence: a Critical (C1, cancellation must
// outlive a create across Leave() rather than dropping it) was found and
// fixed by review-trace, not a test, and the very next fix round (round 1)
// introduced a Major regression (N1) in the exact same state machine --
// m_session_create_cancelled was cleared by the expiry path,
// m_nomination_create_cancelled was not, so a stale cancellation silently
// destroyed the NEXT nomination's first channel instead of adopting it. This
// file exists so that class of asymmetry is caught by ctest, not by a third
// review round.
//
// It did not go far enough. Fix round 3 added generation tracking to the
// header and tested it -- but the ENGINE-side wiring (which sites bump, which
// sites record an issued create, which responses update the state) stayed in
// engine-talkback.cpp, untested, and that wiring is where round 3's own
// Critical lived: a queue pushed on one path and popped on another, so one
// unmatched push wedged every later nomination permanently. The round-4
// cases below therefore drive the transitions in the ORDER THE ENGINE CALLS
// THEM, across whole ladders, including the two paths that leak (a create
// whose response never arrives, and a response claimed by another owner).
// Extraction that stops short of the failing dimension is worse than none:
// the next reader sees a tested state machine and trusts it.
#include "talkback-channel-owner.h"

#include <cstdint>
#include <iostream>

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
    // ── talkback_create_disposition(): each owner, with and without its ────
    // ── cancelled flag set ──────────────────────────────────────────────────
    check(talkback_create_disposition(TalkbackChannelOwner::Probe, false) ==
              TalkbackCreateDisposition::Claim,
          "an uncancelled Probe response was not disposed as Claim");
    check(talkback_create_disposition(TalkbackChannelOwner::Session, false) ==
              TalkbackCreateDisposition::Claim,
          "an uncancelled Session response was not disposed as Claim");
    check(talkback_create_disposition(TalkbackChannelOwner::Nomination, false) ==
              TalkbackCreateDisposition::Claim,
          "an uncancelled Nomination response was not disposed as Claim");

    check(talkback_create_disposition(TalkbackChannelOwner::Session, true) ==
              TalkbackCreateDisposition::DestroyCancelled,
          "a cancelled Session response was not disposed as DestroyCancelled");
    check(talkback_create_disposition(TalkbackChannelOwner::Nomination, true) ==
              TalkbackCreateDisposition::DestroyCancelled,
          "a cancelled Nomination response was not disposed as DestroyCancelled");
    // Probe has no cancellation flag of its own -- every real caller always
    // passes false for it (Probe's timeout disposition is tick()'s separate
    // AwaitingChannel mechanism) -- but the pure function does not special-
    // case Probe, so pin its documented, generic behaviour for a `true` it
    // should never actually receive: cancelled still wins.
    check(talkback_create_disposition(TalkbackChannelOwner::Probe, true) ==
              TalkbackCreateDisposition::DestroyCancelled,
          "cancelled=true did not win regardless of owner");

    // ── An ownerless response is a stray, regardless of `cancelled` ────────
    check(talkback_create_disposition(TalkbackChannelOwner::None, false) ==
              TalkbackCreateDisposition::Stray,
          "an ownerless response was not disposed as Stray");
    check(talkback_create_disposition(TalkbackChannelOwner::None, true) ==
              TalkbackCreateDisposition::Stray,
          "an ownerless response was adopted as cancelled instead of Stray -- "
          "there was nothing outstanding to cancel");

    // ── talkback_cancel(): only affects the owner that is actually pending ──
    {
        TalkbackCreateState s{TalkbackChannelOwner::Session, false, false};
        s = talkback_cancel(s, TalkbackChannelOwner::Session);
        check(s.owner == TalkbackChannelOwner::Session,
              "talkback_cancel() cleared the owner -- a cancelled create must "
              "stay claimed so its response is still routed here, not to the "
              "stray queue nothing drains");
        check(s.session_cancelled, "talkback_cancel() did not set session_cancelled");
        check(!s.nomination_cancelled,
              "talkback_cancel(..., Session) touched nomination_cancelled");
    }
    {
        TalkbackCreateState s{TalkbackChannelOwner::Nomination, false, false};
        s = talkback_cancel(s, TalkbackChannelOwner::Nomination);
        check(s.owner == TalkbackChannelOwner::Nomination,
              "talkback_cancel() cleared the owner for Nomination");
        check(s.nomination_cancelled, "talkback_cancel() did not set nomination_cancelled");
        check(!s.session_cancelled,
              "talkback_cancel(..., Nomination) touched session_cancelled");
    }
    // No-op when the named owner is not the one actually pending -- nothing
    // outstanding to cancel (e.g. session_stop() called with Nomination
    // pending, or vice versa).
    {
        TalkbackCreateState s{TalkbackChannelOwner::Probe, false, false};
        const TalkbackCreateState before = s;
        s = talkback_cancel(s, TalkbackChannelOwner::Session);
        check(s.owner == before.owner && s.session_cancelled == before.session_cancelled &&
                  s.nomination_cancelled == before.nomination_cancelled,
              "talkback_cancel() mutated state when the named owner was not pending");
    }

    // ── talkback_expire(): clears the owner AND that owner's own flag ──────
    {
        TalkbackCreateState s{TalkbackChannelOwner::Session, true, false};
        s = talkback_expire(s);
        check(s.owner == TalkbackChannelOwner::None,
              "talkback_expire() did not clear a pending Session owner");
        check(!s.session_cancelled,
              "talkback_expire() did not clear session_cancelled");
    }
    {
        // The N1 case, in isolation: a cancelled Nomination create that
        // expires must come out with its flag cleared, exactly like Session
        // above. Round 1's hand-written expire_stale_pending_create_locked()
        // cleared the queue in this arm but left this flag `true`.
        TalkbackCreateState s{TalkbackChannelOwner::Nomination, false, true};
        s = talkback_expire(s);
        check(s.owner == TalkbackChannelOwner::None,
              "talkback_expire() did not clear a pending Nomination owner");
        check(!s.nomination_cancelled,
              "N1: talkback_expire() did not clear nomination_cancelled -- a "
              "stale cancellation would outlive the create it was recorded "
              "for and silently destroy the NEXT create for the same owner");
    }
    // Expiring an uncancelled create clears the owner and leaves both flags
    // false -- unremarkable, but pins that expire() never SETS a flag, only
    // clears one.
    {
        TalkbackCreateState s{TalkbackChannelOwner::Nomination, false, false};
        s = talkback_expire(s);
        check(s.owner == TalkbackChannelOwner::None && !s.session_cancelled &&
                  !s.nomination_cancelled,
              "talkback_expire() on an uncancelled create left stray state set");
    }

    // ── End-to-end: the exact N1 regression sequence ────────────────────────
    // nominate() -> CreateChannel ok (claim Nomination) -> Leave before the
    // response is pumped (cancel Nomination) -> the response never arrives;
    // more than kAwaitTimeout later a FRESH nominate() re-arms the owner,
    // which first expires the stale one -- then the fresh create's own
    // response arrives and must be Claimed, not destroyed as cancelled.
    {
        TalkbackCreateState s{TalkbackChannelOwner::None, false, false};
        s.owner = TalkbackChannelOwner::Nomination;                  // claim (1st CreateChannel)
        s = talkback_cancel(s, TalkbackChannelOwner::Nomination);    // Leave, mid-create
        check(s.owner == TalkbackChannelOwner::Nomination && s.nomination_cancelled,
              "setup: the first create was not left claimed-and-cancelled");

        s = talkback_expire(s);                                      // stale-create timeout
        check(s.owner == TalkbackChannelOwner::None && !s.nomination_cancelled,
              "N1 setup: expiry left the cancelled flag set for the fresh "
              "nomination to inherit");

        s.owner = TalkbackChannelOwner::Nomination;                  // claim (2nd, FRESH CreateChannel)
        const bool cancelled_for_fresh_create = s.nomination_cancelled;
        check(!cancelled_for_fresh_create,
              "N1: the fresh nomination's create inherited the PREVIOUS "
              "create's cancellation flag");
        check(talkback_create_disposition(s.owner, cancelled_for_fresh_create) ==
                  TalkbackCreateDisposition::Claim,
              "N1: the fresh nomination's channel was disposed as "
              "DestroyCancelled instead of Claim -- this is exactly the "
              "regression: the operator's second nomination silently "
              "provisions zero channels");
    }
    // Symmetric check for Session, proving the fix is not one-sided again.
    {
        TalkbackCreateState s{TalkbackChannelOwner::None, false, false};
        s.owner = TalkbackChannelOwner::Session;
        s = talkback_cancel(s, TalkbackChannelOwner::Session);
        s = talkback_expire(s);
        s.owner = TalkbackChannelOwner::Session;
        check(talkback_create_disposition(s.owner, s.session_cancelled) ==
                  TalkbackCreateDisposition::Claim,
              "the Session arm regressed the same way N1 regressed Nomination");
    }

    // ── talkback_check_and_clear_cancelled(): reads AND clears in one step,
    // ── the RESPONSE-side clearer (fix round 3, item B) ─────────────────────
    {
        TalkbackCreateState s{TalkbackChannelOwner::Session, true, false};
        const auto result = talkback_check_and_clear_cancelled(s, TalkbackChannelOwner::Session);
        check(result.cancelled, "talkback_check_and_clear_cancelled() did not read a set flag");
        check(!result.next.session_cancelled,
              "talkback_check_and_clear_cancelled() did not clear session_cancelled");
        check(!result.next.nomination_cancelled,
              "talkback_check_and_clear_cancelled(..., Session) touched nomination_cancelled");
    }
    {
        TalkbackCreateState s{TalkbackChannelOwner::Nomination, false, true};
        const auto result = talkback_check_and_clear_cancelled(s, TalkbackChannelOwner::Nomination);
        check(result.cancelled,
              "talkback_check_and_clear_cancelled() did not read a set Nomination flag");
        check(!result.next.nomination_cancelled,
              "talkback_check_and_clear_cancelled() did not clear nomination_cancelled");
        check(!result.next.session_cancelled,
              "talkback_check_and_clear_cancelled(..., Nomination) touched session_cancelled");
    }
    {
        // Probe has no flag of its own -- reads false, clears nothing.
        TalkbackCreateState s{TalkbackChannelOwner::Probe, true, true};
        const auto result = talkback_check_and_clear_cancelled(s, TalkbackChannelOwner::Probe);
        check(!result.cancelled, "talkback_check_and_clear_cancelled(Probe) read a flag Probe has none of");
        check(result.next.session_cancelled && result.next.nomination_cancelled,
              "talkback_check_and_clear_cancelled(Probe) touched a flag it has no business reading");
    }

    // ── Generation tracking (fix round 3, rebuilt as a scalar in round 4) ──
    //
    // Round 3 carried this as a FIFO of outstanding generations and shipped a
    // CRITICAL with it: pushes happen on one path and pops on another, so one
    // unmatched push desynchronised it permanently and every later response
    // read Stale -- destroying its own freshly created channel, provisioning
    // zero, forever. These cases exist to make that unrepresentable: the
    // scalar transitions below are the ones the ENGINE calls, driven here in
    // the same order the engine calls them, INCLUDING the two paths round 3's
    // FIFO leaked on (a create whose response never arrives, and a response
    // claimed by some other owner).
    {
        TalkbackGenerationState g;
        g = talkback_generation_issue(g);
        check(g.outstanding && g.outstanding_generation == 0,
              "talkback_generation_issue() did not stamp generation 0 for the "
              "first create");
    }
    {
        TalkbackGenerationState g;
        g.current = 5;
        g.outstanding = true;
        g.outstanding_generation = 5;
        g = talkback_generation_bump(g);
        check(g.current == 6, "talkback_generation_bump() did not increment current");
        check(g.outstanding && g.outstanding_generation == 5,
              "talkback_generation_bump() cleared the outstanding stamp -- an "
              "abandoned create's stamp must survive the bump so ITS response "
              "is what reads Stale");
    }
    // Nothing outstanding -> Unexpected, state untouched. This is the FAIL
    // OPEN case: the engine treats it exactly like Current rather than
    // destroying a channel it cannot explain.
    {
        TalkbackGenerationState g;
        g.current = 3;
        const auto r = talkback_generation_on_response(g, TalkbackChannelOwner::Nomination);
        check(r.freshness == TalkbackResponseFreshness::Unexpected,
              "a response with nothing outstanding did not read Unexpected");
        check(!r.next.outstanding && r.next.current == 3,
              "an Unexpected response mutated the generation state");
    }
    {
        TalkbackGenerationState g;
        g.current = 3;
        g.outstanding = true;
        g.outstanding_generation = 3;
        const auto r = talkback_generation_on_response(g, TalkbackChannelOwner::Nomination);
        check(r.freshness == TalkbackResponseFreshness::Current,
              "a response matching the current generation was not Current");
        check(!r.next.outstanding,
              "a resolved response left a create recorded as still outstanding");
    }
    {
        TalkbackGenerationState g;
        g.current = 4; // bumped since generation 3's create was issued
        g.outstanding = true;
        g.outstanding_generation = 3;
        const auto r = talkback_generation_on_response(g, TalkbackChannelOwner::Nomination);
        check(r.freshness == TalkbackResponseFreshness::Stale,
              "a response for a superseded generation was not Stale");
        check(r.next.outstanding && r.next.outstanding_generation == 3 &&
                  r.next.current == 4,
              "a Stale response mutated the generation state -- it must destroy "
              "only the channel it names and change nothing, so a second late "
              "response for the same abandoned create reaches the same verdict");
    }

    // ── The round-3 CRITICAL, end-to-end: a create whose response NEVER ────
    // ── arrives must not poison the ladders that follow ─────────────────────
    //
    // The reachable sequence: nominate -> the response is swallowed -> a key
    // press runs the gate check, which expires it -> nominate again. Under
    // round 3's FIFO the first ladder's entry was pushed and never popped,
    // so ladder 2's REAL response popped ladder 1's entry, compared it
    // against the bumped `current`, read Stale, destroyed the channel Zoom
    // had just created, and provisioned zero -- and every later ladder did
    // the same, for the life of the process. Ten ladders in a row here: a
    // scalar cannot accumulate, so the tenth must behave exactly like the
    // first.
    {
        TalkbackGenerationState g;
        for (int ladder = 0; ladder < 10; ++ladder) {
            g = talkback_generation_bump(g);  // nominate()
            g = talkback_generation_issue(g); // CreateChannel #1: swallowed, never answered
            // The operator presses the talkback key; the gate check expires it.
            g = talkback_generation_bump(g);  // expire_stale_pending_create_locked()

            g = talkback_generation_bump(g);  // nominate() again
            g = talkback_generation_issue(g); // CreateChannel #2: this one WILL answer
            const auto r = talkback_generation_on_response(g, TalkbackChannelOwner::Nomination);
            check(r.freshness == TalkbackResponseFreshness::Current,
                  "swallowed-response ladder: the NEXT nomination's own real "
                  "create response was judged stale -- this is the round-3 "
                  "Critical: it destroys the channel Zoom just created, "
                  "provisions zero, and repeats for the life of the process");
            g = r.next;
            check(!g.outstanding,
                  "swallowed-response ladder: a resolved response left the "
                  "outstanding slot occupied, so the NEXT ladder's response "
                  "would be judged against a create that is already answered");
        }
    }

    // ── The other round-3 orphan path: a response claimed by some OTHER ────
    // ── owner must leave this state completely alone ────────────────────────
    //
    // After a Nomination expiry the owner is None until someone re-claims it,
    // and the other claimers are probe() and session_start(). Round 3 updated
    // the generation state only from inside onCreateChannelResponse's
    // Nomination branch, so a response landing under None/Probe/Session
    // skipped the pop and left the FIFO one entry off -- permanently. The
    // engine now calls talkback_generation_on_response() for EVERY response
    // and passes the owner, so this is a case the function answers rather
    // than one expressed by a call that isn't there.
    {
        const TalkbackChannelOwner others[] = {TalkbackChannelOwner::None,
                                               TalkbackChannelOwner::Probe,
                                               TalkbackChannelOwner::Session};
        for (const auto other : others) {
            TalkbackGenerationState g;
            g = talkback_generation_bump(g);
            g = talkback_generation_issue(g); // this ladder's create, genuinely in flight

            const auto foreign = talkback_generation_on_response(g, other);
            check(foreign.freshness == TalkbackResponseFreshness::NotNomination,
                  "a response claimed by another owner was judged as if it "
                  "were Nomination's");
            check(foreign.next.outstanding &&
                      foreign.next.outstanding_generation == g.outstanding_generation &&
                      foreign.next.current == g.current,
                  "a response claimed by another owner mutated the nomination "
                  "generation state -- that silent skip is exactly what "
                  "desynchronised round 3's queue");

            // Our own response, arriving after the foreign one, must still be
            // recognized as the one we are waiting for.
            const auto ours = talkback_generation_on_response(foreign.next,
                                                              TalkbackChannelOwner::Nomination);
            check(ours.freshness == TalkbackResponseFreshness::Current,
                  "after a response for another owner passed through, this "
                  "ladder's own create response was no longer recognized as "
                  "current");
        }
    }

    // ── The accepted residual, pinned so it is a decision and not a ────────
    // ── surprise ────────────────────────────────────────────────────────────
    //
    // create A expires; a later ladder issues create B, which overwrites the
    // one outstanding slot; A's response (if it ever arrives, with the owner
    // re-claimed by B) is then indistinguishable from B's and is adopted as
    // B's. No scheme can do better -- Zoom provides no correlation id -- and
    // failing OPEN here costs at most one extra create in flight, whose
    // response finds the plan queue empty and is destroyed down the
    // channel_untracked path. Failing CLOSED instead is what wedged round 3.
    {
        TalkbackGenerationState g;
        g = talkback_generation_bump(g);
        g = talkback_generation_issue(g); // create A
        g = talkback_generation_bump(g);  // expiry abandons A
        g = talkback_generation_bump(g);  // fresh nominate()
        g = talkback_generation_issue(g); // create B overwrites the slot
        const auto r = talkback_generation_on_response(g, TalkbackChannelOwner::Nomination);
        check(r.freshness == TalkbackResponseFreshness::Current,
              "the accepted residual changed shape: a response arriving while "
              "a genuine create is outstanding must be adopted (fail open), "
              "never destroyed");
    }
    // ...but between the expiry and the next issue, the abandoned create's
    // own stamp is still what a response is judged against, so it destroys
    // the orphan rather than adopting it.
    {
        TalkbackGenerationState g;
        g = talkback_generation_bump(g);
        g = talkback_generation_issue(g); // create A
        g = talkback_generation_bump(g);  // expiry abandons A; nothing issued since
        const auto r = talkback_generation_on_response(g, TalkbackChannelOwner::Nomination);
        check(r.freshness == TalkbackResponseFreshness::Stale,
              "an abandoned create's response, with nothing issued since, was "
              "not recognized as stale");
    }

    if (failures == 0)
        std::cout << "talkback-create-state: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
