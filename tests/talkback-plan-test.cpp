// tests/talkback-plan-test.cpp
// How nominated talent maps onto Zoom's 16-channel / 10-user budget.
//
// Two SDK limits drive everything here and neither degrades gracefully:
// CreateChannel refuses past 16 channels, and a channel refuses past 10
// members. So the arithmetic has to be decided up front, not discovered when
// an invite fails in front of an audience.
//
// The rule this pins hardest: when the budget runs out, the people who did
// not get a private channel are NAMED. Silently dropping one means a director
// keys "talk to Sarah", hears their own cue, speaks -- and Sarah never had a
// channel. That failure is invisible from the control room, which is exactly
// why it is a test and not a comment.
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

static std::vector<std::string> names(int n)
{
    std::vector<std::string> v;
    for (int i = 0; i < n; ++i) v.push_back("Talent " + std::to_string(i + 1));
    return v;
}

static uint32_t private_count(const TalkbackPlan &p)
{
    uint32_t n = 0;
    for (const auto &c : p.channels) if (!c.is_all_talent) ++n;
    return n;
}

static uint32_t all_talent_count(const TalkbackPlan &p)
{
    uint32_t n = 0;
    for (const auto &c : p.channels) if (c.is_all_talent) ++n;
    return n;
}

int main()
{
    // ── Nobody nominated: nothing planned, nothing uncovered ───────────────
    {
        const TalkbackPlan p = talkback_plan({});
        check(p.channels.empty(), "an empty nomination produced channels");
        check(p.uncovered_private.empty(), "an empty nomination reported uncovered people");
        check(p.all_talent_complete, "an empty nomination was not considered complete");
    }

    // ── One nominee: one all-talent channel + one private ──────────────────
    {
        const TalkbackPlan p = talkback_plan({"Sarah Muller"});
        check(all_talent_count(p) == 1, "one nominee did not yield exactly one all-talent channel");
        check(private_count(p) == 1, "one nominee did not yield exactly one private channel");
        check(p.uncovered_private.empty(), "one nominee was reported uncovered");
    }

    // ── The 10-user cap: all-talent fans out at 11 ─────────────────────────
    {
        const TalkbackPlan ten = talkback_plan(names(10));
        check(all_talent_count(ten) == 1, "10 nominees needed more than one all-talent channel");
        const TalkbackPlan eleven = talkback_plan(names(11));
        check(all_talent_count(eleven) == 2,
              "11 nominees did not fan all-talent out to 2 channels -- the 10-user cap is hard");
        // No all-talent channel may exceed the cap.
        for (const auto &c : eleven.channels)
            if (c.is_all_talent)
                check(c.members.size() <= kTalkbackMaxUsersPerChannel,
                      "an all-talent channel exceeded the 10-user cap");
    }

    // ── Every nominee appears in exactly one all-talent channel ────────────
    {
        const TalkbackPlan p = talkback_plan(names(24));
        std::vector<std::string> seen;
        for (const auto &c : p.channels)
            if (c.is_all_talent)
                for (const auto &m : c.members) seen.push_back(m);
        check(seen.size() == 24,
              "the all-talent fan-out did not cover every nominee exactly once");
    }

    // ── A private channel holds exactly one person ─────────────────────────
    {
        const TalkbackPlan p = talkback_plan(names(5));
        for (const auto &c : p.channels)
            if (!c.is_all_talent)
                check(c.members.size() == 1,
                      "a private channel held more than one member -- private means one");
    }

    // ── The 16-channel budget is never exceeded ────────────────────────────
    {
        const TalkbackPlan p = talkback_plan(names(40));
        check(p.channels.size() <= kTalkbackMaxChannels,
              "the plan exceeded the 16-channel cap -- CreateChannel would fail live");
    }

    // ── Budget exhaustion NAMES the uncovered, never drops them silently ───
    // 24 nominees: 3 all-talent (ceil(24/10)) + 13 private = 16. So 11 people
    // get no private channel, and all 11 must be named.
    {
        const TalkbackPlan p = talkback_plan(names(24));
        check(all_talent_count(p) == 3, "24 nominees did not yield 3 all-talent channels");
        check(private_count(p) == 13, "24 nominees did not yield 13 private channels");
        check(p.channels.size() == 16, "the plan did not use the full 16-channel budget");
        check(p.uncovered_private.size() == 11,
              "the 11 nominees with no private channel were not all reported");
        // and the named ones must be real nominees, not invented
        for (const auto &n : p.uncovered_private) {
            bool known = false;
            for (const auto &c : names(24)) if (c == n) known = true;
            check(known, "an uncovered name was not one of the nominees");
        }
    }

    // ── All-talent is never sacrificed to fit privates ─────────────────────
    // Talking to everyone must survive the budget; private coverage is what
    // degrades. A director who cannot reach the whole panel has lost more
    // than one who cannot take somebody aside.
    {
        const TalkbackPlan p = talkback_plan(names(160));
        check(all_talent_count(p) == 16,
              "all-talent was truncated to make room for private channels");
        check(private_count(p) == 0, "privates were allocated with no budget left");
        check(p.uncovered_private.size() == 160,
              "every nominee should be reported uncovered when no private budget remains");
        check(p.all_talent_complete, "all-talent fit the budget but was reported incomplete");
    }

    // ── Beyond even all-talent's reach, say so ─────────────────────────────
    {
        const TalkbackPlan p = talkback_plan(names(200));   // needs 20 > 16
        check(!p.all_talent_complete,
              "a panel too large for even the all-talent fan-out was reported complete");
        check(p.channels.size() <= kTalkbackMaxChannels, "the cap was exceeded anyway");
    }

    // ── `unreachable` names exactly who is on no channel at all ────────────
    // At N=200 the all-talent fan-out (clamped to 16 channels x 10 = 160)
    // only covers "Talent 1".."Talent 160". "Talent 161".."Talent 200" (40
    // people) are in no all-talent slice AND, per the finding this pins,
    // uncovered_private alone cannot tell them apart from the other 160 who
    // are merely missing a private channel -- so unreachable must name them,
    // by value, not just by count (a count-only check would pass even if the
    // wrong 40 names were reported).
    {
        const TalkbackPlan p = talkback_plan(names(200));
        const std::vector<std::string> all200 = names(200);
        std::vector<std::string> expected_unreachable(all200.begin() + 160, all200.end());
        check(p.unreachable.size() == 40,
              "200 nominees did not yield exactly 40 unreachable people");
        check(p.unreachable == expected_unreachable,
              "unreachable did not name exactly Talent 161..Talent 200, in order");

        // Subset relationship: everyone unreachable must also appear in
        // uncovered_private (they still have no private channel either).
        for (const auto &u : p.unreachable) {
            bool also_in_uncovered_private = false;
            for (const auto &c : p.uncovered_private)
                if (c == u) { also_in_uncovered_private = true; break; }
            check(also_in_uncovered_private,
                  "an unreachable nominee was missing from uncovered_private -- not a subset");
        }
    }

    // ── `unreachable` is empty whenever all-talent fits the budget ─────────
    // N=160 is an exact fit (16 channels x 10) and N=24 is comfortably under
    // it; in both, all_talent_complete is true, so nobody should be reported
    // unreachable even though uncovered_private is large for N=160.
    {
        const TalkbackPlan p160 = talkback_plan(names(160));
        check(p160.unreachable.empty(),
              "160 nominees fit the all-talent fan-out but reported someone unreachable");
        const TalkbackPlan p24 = talkback_plan(names(24));
        check(p24.unreachable.empty(),
              "24 nominees fit the all-talent fan-out but reported someone unreachable");
    }

    // ── Duplicate names collapse, they do not consume two channels ─────────
    {
        const TalkbackPlan p = talkback_plan({"Sarah", "Sarah", "Luis"});
        check(private_count(p) == 2, "a duplicate nominee consumed a second private channel");
    }

    // ── Selecting at key time: which channels serve a target (Task 3) ──────
    //
    // This is the whole of the new key path. Keying used to CREATE a channel
    // and invite into it, which cost a create round trip plus an invite round
    // trip before any audio could flow -- measured live on 2026-08-25 as
    // buffers discarded on every press. Now a press is this lookup and a send,
    // so what this function decides is what the director is heard on.
    {
        const TalkbackPlan p = talkback_plan(names(24));

        // 24 nominees: 3 all-talent slices (ceil(24/10)) + 13 private.
        // Keying "all" must select ALL THREE. Selecting only the first is the
        // failure this fan-out exists to prevent: the director talks, the
        // first ten hear it, the other fourteen hear silence, and nothing
        // anywhere says so.
        uint32_t all_selected = 0;
        for (const auto &c : p.channels)
            if (talkback_channel_serves_target(c.is_all_talent, c.members,
                                               kTalkbackAllTalentTarget))
                ++all_selected;
        check(all_selected == 3,
              "keying the all-talent target did not select every all-talent channel");

        // Keying one person selects exactly their private channel -- not the
        // all-talent slice that also contains them. Selecting that slice would
        // put a private aside on air to nine other people.
        uint32_t sarah_selected = 0;
        bool selected_an_all_talent_slice = false;
        const std::string one = "Talent 7";   // in all-talent slice 1 and privately covered
        for (const auto &c : p.channels) {
            if (!talkback_channel_serves_target(c.is_all_talent, c.members, one)) continue;
            ++sarah_selected;
            if (c.is_all_talent) selected_an_all_talent_slice = true;
        }
        check(sarah_selected == 1, "keying one nominee did not select exactly one channel");
        check(!selected_an_all_talent_slice,
              "keying one nominee selected an all-talent channel -- a private aside would "
              "have gone out to everyone in that slice");

        // A name nobody nominated selects nothing, so session_start() refuses
        // rather than creating one on the spot -- creating on the key is the
        // behaviour this milestone removes.
        uint32_t stranger_selected = 0;
        for (const auto &c : p.channels)
            if (talkback_channel_serves_target(c.is_all_talent, c.members, "Nobody At All"))
                ++stranger_selected;
        check(stranger_selected == 0, "an unnominated target selected a channel");
    }

    // ── An 11th nominee is why the fan-out is not one channel ──────────────
    // The 10-user cap means all-talent is two channels at N=11, and both must
    // be selected by one press. This is the smallest case where "send to the
    // channel" and "send to the target" differ.
    {
        const TalkbackPlan p = talkback_plan(names(11));
        uint32_t all_selected = 0;
        for (const auto &c : p.channels)
            if (talkback_channel_serves_target(c.is_all_talent, c.members,
                                               kTalkbackAllTalentTarget))
                ++all_selected;
        check(all_selected == 2,
              "at 11 nominees the all-talent target did not select both of its channels");
    }

    // ── Task 5: what the plugin can refuse locally, before any round trip ──
    // talkback_target_known_unprovisioned() answers "known bad" from nothing
    // but the last nomination's reported plan -- see its header comment for
    // why a false negative (missing a case) is fine but a false positive
    // (refusing a target that would have worked) is not.
    {
        // Nothing ever nominated: both "all" and a name are unprovisioned.
        check(talkback_target_known_unprovisioned("all", {}, {}),
              "\"all\" was called provisioned with nothing ever nominated");
        check(talkback_target_known_unprovisioned("Bob", {}, {}),
              "a name was called provisioned with nothing ever nominated");
        // Case-insensitive sentinel match, mirroring talkback_target_is_all_talent.
        check(talkback_target_known_unprovisioned("ALL", {}, {}),
              "\"ALL\" was not recognised as the all-talent sentinel");
    }
    {
        // Nominated with room for private channels for everyone: "all" and
        // every nominee's own name are provisioned.
        const std::vector<std::string> requested = {"Bob", "Sue"};
        check(!talkback_target_known_unprovisioned("all", requested, {}),
              "\"all\" was refused although someone was nominated");
        check(!talkback_target_known_unprovisioned("Bob", requested, {}),
              "a privately-covered nominee was refused");
        check(!talkback_target_known_unprovisioned("Sue", requested, {}),
              "a privately-covered nominee was refused");
        // A name that was never nominated at all is refused.
        check(talkback_target_known_unprovisioned("Stranger", requested, {}),
              "a name never nominated was not refused");
    }
    {
        // Budget exhausted for Sue's private channel (she is in
        // uncovered_private): her own name is refused, but "all" still isn't
        // -- the all-talent fan-out still reaches her.
        const std::vector<std::string> requested = {"Bob", "Sue"};
        const std::vector<std::string> uncovered = {"Sue"};
        check(talkback_target_known_unprovisioned("Sue", requested, uncovered),
              "a nominee with no private channel of her own was not refused");
        check(!talkback_target_known_unprovisioned("Bob", requested, uncovered),
              "Bob's own private channel was refused by Sue's shortfall");
        check(!talkback_target_known_unprovisioned("all", requested, uncovered),
              "\"all\" was refused even though the all-talent slice still exists");
    }

    if (failures == 0)
        std::cout << "talkback-plan: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
