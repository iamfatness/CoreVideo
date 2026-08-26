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
// room. Past 160 unique nominees even the all-talent fan-out no longer fits
// 16 channels; those people are named again, separately, in `unreachable`,
// because they hear nothing at all -- a strictly worse failure than losing
// only the private aside, so it gets its own list rather than being buried
// inside uncovered_private.
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
    // Nominees with no private channel of their own. Most are still reachable
    // via all-talent -- but not all: when `all_talent_complete` is false, the
    // ones ALSO in `unreachable` are on no channel whatsoever. This field
    // alone cannot tell the two apart; cross-check `unreachable`.
    std::vector<std::string> uncovered_private;
    // Nominees on no channel at all -- not in any all-talent slice AND not
    // privately covered. Always a subset of uncovered_private (see above),
    // and always empty when all_talent_complete is true: the all-talent
    // fan-out is only short a member when it was clamped to the 16-channel
    // cap below the count it actually needed. Named explicitly, like
    // uncovered_private, rather than left for a caller to infer from the
    // all_talent_complete bool alone -- a bare bool tells the operator
    // *that* someone is unreachable, not *who*, and that is the exact
    // failure this planner exists to name instead of hide.
    std::vector<std::string> unreachable;
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
        // Fix round 1, m5: parenthesizing the call defeats windows.h's
        // min/max macros for any future TU that includes this header after
        // <windows.h> without NOMINMAX -- CMakeLists.txt's NOMINMAX fix for
        // ZoomObsEngine (Task 2) covers the two targets that exist today,
        // but this header itself stays a trap for the next one otherwise;
        // src/shm-generation.h has the same precedent (UINT32_MAX instead of
        // std::numeric_limits) for the same reason.
        const std::size_t last  =
            (std::min)(first + kTalkbackMaxUsersPerChannel, unique.size());
        for (std::size_t m = first; m < last; ++m) c.members.push_back(unique[m]);
        plan.channels.push_back(std::move(c));
    }

    // ── Anyone past the all-talent fan-out's reach is unreachable ──────────
    // Only nonempty when all_talent_channels was clamped below `need` (i.e.
    // !all_talent_complete): the fan-out then only covers the first
    // all_talent_channels * 10 unique nominees, and everyone after that
    // index is in no all-talent slice. The private loop below is about to
    // push every one of these into uncovered_private too (its `remaining`
    // budget is 16 - all_talent_channels, which is exactly 0 whenever this
    // clamp fired), so `unreachable` stays the strict subset the struct
    // comment promises without extra bookkeeping.
    const std::size_t all_talent_covered =
        (std::min)(all_talent_channels * kTalkbackMaxUsersPerChannel, unique.size());
    for (std::size_t i = all_talent_covered; i < unique.size(); ++i)
        plan.unreachable.push_back(unique[i]);

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

// ── Selecting a provisioned channel at key time (Task 3, 2026-08-25) ────────
//
// The planner above decides which channels exist. This decides which of them a
// key press talks on, and it lives here rather than in the engine for the same
// reason the planner does: it is the whole of the new key-path decision, it
// needs no SDK, and the engine is the one layer this feature's tests cannot
// reach (three review rounds have now named that gap). Keying is meant to be
// nothing but this lookup plus a send -- the create+invite round trip that used
// to sit here is exactly what clipped the director's first words.
//
// A TARGET is what the operator keys: either the all-talent target (the
// sentinel below) or one nominee's name. It is NOT a channel: an all-talent
// target with more than 10 people owns ceil(n/10) channels and the same PCM
// goes to all of them, so this answers "does this channel serve that target",
// once per channel, rather than returning a single id.
//
// The sentinel shares a namespace with participant display names, and the
// control API's target argument is one string (Task 5: `"all"` or a nominee's
// name), so a participant displaying as "all" collides with it. Fix round 1
// (review, promoted from Minor): the consequence is not merely that they
// cannot be keyed privately -- it is that keying their name goes on air to
// the WHOLE PANEL, which is the exact privacy promise talkback is built on,
// and display names are set by the participant, so it is provokable rather
// than only accidental. So this is DETECTED AND REFUSED at nomination
// (`talkback_nominate_sentinel_collision()` below), where the operator can
// still do something about it, instead of being documented and left live.
constexpr const char *kTalkbackAllTalentTarget = "all";

// Is `target` the all-talent target? ASCII case-insensitive, deliberately.
// The review found the case-sensitive version worse than a plain collision:
// "All" did not collide while "all" did, so an operator would see the failure
// come and go with a participant's capitalisation and have no way to reason
// about it. One rule, both directions -- any casing is the sentinel here, and
// any casing is refused as a nominee name below. Participant names themselves
// are still compared EXACTLY (identity is by name; two people can differ only
// in case and both be real).
inline bool talkback_target_is_all_talent(const std::string &target)
{
    static const std::string sentinel = kTalkbackAllTalentTarget;
    if (target.size() != sentinel.size()) return false;
    for (std::size_t i = 0; i < target.size(); ++i) {
        char a = target[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != sentinel[i]) return false;
    }
    return true;
}

// The first nominee whose name would collide with the sentinel, or an empty
// string if none does. nominate() refuses the whole plan on a hit and names
// the person: provisioning around them would leave a channel nobody can key
// privately and a name that silently broadcasts.
inline std::string talkback_nominate_sentinel_collision(
    const std::vector<std::string> &nominees)
{
    for (const auto &n : nominees)
        if (talkback_target_is_all_talent(n)) return n;
    return {};
}

// Task 5: can the PLUGIN, from nothing but the last nomination's async plan
// report (the "plan"/"uncovered_private" stage lines, forwarded verbatim over
// the E2P pipe), already know that keying `target` cannot possibly succeed --
// so a control-API key press can be refused before it ever opens the tap or
// asks the engine, instead of opening then being retracted a tick later once
// session_start()'s refusal arrives (the exact flicker key_on()'s
// engine-running/in-meeting checks already exist to avoid, for the same
// reason).
//
// This is deliberately a "known bad" test, not a "known good" one: a false
// negative here just costs one refused round trip (session_start() still
// checks, and still refuses with a specific reason -- see
// engine/src/engine-talkback.cpp), while a false positive would refuse a key
// that would have worked. The one case this CANNOT see is
// "provisioning_incomplete" -- a target whose channel the nomination ladder
// is still creating -- because the plugin is only ever told the finished
// PLAN, never the ladder's in-flight progress; that case still relies on
// session_start()'s own async refusal (see TalkbackController::evaluate()'s
// grace period).
//
// `requested` is the nominee list the plugin itself sent with the last
// talkback_nominate; `uncovered_private` is the engine's own report of who
// in that list got no private channel. "all" is coverable whenever
// `requested` is non-empty: talkback_plan() always allocates at least one
// all-talent slice first, and never sacrifices it for private channels (see
// this file's header comment), so the only way "all" is unprovisioned is
// that nothing was ever nominated. A specific name is coverable only when it
// was nominated AND is not in uncovered_private -- talkback_channel_serves_target()
// matches a private target by membership alone, never by belonging to an
// all-talent slice, so being reachable via "all" does not make a person's
// OWN name keyable.
inline bool talkback_target_known_unprovisioned(
    const std::string &target,
    const std::vector<std::string> &requested,
    const std::vector<std::string> &uncovered_private)
{
    if (talkback_target_is_all_talent(target))
        return requested.empty();
    for (const auto &name : requested) {
        if (name != target) continue;
        for (const auto &u : uncovered_private)
            if (u == target) return true; // nominated, but no private channel
        return false;                     // nominated AND privately covered
    }
    return true; // never nominated at all
}

inline bool talkback_channel_serves_target(bool is_all_talent,
                                           const std::vector<std::string> &members,
                                           const std::string &target)
{
    if (talkback_target_is_all_talent(target)) return is_all_talent;
    // A private channel is matched by MEMBERSHIP, not by "members[0] == target":
    // talkback_plan() gives every private channel exactly one member today, and
    // matching by membership keeps this correct if that ever stops being true
    // (a two-person aside, say) instead of silently selecting nothing. An
    // all-talent slice is deliberately never matched by a person's name -- it
    // contains them, but keying their name must not put the director on air to
    // nine other people.
    if (is_all_talent) return false;
    for (const auto &m : members) if (m == target) return true;
    return false;
}
