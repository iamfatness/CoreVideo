// src/zoom-tiles-audio-plan.h
// Decides what to do about per-participant audio for a Tiles wall: what to
// create, what to adopt, what to mute, what to leave strictly alone.
//
// Pure by design, and the purity is not stylistic. Every action in the output
// mutates the operator's scene collection, so the rules that decide them are
// the part that has to be provable without a rig — a bug here either doubles
// somebody's audio on air or damages a scene the operator built by hand.

#pragma once

#include "zoom-types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// OBS provides six audio tracks. Track 1 is the program mix.
inline constexpr std::size_t kTilesAudioMaxTracks = 6;

// One audio source the plugin owns or could adopt, as read off the scene
// collection before planning. Only sources carrying the ownership marker are
// ever represented here; an operator's own sources are invisible to the
// planner and therefore cannot be chosen as targets.
struct TilesAudioSourceState {
    uint32_t    participant_id = 0;
    std::string name;
    std::string owner_uuid;  // empty => orphaned: marked as ours, owner gone
    bool        muted        = false;
    uint32_t    mixers       = 0;  // bitmask; bit 0 = track 1
};

enum class TilesAudioActionKind {
    Create,     // nothing exists for this participant
    Adopt,      // an orphan exists; take ownership of it
    Unmute,     // back on the wall
    Mute,       // left the wall — never deleted
    SetMixers,  // track assignment drifted, or was just adopted
};

struct TilesAudioAction {
    TilesAudioActionKind kind           = TilesAudioActionKind::Create;
    uint32_t             participant_id = 0;
    std::string          name;        // Create only
    uint32_t             mixers       = 0;  // Create and SetMixers
};

struct TilesAudioPlanParams {
    std::string self_uuid;        // this Tiles source's obs_source_get_uuid()
    bool        enabled = false;  // false => no actions at all
};

struct TilesAudioPlan {
    std::vector<TilesAudioAction> actions;
    std::size_t                   overflow = 0;  // participants past the stems
};

// Track 1 (bit 0) is the program mix and every source joins it, which is what
// gives the operator a live fader for everyone. Tracks 2..6 carry one ISO stem
// each — five of them — so the sixth participant onward is program-only. That
// is a real ceiling in OBS, not a limit worth pretending around: the plan
// reports the overflow so it can be logged rather than silently swallowed.
inline uint32_t tiles_audio_mixers_for_slot(std::size_t slot)
{
    constexpr uint32_t kProgram = 1u;
    if (slot + 1 >= kTilesAudioMaxTracks) return kProgram;
    return kProgram | (1u << static_cast<uint32_t>(slot + 1));
}

inline TilesAudioPlan plan_tiles_audio(
    const std::vector<uint32_t>              &assignments,
    const std::vector<TilesAudioSourceState> &existing,
    const std::vector<ParticipantInfo>       &roster,
    const TilesAudioPlanParams               &params)
{
    TilesAudioPlan plan;
    if (!params.enabled) return plan;

    const auto find_existing =
        [&existing](uint32_t id) -> const TilesAudioSourceState * {
        for (const auto &s : existing)
            if (s.participant_id == id) return &s;
        return nullptr;
    };

    const auto display_name = [&roster](uint32_t id) -> std::string {
        for (const auto &p : roster)
            if (p.user_id == id && !p.display_name.empty())
                return p.display_name + " (CoreVideo)";
        // A participant can be assigned but momentarily absent from the roster
        // (a manual tile held for someone who has not rejoined). The id keeps
        // the source identifiable rather than anonymous.
        return "Participant " + std::to_string(id) + " (CoreVideo)";
    };

    std::vector<uint32_t> handled;
    handled.reserve(assignments.size());

    std::size_t slot = 0;
    for (const uint32_t id : assignments) {
        if (id == 0) continue;
        // resolve_tile_assignments already de-duplicates, but a duplicate here
        // would mean two slots claiming one voice, so it is dropped explicitly.
        if (std::find(handled.begin(), handled.end(), id) != handled.end())
            continue;
        handled.push_back(id);

        const TilesAudioSourceState *cur = find_existing(id);

        // Owned by a different Tiles source: not ours to create, mute, or
        // retrack. Creating a second source here would carry this participant's
        // voice twice and double them in the mix.
        if (cur && !cur->owner_uuid.empty() && cur->owner_uuid != params.self_uuid) {
            ++slot;
            continue;
        }

        const uint32_t want = tiles_audio_mixers_for_slot(slot);
        if (slot + 1 >= kTilesAudioMaxTracks) ++plan.overflow;

        if (!cur) {
            TilesAudioAction a;
            a.kind           = TilesAudioActionKind::Create;
            a.participant_id = id;
            a.name           = display_name(id);
            a.mixers         = want;
            plan.actions.push_back(std::move(a));
            ++slot;
            continue;
        }

        // An orphan: marked as ours, but its creating Tiles source is gone.
        // Adopting beats creating — the operator's fader and any filters they
        // added to it survive.
        if (cur->owner_uuid.empty()) {
            TilesAudioAction a;
            a.kind           = TilesAudioActionKind::Adopt;
            a.participant_id = id;
            plan.actions.push_back(a);
        }

        if (cur->muted) {
            TilesAudioAction a;
            a.kind           = TilesAudioActionKind::Unmute;
            a.participant_id = id;
            plan.actions.push_back(a);
        }
        if (cur->mixers != want) {
            TilesAudioAction a;
            a.kind           = TilesAudioActionKind::SetMixers;
            a.participant_id = id;
            a.mixers         = want;
            plan.actions.push_back(a);
        }
        ++slot;
    }

    // Anyone we own who is no longer on the wall is muted and kept. Deleting
    // would take their fader, their filters and any operator tuning with them,
    // and in Auto mode the wall reflows constantly.
    for (const auto &s : existing) {
        if (s.owner_uuid != params.self_uuid) continue;
        if (std::find(handled.begin(), handled.end(), s.participant_id) !=
            handled.end())
            continue;
        if (s.muted) continue;
        TilesAudioAction a;
        a.kind           = TilesAudioActionKind::Mute;
        a.participant_id = s.participant_id;
        plan.actions.push_back(a);
    }

    return plan;
}
