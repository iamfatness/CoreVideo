#pragma once

// Which participants a VIDEO-assignment picker should offer.
//
// Extracted so it can be tested without OBS or Qt, and so every picker shares
// one rule rather than each growing its own.
//
// Deliberately NOT used by the audio pickers: a dedicated CoreVideo audio
// source follows somebody's microphone, and the people worth adding one for are
// very often exactly the ones with their camera off. Filtering them out of an
// audio picker would make them unreachable.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

#include "zoom-types.h"

// `keep_user_id` is the participant this picker is currently pointed at, or 0.
//
// It is never filtered out, even with their camera off. A picker that dropped
// its own current value would lose the selection on the next roster tick and
// silently unbind the source -- which is how a camera being switched off
// mid-show would turn into a black output nobody asked for.
inline std::vector<ParticipantInfo> visible_for_video_assignment(
    const std::vector<ParticipantInfo> &roster,
    bool                                hide_non_video,
    uint32_t                            keep_user_id)
{
    if (!hide_non_video) return roster;

    std::vector<ParticipantInfo> visible;
    visible.reserve(roster.size());
    std::copy_if(roster.begin(), roster.end(), std::back_inserter(visible),
                 [keep_user_id](const ParticipantInfo &p) {
                     return p.has_video ||
                            (keep_user_id != 0 && p.user_id == keep_user_id);
                 });
    return visible;
}
