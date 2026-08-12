// src/director-preview-frame-guard.h
#pragma once

#include <cstdint>

// Whether a frame delivered by the hidden director-preview subscription may be
// published to the program output.
//
// The preview slot exists only to warm the NEXT speaker's region so the cut
// lands on a real frame instead of a gap, but publishing pushes straight to the
// program output — so a frame from anyone other than the participant the slot
// is currently pointed at puts the wrong face on air. Two of those arrive
// routinely: frames still in flight for the previous target after the slot is
// re-pointed, and frames the engine had already queued when the cut
// unsubscribed it (which leaves nothing awaited, i.e. 0). On the 2026-08-11
// live show that read as a participant flashing up with no speaker change
// behind them, and a late frame carrying a different id than the one just cut
// to made the commit logic cut straight back to it — sub-second A->B->A cuts.
//
// So: exactly one frame is the cut, the one belonging to the live preview
// target. An idle slot (awaited 0) awaits nobody and publishes nothing, which
// also keeps an unidentified frame (id 0) from reading as a match for it.
inline bool should_publish_director_preview_frame(uint32_t awaited_participant_id,
                                                  uint32_t frame_participant_id)
{
    return awaited_participant_id != 0 &&
           frame_participant_id == awaited_participant_id;
}
