// tests/talkback-nomination-test.cpp
//
// Task 5 fix round 1 (F1, F2): pins the plugin's local nomination record
// against the exact two review-caught defects.
//
// F1 -- a refused re-nomination must NOT poison the confirmed plan. The
// engine leaves the standing channel set untouched on every refusal path
// (engine/src/engine-talkback.cpp's nominate(), the arbiter gate's own
// comment), so the plugin's record of who has a channel must not move
// either, or key_on() falsely refuses a target whose channel is still
// standing -- on the operator's own mistake-recovery path, under pressure,
// mid-show.
//
// F2 -- the confirmed plan must be reset at the same lifecycle points the
// engine wipes its own channel table (Leave, engine restart), or
// talkback_status keeps advertising a plan that no longer exists and the
// pre-check keeps passing for targets that will now refuse with
// "no_nomination".
#include "talkback-nomination.h"
#include "talkback-plan.h"

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
    // ── Normal flow: begin -> stage reports -> commit ──────────────────────
    {
        TalkbackNominationPending pending;
        TalkbackNominationPlan confirmed;
        talkback_nomination_begin(pending, {"Sarah", "Luis"});
        talkback_nomination_note_plan(pending, /*channels=*/3, /*all_talent_complete=*/true);
        // Neither Sarah nor Luis is uncovered in this scenario.
        talkback_nomination_commit(confirmed, pending, /*final_channels=*/3);

        check(confirmed.done, "a committed plan was not marked done");
        check(confirmed.channels == 3, "commit did not record the final channel count");
        check(confirmed.all_talent_complete, "commit lost all_talent_complete");
        check(confirmed.requested == pending.requested, "commit did not copy the requested list");
        check(confirmed.uncovered_private.empty(), "commit invented an uncovered nominee");
        check(confirmed.last_attempt_ok, "a successful commit reported last_attempt_ok == false");
        check(confirmed.last_attempt_reason.empty(),
              "a successful commit left a stale last_attempt_reason");
        check(!talkback_target_known_unprovisioned("Sarah", confirmed.requested,
                                                    confirmed.uncovered_private),
              "a privately-covered, committed nominee was refused");
    }

    // ── F1: the exact review scenario ───────────────────────────────────────
    // Nominate {Sarah, Luis}; channels stand (committed). Operator later
    // re-nominates {Ana, Tom} while a key is open (or a ladder is still
    // creating, or a name collides with "all") -- the engine refuses
    // outright. Sarah's and Luis's channels are UNTOUCHED. A key for
    // "Sarah" must still be locally allowed.
    {
        TalkbackNominationPending pending;
        TalkbackNominationPlan confirmed;

        // First nomination: accepted.
        talkback_nomination_begin(pending, {"Sarah", "Luis"});
        talkback_nomination_note_plan(pending, 3, true);
        talkback_nomination_commit(confirmed, pending, 3);

        // Second nomination: sent, then refused (session_live, probe_busy,
        // create_busy, target_name_collision, ... -- any of the seven).
        talkback_nomination_begin(pending, {"Ana", "Tom"});
        talkback_nomination_note_refused(confirmed, "session_live");

        check(confirmed.requested == std::vector<std::string>({"Sarah", "Luis"}),
              "F1 regression: a refused re-nomination overwrote the confirmed requested list");
        check(confirmed.uncovered_private.empty(),
              "F1 regression: a refused re-nomination touched uncovered_private");
        check(!confirmed.last_attempt_ok,
              "a refused attempt did not record last_attempt_ok == false");
        check(confirmed.last_attempt_reason == "session_live",
              "a refused attempt did not record its reason");

        // The whole point: key_on() must not refuse Sarah.
        check(!talkback_target_known_unprovisioned("Sarah", confirmed.requested,
                                                    confirmed.uncovered_private),
              "F1: a refused re-nomination caused a FALSE REFUSAL of a still-standing channel");
        check(!talkback_target_known_unprovisioned("Luis", confirmed.requested,
                                                    confirmed.uncovered_private),
              "F1: a refused re-nomination caused a FALSE REFUSAL of a still-standing channel");
        // Ana/Tom were never confirmed -- correctly still refused.
        check(talkback_target_known_unprovisioned("Ana", confirmed.requested,
                                                   confirmed.uncovered_private),
              "an attempt that was refused was treated as though it provisioned its nominees");
    }

    // ── F1 corollary: a refused FIRST-EVER nomination leaves "nothing ──────
    // confirmed", which is the same as a fresh meeting -- not a plan with
    // Ana/Tom's names half-recorded.
    {
        TalkbackNominationPending pending;
        TalkbackNominationPlan confirmed; // never committed
        talkback_nomination_begin(pending, {"Ana", "Tom"});
        talkback_nomination_note_refused(confirmed, "not_in_meeting");

        check(!confirmed.done, "a refused first-ever nomination was marked done");
        check(confirmed.requested.empty(),
              "a refused first-ever nomination populated the confirmed requested list");
        check(talkback_target_known_unprovisioned("all", confirmed.requested,
                                                   confirmed.uncovered_private),
              "\"all\" was provisioned after the only nomination attempt was refused");
    }

    // ── F2: reset at a world-reset (Leave / engine restart) ────────────────
    {
        TalkbackNominationPending pending;
        TalkbackNominationPlan confirmed;
        talkback_nomination_begin(pending, {"Sarah", "Luis"});
        talkback_nomination_note_plan(pending, 3, true);
        talkback_nomination_commit(confirmed, pending, 3);
        check(confirmed.done && confirmed.channels == 3,
              "setup for the F2 test did not produce a confirmed plan");

        talkback_nomination_reset(confirmed);

        check(!confirmed.done, "F2 regression: reset left a confirmed plan marked done");
        check(confirmed.channels == 0, "F2 regression: reset left a stale channel count");
        check(confirmed.requested.empty(), "F2 regression: reset left a stale requested list");
        check(confirmed.uncovered_private.empty(),
              "F2 regression: reset left stale uncovered_private");
        check(confirmed.last_attempt_ok, "reset did not clear a stale refusal flag");
        check(confirmed.last_attempt_reason.empty(), "reset did not clear a stale refusal reason");
        // The pre-check must now refuse everyone -- exactly a fresh meeting.
        check(talkback_target_known_unprovisioned("all", confirmed.requested,
                                                   confirmed.uncovered_private),
              "F2: \"all\" still reads as provisioned after a world-reset");
        check(talkback_target_known_unprovisioned("Sarah", confirmed.requested,
                                                   confirmed.uncovered_private),
              "F2: a name from the destroyed plan still reads as provisioned after a world-reset");
    }

    // ── F4: duplicate nominees must not inflate the recorded plan ──────────
    {
        TalkbackNominationPending pending;
        TalkbackNominationPlan confirmed;
        const std::vector<std::string> deduped =
            talkback_dedup_preserve_order({"Bob", "Bob", "Sue"});
        talkback_nomination_begin(pending, deduped);
        talkback_nomination_note_plan(pending, 3, true); // 1 all-talent + Bob + Sue
        talkback_nomination_commit(confirmed, pending, 3);
        check(confirmed.requested.size() == 2,
              "a duplicate nominee was still present after commit -- dedup did not happen "
              "before talkback_nomination_begin()");
        check(!contains(confirmed.uncovered_private, "Bob") &&
              !contains(confirmed.uncovered_private, "Sue"),
              "deduped nominees were incorrectly reported uncovered");
    }

    if (failures == 0)
        std::cout << "talkback-nomination: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
