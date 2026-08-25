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

    if (failures == 0)
        std::cout << "talkback-create-state: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
