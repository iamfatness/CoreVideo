#pragma once

// How long the CoreVideo Active Speaker source keeps its hidden director
// preview alive after a cut.
//
// Extracted from src/zoom-source.cpp so it can be tested without libobs, the
// same treatment director-preview-frame-guard.h gets and for the same reason:
// the only symptom of a regression is a flash on a live broadcast.
//
// THE DEFECT THIS EXISTS FOR (2026-08-16, live show). The cut released the main
// source's SHM mapping, sent an asynchronous subscribe for the new participant
// on the main uuid, and unsubscribed the preview in the same breath. The engine
// needs 735-1277 ms to destroy and rebuild that uuid's region, and the preview
// -- which had been publishing the new speaker to air via
// on_director_preview_frame() -- stops delivering the moment it is
// unsubscribed. So for the best part of a second nothing published at all. With
// hold_ms tuned to 1200, below the handover cost, cuts landed on top of each
// other and the source was in that window almost continuously.

#include <cstdint>

// How long to wait for the main subscription before giving the preview back
// anyway. A preview held forever is a wasted shared-memory slot (one of
// kMaxShmSources, engine-ipc.h) and a wasted Zoom subscription; three seconds
// is several times the worst handover observed on air (1277 ms).
constexpr uint64_t kDirectorHandoverTimeoutMs = 3000;

enum class DirectorHandoverAction {
    // Not mid-handover. Nothing to do.
    None,
    // Mid-handover and the main subscription has not delivered the new
    // participant yet. Keep the preview: it is the only thing on air.
    Hold,
    // The main subscription delivered the participant we cut to. Release the
    // preview; the main mapping takes over with no gap.
    Complete,
    // The main subscription never delivered. Release the preview anyway rather
    // than pin a shared-memory slot for the rest of the session.
    AbandonOnTimeout,
};

// `main_frame_id` is the participant whose frame just arrived on the MAIN
// subscription, or 0 if none has arrived since the cut.
//
// Completing is keyed on the main subscription delivering the participant we
// cut TO, not merely delivering something: frames for the previous speaker stay
// in flight across the cut, and treating one of those as "the main mapping is
// ready" would release the preview while the new region is still being built --
// which is exactly the gap this exists to close.
inline DirectorHandoverAction director_handover_action(
    bool     pending,
    uint32_t target_id,
    uint32_t main_frame_id,
    uint64_t elapsed_ms,
    uint64_t timeout_ms)
{
    if (!pending) return DirectorHandoverAction::None;

    if (target_id != 0 && main_frame_id == target_id)
        return DirectorHandoverAction::Complete;

    if (elapsed_ms >= timeout_ms)
        return DirectorHandoverAction::AbandonOnTimeout;

    return DirectorHandoverAction::Hold;
}
