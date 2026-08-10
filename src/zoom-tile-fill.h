#pragma once

#include "zoom-types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// How the tile wall decides who it shows.
enum class TileFillMode {
    Auto   = 0,  // everyone sending video, minus exclusions
    Manual = 1,  // exactly who the operator cast, in slot order
};

struct TileFillParams {
    TileFillMode          mode      = TileFillMode::Auto;
    std::size_t           max_tiles = 9;
    std::vector<uint32_t> excluded;  // Auto only; zeros are empty slots
    std::vector<uint32_t> manual;    // Manual only; zeros are empty slots
};

// Decides the wall's next assignment list.
//
// `previous` is what is on the wall right now, and it is what makes the result
// stable: participants who are still eligible keep their existing positions,
// and only new arrivals are appended. Rebuilding purely from roster order would
// let any SDK reordering move every face at once, which on air is
// indistinguishable from a bug.
//
// Duplicate and zero ids are dropped in both modes. An empty result is legal
// and means an empty wall.
inline std::vector<uint32_t> resolve_tile_assignments(
    const std::vector<uint32_t> &previous,
    const std::vector<ParticipantInfo> &roster,
    const TileFillParams &params)
{
    std::vector<uint32_t> out;
    if (params.max_tiles == 0) return out;
    out.reserve(params.max_tiles);

    const auto push_unique = [&out](uint32_t id) {
        if (id == 0) return;
        if (std::find(out.begin(), out.end(), id) != out.end()) return;
        out.push_back(id);
    };

    if (params.mode == TileFillMode::Manual) {
        // The roster is deliberately not consulted: an operator who cast a
        // tile keeps it even while that participant's camera is off.
        for (const uint32_t id : params.manual) push_unique(id);
        if (out.size() > params.max_tiles) out.resize(params.max_tiles);
        return out;
    }

    const auto eligible = [&params, &roster](uint32_t id) {
        if (id == 0) return false;
        if (std::find(params.excluded.begin(), params.excluded.end(), id) !=
            params.excluded.end())
            return false;
        const auto it = std::find_if(
            roster.begin(), roster.end(),
            [id](const ParticipantInfo &p) { return p.user_id == id; });
        return it != roster.end() && it->has_video;
    };

    // Incumbents first, in their existing order...
    for (const uint32_t id : previous)
        if (eligible(id)) push_unique(id);
    // ...then newcomers, in roster order.
    for (const ParticipantInfo &p : roster)
        if (eligible(p.user_id)) push_unique(p.user_id);

    if (out.size() > params.max_tiles) out.resize(params.max_tiles);
    return out;
}
