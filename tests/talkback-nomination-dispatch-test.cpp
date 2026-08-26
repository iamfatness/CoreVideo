// tests/talkback-nomination-dispatch-test.cpp
//
// Task 5 fix round 2 (N5): pins the WIRING, not just the pure state machine.
//
// The re-review found that neither F1 nor N1 was ever a bug in
// src/talkback-nomination.h's pure transitions -- both were the CALLER
// picking the wrong transition for a given report shape. Round 1's mutation
// tests (tests/talkback-nomination-test.cpp) proved the transitions
// themselves are correct but could not reach that caller, because it lived
// inlined inside ZoomEngineClient::handle_event(), which needs the whole OBS
// plugin to compile. Proof: re-routing the refusal branch to
// talkback_nomination_commit() instead of talkback_nomination_note_refused()
// -- F1, reintroduced verbatim, in that exact mapping -- left the full
// 66-test suite green.
//
// src/talkback-nomination-dispatch.h factors that mapping out to depend on
// nothing but Qt Core's JSON types, so it can be driven here with the exact
// report shapes engine/src/engine-talkback.cpp's report_nomination() emits.
#include "talkback-nomination-dispatch.h"

#include <QJsonObject>

#include <iostream>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static bool contains(const std::vector<std::string> &v, const std::string &s)
{
    for (const auto &e : v) if (e == s) return true;
    return false;
}

int main()
{
    // ── A full successful ladder, driven entirely through report shapes ────
    // exactly as engine-talkback.cpp's nominate()/report_nomination() emit
    // them: uncovered_private*, unreachable*, plan, nominate_done.
    {
        TalkbackNominationPending pending;
        TalkbackNominationPlan confirmed;
        talkback_nomination_begin(pending, {"Sarah", "Luis", "Ana"}, 1);

        talkback_nomination_apply_report(confirmed, pending, "uncovered_private",
            QJsonObject{{"stage", "uncovered_private"}, {"name", "Ana"}});
        talkback_nomination_apply_report(confirmed, pending, "unreachable",
            QJsonObject{{"stage", "unreachable"}, {"name", "Ana"}});
        talkback_nomination_apply_report(confirmed, pending, "plan",
            QJsonObject{{"stage", "plan"}, {"channels", 3}, {"all_talent_complete", true}});
        talkback_nomination_apply_report(confirmed, pending, "nominate_done",
            QJsonObject{{"stage", "nominate_done"}, {"channels", 3}});

        check(confirmed.done, "a full report sequence did not commit");
        check(confirmed.channels == 3, "commit did not take nominate_done's channel count");
        check(confirmed.all_talent_complete, "commit lost all_talent_complete from the plan stage");
        check(confirmed.requested == std::vector<std::string>({"Sarah", "Luis", "Ana"}),
              "commit did not carry the requested list through");
        check(contains(confirmed.uncovered_private, "Ana"),
              "an uncovered_private report was not staged");
        check(contains(confirmed.unreachable, "Ana"),
              "an unreachable report was not staged");
        check(confirmed.last_attempt_ok, "a successful ladder left last_attempt_ok false");
    }

    // ── Reason-string collision: "create_busy" means two different things ──
    // depending ONLY on "channels_destroyed" -- nominate()'s own early gate
    // (no destroy yet) vs. nomination_create_next()'s mid-ladder gate (this
    // ladder's partial channels, or the replaced set, already destroyed).
    // A committed baseline plan must survive the first and be wiped by the
    // second, with the IDENTICAL reason string in both reports.
    auto committed_baseline = []() {
        TalkbackNominationPending pending;
        TalkbackNominationPlan confirmed;
        talkback_nomination_begin(pending, {"Sarah", "Luis"}, 1);
        talkback_nomination_apply_report(confirmed, pending, "plan",
            QJsonObject{{"stage", "plan"}, {"channels", 3}, {"all_talent_complete", true}});
        talkback_nomination_apply_report(confirmed, pending, "nominate_done",
            QJsonObject{{"stage", "nominate_done"}, {"channels", 3}});
        return confirmed;
    };

    // "create_busy" WITHOUT channels_destroyed -- nominate()'s own early
    // gate check (engine-talkback.cpp:1589-1592). The standing set is
    // untouched; a re-nomination attempt ("Ana","Tom") was refused before
    // anything was destroyed.
    {
        TalkbackNominationPlan confirmed = committed_baseline();
        TalkbackNominationPending pending;
        talkback_nomination_begin(pending, {"Ana", "Tom"}, 1);

        talkback_nomination_apply_report(confirmed, pending, "nominate",
            QJsonObject{{"stage", "nominate"}, {"ok", false}, {"reason", "create_busy"}});

        check(confirmed.requested == std::vector<std::string>({"Sarah", "Luis"}),
              "an early create_busy refusal (no channels_destroyed) overwrote the confirmed plan");
        check(confirmed.done, "an early refusal cleared 'done' on the still-standing plan");
        check(!confirmed.last_attempt_ok, "an early refusal did not record last_attempt_ok == false");
        check(confirmed.last_attempt_reason == "create_busy",
              "an early refusal did not record its reason");
    }

    // "create_busy" WITH channels_destroyed:true -- nomination_create_next()'s
    // mid-ladder gate (engine-talkback.cpp:1704-1720). The IDENTICAL reason
    // string, but the standing set (whatever this ladder or the replace step
    // destroyed) is gone. This is N1: the fix must key off the explicit
    // field, never the reason string.
    {
        TalkbackNominationPlan confirmed = committed_baseline();
        TalkbackNominationPending pending;
        talkback_nomination_begin(pending, {"Ana", "Tom"}, 1);

        talkback_nomination_apply_report(confirmed, pending, "nominate",
            QJsonObject{{"stage", "nominate"}, {"ok", false},
                        {"reason", "create_busy"}, {"channels_destroyed", true}});

        check(confirmed.requested.empty(),
              "N1: channels_destroyed:true did not clear the confirmed requested list");
        check(!confirmed.done,
              "N1: channels_destroyed:true left the destroyed plan marked done");
        check(confirmed.channels == 0,
              "N1: channels_destroyed:true left a stale channel count");
        check(!confirmed.last_attempt_ok,
              "N1: the failure was not recorded as last_attempt_ok == false");
        check(confirmed.last_attempt_reason == "create_busy",
              "N1: the failure reason was lost");
    }

    // "create_channel_failed" WITH channels_destroyed:true -- the OTHER N1
    // abort branch (CreateChannel itself returning non-SUCCESS). Same
    // requirement, different reason string, confirming the mapping keys off
    // the field and not a hardcoded reason match.
    {
        TalkbackNominationPlan confirmed = committed_baseline();
        TalkbackNominationPending pending;
        talkback_nomination_begin(pending, {"Ana"}, 1);

        talkback_nomination_apply_report(confirmed, pending, "nominate",
            QJsonObject{{"stage", "nominate"}, {"ok", false},
                        {"reason", "create_channel_failed"}, {"channels_destroyed", true}});

        check(confirmed.requested.empty(),
              "N1 (create_channel_failed): confirmed plan was not reset");
        check(confirmed.last_attempt_reason == "create_channel_failed",
              "N1 (create_channel_failed): reason was lost");
    }

    // ── Unrecognised / diagnostic-only stages are no-ops here ──────────────
    // "replacing" and "create_channel" are logged verbatim by the caller
    // (handle_event()) before this dispatcher ever runs, but carry nothing
    // this state machine acts on.
    {
        TalkbackNominationPlan confirmed = committed_baseline();
        TalkbackNominationPending pending;
        talkback_nomination_begin(pending, {"Ana"}, 1);
        const TalkbackNominationPlan before = confirmed;

        talkback_nomination_apply_report(confirmed, pending, "replacing",
            QJsonObject{{"stage", "replacing"}, {"channels", 3}});
        talkback_nomination_apply_report(confirmed, pending, "create_channel",
            QJsonObject{{"stage", "create_channel"}, {"code", 1}});

        check(confirmed.requested == before.requested &&
              confirmed.done == before.done &&
              confirmed.last_attempt_ok == before.last_attempt_ok,
              "a diagnostic-only stage mutated the confirmed plan");
    }

    // ── C1 (CRITICAL, final whole-branch review): a terminal report for a
    // SUPERSEDED attempt must never commit the staged attempt's list ───────
    //
    // The exact interleaving the review proved: nominate A while nothing is
    // running (ladder A starts, seconds long), nominate B mid-ladder (the
    // engine refuses it with "create_busy" and leaves ladder A alone -- its
    // documented, correct behaviour), then ladder A finishes. Before the
    // attempt id, B's begin() had wiped A's staging and A's nominate_done
    // committed B's ["Dave"] against A's three channels: Sarah's standing
    // channel refused locally, Dave's non-existent one passing the
    // pre-check, and A's shortfall names gone from talkback_status.
    {
        TalkbackNominationPending pending;
        TalkbackNominationPlan confirmed;

        // Attempt 1 -- ladder A, in flight, with a real shortfall reported.
        talkback_nomination_begin(pending, {"Sarah", "Tom"}, 1);
        talkback_nomination_apply_report(confirmed, pending, "plan",
            QJsonObject{{"stage", "plan"}, {"channels", 3},
                        {"all_talent_complete", true}, {"attempt", 1}});

        // Attempt 2 -- sent mid-ladder, staged over attempt 1's slot, and
        // refused by the engine WITHOUT destroying anything.
        talkback_nomination_begin(pending, {"Dave"}, 2);
        talkback_nomination_apply_report(confirmed, pending, "nominate",
            QJsonObject{{"stage", "nominate"}, {"ok", false},
                        {"reason", "create_busy"}, {"attempt", 2}});

        // A late stage line from ladder A must not pollute attempt 2's
        // staging either -- reports for a superseded attempt are inert.
        talkback_nomination_apply_report(confirmed, pending, "uncovered_private",
            QJsonObject{{"stage", "uncovered_private"}, {"name", "Tom"}, {"attempt", 1}});
        check(pending.uncovered_private.empty(),
              "C1: a stage report from a SUPERSEDED attempt was staged into "
              "the current attempt's slot");

        // Ladder A completes. This is the commit that used to write ["Dave"].
        talkback_nomination_apply_report(confirmed, pending, "nominate_done",
            QJsonObject{{"stage", "nominate_done"}, {"channels", 3}, {"attempt", 1}});

        check(confirmed.requested != std::vector<std::string>({"Dave"}),
              "C1: A SUPERSEDED LADDER'S nominate_done COMMITTED THE NEWER "
              "ATTEMPT'S NOMINEE LIST -- key_on() would now refuse a standing "
              "channel and pass an unprovisioned name");
        check(confirmed.requested.empty() && !confirmed.done,
              "C1: a superseded nominate_done must leave NOTHING confirmed "
              "(fail closed: the ladder it superseded destroyed whatever the "
              "old record described, and this plugin cannot reconstruct what "
              "the completed one built)");
        check(!confirmed.last_attempt_ok && !confirmed.last_attempt_reason.empty(),
              "C1: a superseded terminal left no diagnostic for the operator");
    }

    // The matching attempt still commits -- the id check must not turn every
    // report into a no-op (the other direction of the same mutation).
    {
        TalkbackNominationPending pending;
        TalkbackNominationPlan confirmed;
        talkback_nomination_begin(pending, {"Sarah"}, 7);
        talkback_nomination_apply_report(confirmed, pending, "plan",
            QJsonObject{{"stage", "plan"}, {"channels", 2},
                        {"all_talent_complete", true}, {"attempt", 7}});
        talkback_nomination_apply_report(confirmed, pending, "nominate_done",
            QJsonObject{{"stage", "nominate_done"}, {"channels", 2}, {"attempt", 7}});

        check(confirmed.done && confirmed.channels == 2 &&
              confirmed.requested == std::vector<std::string>({"Sarah"}),
              "C1: a report whose attempt MATCHES the staged one did not commit");
    }

    // MIXED VERSIONS: an engine that predates the attempt id emits no
    // "attempt" field at all, and a DLL-only install (CLAUDE.md's canonical
    // mistake) is exactly how that pairing reaches an operator. A report with
    // no attempt is treated as matching -- the pre-C1 behaviour, which is
    // right for a peer that only ever runs one attempt's reports past us in
    // order anyway.
    {
        TalkbackNominationPending pending;
        TalkbackNominationPlan confirmed;
        talkback_nomination_begin(pending, {"Sarah"}, 5);
        talkback_nomination_apply_report(confirmed, pending, "plan",
            QJsonObject{{"stage", "plan"}, {"channels", 2}, {"all_talent_complete", true}});
        talkback_nomination_apply_report(confirmed, pending, "nominate_done",
            QJsonObject{{"stage", "nominate_done"}, {"channels", 2}});

        check(confirmed.done && confirmed.requested == std::vector<std::string>({"Sarah"}),
              "C1: a report from an OLD ENGINE (no \"attempt\" field) was "
              "treated as superseded -- a mixed-version pair must still be "
              "able to confirm a nomination");
    }

    // A superseded terminal that DID destroy channels still has to invalidate
    // the confirmed plan: the channels are gone whichever attempt owned them.
    {
        TalkbackNominationPlan confirmed = committed_baseline();
        TalkbackNominationPending pending;
        talkback_nomination_begin(pending, {"Ana"}, 9);

        talkback_nomination_apply_report(confirmed, pending, "nominate",
            QJsonObject{{"stage", "nominate"}, {"ok", false},
                        {"reason", "channel_failed"},
                        {"channels_destroyed", true}, {"attempt", 8}});

        check(confirmed.requested.empty() && !confirmed.done,
              "C1: a SUPERSEDED abort carrying channels_destroyed:true left "
              "the confirmed plan describing channels the engine has destroyed");
    }

    if (failures == 0)
        std::cout << "talkback-nomination-dispatch: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
