// tests/talkback-create-state-test.cpp
//
// Pins the cancellation/ownership decisions engine-talkback.cpp makes around
// an outstanding CreateChannel: what session_stop()'s early branch and
// nomination_reset() do when they must tear down while a create is still in
// flight (talkback_cancel()), what expire_stale_pending_create_locked() does
// when a create's response never arrives at all (talkback_expire()), and
// what onCreateChannelResponse does with a response once its owner is known
// (talkback_create_disposition()). See src/talkback-channel-owner.h.
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
#include "talkback-channel-owner.h"

#include <cstdint>
#include <iostream>
#include <vector>

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

    // ── Generation tracking (fix round 3, "expire-path double create") ─────
    //
    // Cover: issuing stamps the current generation; bumping does not touch
    // the outstanding queue; a response pops FIFO and is Stale/Current
    // exactly when its popped generation does/doesn't match `current`; an
    // empty queue reads Unexpected, not Stale.
    {
        TalkbackGenerationState g;
        g = talkback_issue_create(g);
        check(g.outstanding.size() == 1 && g.outstanding.front() == 0,
              "talkback_issue_create() did not stamp generation 0 for the first create");
    }
    {
        TalkbackGenerationState g;
        g.current = 5;
        g = talkback_bump_generation(g);
        check(g.current == 6, "talkback_bump_generation() did not increment current");
        check(g.outstanding.empty(),
              "talkback_bump_generation() touched the outstanding queue -- an "
              "already-queued create's generation must survive a bump so its "
              "eventual response can still be recognized as stale");
    }
    {
        TalkbackGenerationState g;
        g.current = 3;
        const auto check_result = talkback_check_response_generation(g);
        check(check_result.freshness == TalkbackResponseFreshness::Unexpected,
              "an empty outstanding queue did not read Unexpected");
    }
    {
        TalkbackGenerationState g;
        g.current = 3;
        g.outstanding = {3};
        const auto check_result = talkback_check_response_generation(g);
        check(check_result.freshness == TalkbackResponseFreshness::Current,
              "a response matching the current generation was not Current");
        check(check_result.next.outstanding.empty(),
              "talkback_check_response_generation() did not pop the resolved entry");
    }
    {
        TalkbackGenerationState g;
        g.current = 4; // bumped since generation 3's create was issued
        g.outstanding = {3};
        const auto check_result = talkback_check_response_generation(g);
        check(check_result.freshness == TalkbackResponseFreshness::Stale,
              "a response for a superseded generation was not Stale");
    }

    // ── End-to-end: the exact "expire-path double create" sequence ─────────
    // nominate() -> create#1 issued (gen 0) -> Leave, no response -> >10s
    // later, EXPIRY bumps to gen 1 (create#1's entry stays queued) -> a
    // FRESH nominate() bumps to gen 2 and issues create#2 (gen 2 pushed) ->
    // create#1's stale response finally arrives: must read Stale, and MUST
    // NOT consume create#2's slot -- create#2's own response, arriving
    // after, must still read Current.
    {
        TalkbackGenerationState g;
        g = talkback_issue_create(g); // create#1, gen 0
        check(g.current == 0 && g.outstanding == std::vector<uint32_t>{0},
              "setup: create#1 was not issued under generation 0");

        g = talkback_bump_generation(g); // expiry gives up on create#1
        check(g.current == 1 && g.outstanding == std::vector<uint32_t>{0},
              "setup: expiry did not bump generation while leaving create#1 queued");

        g = talkback_bump_generation(g); // fresh nominate()
        g = talkback_issue_create(g);    // create#2, gen 2
        check(g.current == 2 && (g.outstanding == std::vector<uint32_t>{0, 2}),
              "setup: create#2 was not queued behind the still-unresolved create#1");

        // create#1's late response arrives first (FIFO) -- must be Stale,
        // and create#2's entry (gen 2) must still be waiting afterward.
        auto first = talkback_check_response_generation(g);
        check(first.freshness == TalkbackResponseFreshness::Stale,
              "expire-path double create: create#1's late response was not "
              "recognized as Stale -- it would have been adopted as the "
              "fresh nomination's channel 1 while create#2 was still "
              "genuinely in flight, and the ladder would then issue a "
              "THIRD create: two outstanding at once, the one thing the "
              "arbiter exists to prevent");
        check(first.next.outstanding == std::vector<uint32_t>{2},
              "expire-path double create: create#2's entry did not survive "
              "discarding create#1's stale response");

        // create#2's real response arrives next -- must be Current.
        auto second = talkback_check_response_generation(first.next);
        check(second.freshness == TalkbackResponseFreshness::Current,
              "expire-path double create: create#2's genuine response was "
              "not recognized as Current after the stale one was discarded "
              "-- discarding the wrong response would leave a legitimately "
              "created channel forever unclaimed");
        check(second.next.outstanding.empty(),
              "create#2's response did not fully drain the outstanding queue");
    }

    if (failures == 0)
        std::cout << "talkback-create-state: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
