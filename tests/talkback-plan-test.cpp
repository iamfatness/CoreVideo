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

    // ── Duplicate names collapse, they do not consume two channels ─────────
    {
        const TalkbackPlan p = talkback_plan({"Sarah", "Sarah", "Luis"});
        check(private_count(p) == 2, "a duplicate nominee consumed a second private channel");
    }

    if (failures == 0)
        std::cout << "talkback-plan: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
