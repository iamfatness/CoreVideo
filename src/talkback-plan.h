#pragma once
//
// talkback-plan.h — how nominated talent maps onto Zoom's channel budget.
//
// Two SDK limits drive this, and neither degrades gracefully: CreateChannel
// refuses past 16 channels, and a channel refuses past 10 members. So the
// arithmetic is decided up front rather than discovered when an invite fails
// in front of an audience.
//
// WHY PRE-PROVISION AT ALL. Creating a channel on the key press costs a
// create round-trip plus an invite round-trip before any audio can flow --
// measured live on 2026-08-25 as buffers discarded on every press
// (no_channel_drops). The director speaks and the first words are gone. So
// channels are created at NOMINATION time and keying only selects one.
//
// WHAT DEGRADES WHEN THE BUDGET RUNS OUT. All-talent is allocated first and
// never sacrificed: a director who cannot reach the whole panel has lost more
// than one who cannot take somebody aside. Private channels take what is
// left, and everyone who does not get one is NAMED in uncovered_private --
// never silently dropped, because that failure is invisible from the control
// room.
//
// Free of Qt / OBS / Zoom SDK dependencies so the whole decision can be
// pinned by a test with no engine and no meeting.
//
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// From the SDK headers: IMeetingTalkbackController::CreateChannel documents
// "Supports a maximum of 16 channels", and AddUserToInvite "A channel can
// have at most 10 users."
constexpr uint32_t kTalkbackMaxChannels        = 16;
constexpr uint32_t kTalkbackMaxUsersPerChannel = 10;

struct TalkbackPlannedChannel {
    // Nominee display names, resolved to meeting-scoped ids only at invite
    // time -- see the spec on why ids are never stored.
    std::vector<std::string> members;
    // True for a slice of the all-talent target, false for a one-person
    // private channel.
    bool is_all_talent = false;
};

struct TalkbackPlan {
    std::vector<TalkbackPlannedChannel> channels;
    // Nominees with no private channel of their own. They are still reachable
    // via all-talent; they just cannot be taken aside.
    std::vector<std::string> uncovered_private;
    // False when the panel is so large that even the all-talent fan-out does
    // not fit in 16 channels -- at which point some people cannot be reached
    // at all, and the operator has to know.
    bool all_talent_complete = true;
};

inline TalkbackPlan talkback_plan(const std::vector<std::string> &nominees)
{
    TalkbackPlan plan;

    // Collapse duplicates, preserving nomination order. A name repeated in the
    // list is one person; letting it consume two private channels would spend
    // the budget on nobody.
    std::vector<std::string> unique;
    for (const auto &n : nominees) {
        bool seen = false;
        for (const auto &u : unique) if (u == n) { seen = true; break; }
        if (!seen) unique.push_back(n);
    }
    if (unique.empty()) return plan;

    // ── All-talent first, and never truncated to make room for privates ───
    const std::size_t need =
        (unique.size() + kTalkbackMaxUsersPerChannel - 1) / kTalkbackMaxUsersPerChannel;
    const std::size_t all_talent_channels =
        need > kTalkbackMaxChannels ? kTalkbackMaxChannels : need;
    plan.all_talent_complete = (all_talent_channels == need);

    for (std::size_t i = 0; i < all_talent_channels; ++i) {
        TalkbackPlannedChannel c;
        c.is_all_talent = true;
        const std::size_t first = i * kTalkbackMaxUsersPerChannel;
        const std::size_t last  =
            std::min(first + kTalkbackMaxUsersPerChannel, unique.size());
        for (std::size_t m = first; m < last; ++m) c.members.push_back(unique[m]);
        plan.channels.push_back(std::move(c));
    }

    // ── Private channels take whatever is left ────────────────────────────
    std::size_t remaining = kTalkbackMaxChannels - plan.channels.size();
    for (std::size_t i = 0; i < unique.size(); ++i) {
        if (remaining == 0) {
            // Named, not dropped. Someone who cannot be taken aside must be
            // visible to the operator BEFORE they try.
            plan.uncovered_private.push_back(unique[i]);
            continue;
        }
        TalkbackPlannedChannel c;
        c.is_all_talent = false;
        c.members.push_back(unique[i]);
        plan.channels.push_back(std::move(c));
        --remaining;
    }

    return plan;
}
