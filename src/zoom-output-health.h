#pragma once

#include "zoom-output-manager.h"
#include <algorithm>
#include <unordered_map>
#include <vector>

inline void apply_output_health(std::vector<ZoomOutputInfo> &outputs,
                                const std::vector<ParticipantInfo> &roster,
                                bool raw_media_active)
{
    // Loop variables are named `out` rather than `output` because a test
    // translation unit that includes this header (tests/output-health-test.cpp)
    // defines a file-static helper function named `output()`; a local
    // `output` here shadowed it (cppcheck shadowFunction).
    std::unordered_map<uint32_t, size_t> assigned_counts;
    for (const auto &out : outputs) {
        if (out.assignment == AssignmentMode::Participant &&
            out.participant_id != 0) {
            ++assigned_counts[out.participant_id];
        }
    }

    for (auto &out : outputs) {
        out.duplicate_participant_assignment = false;
        out.health_reason = ZoomOutputHealthReason::Ok;
        if (out.assignment == AssignmentMode::Participant &&
            out.participant_id != 0) {
            out.duplicate_participant_assignment =
                assigned_counts[out.participant_id] > 1;
        }
    }

    const bool screen_share_available = std::any_of(
        roster.begin(), roster.end(),
        [](const ParticipantInfo &participant) {
            return participant.is_sharing_screen;
        });
    const bool active_video_speaker_available = std::any_of(
        roster.begin(), roster.end(),
        [](const ParticipantInfo &participant) {
            return participant.is_talking && participant.has_video;
        });

    auto find_participant = [&roster](uint32_t participant_id) {
        return std::find_if(roster.begin(), roster.end(),
            [participant_id](const ParticipantInfo &participant) {
                return participant.user_id == participant_id;
            });
    };

    for (auto &out : outputs) {
        const bool wants_media =
            out.assignment == AssignmentMode::Participant ||
            out.assignment == AssignmentMode::ActiveSpeaker ||
            out.assignment == AssignmentMode::SpotlightIndex ||
            out.assignment == AssignmentMode::ScreenShare;
        if (!raw_media_active) {
            out.health_reason = wants_media
                ? ZoomOutputHealthReason::RawMediaNotReady
                : ZoomOutputHealthReason::Ok;
        } else if (out.duplicate_participant_assignment) {
            out.health_reason = ZoomOutputHealthReason::DuplicateAssignment;
        } else if (out.assignment == AssignmentMode::ScreenShare &&
                   !screen_share_available) {
            out.health_reason = ZoomOutputHealthReason::ScreenShareUnavailable;
        } else if (out.assignment == AssignmentMode::ActiveSpeaker &&
                   out.participant_id == 0 &&
                   !active_video_speaker_available) {
            out.health_reason = ZoomOutputHealthReason::ActiveSpeakerUnavailable;
        } else if (out.assignment == AssignmentMode::SpotlightIndex &&
                   out.spotlight_slot > 0) {
            const auto spotlight_it = std::find_if(
                roster.begin(), roster.end(),
                [&out](const ParticipantInfo &participant) {
                    return participant.spotlight_index == out.spotlight_slot;
                });
            if (spotlight_it == roster.end()) {
                out.health_reason = ZoomOutputHealthReason::SpotlightUnavailable;
            } else if (!spotlight_it->has_video) {
                out.health_reason = ZoomOutputHealthReason::ParticipantVideoOff;
            }
        } else if (out.assignment == AssignmentMode::Participant &&
                   out.participant_id != 0) {
            const auto participant_it = find_participant(out.participant_id);
            if (participant_it == roster.end()) {
                out.health_reason = ZoomOutputHealthReason::ParticipantMissing;
            } else if (!participant_it->has_video) {
                out.health_reason = ZoomOutputHealthReason::ParticipantVideoOff;
            }
        }

        if (out.health_reason != ZoomOutputHealthReason::Ok)
            continue;
        if (out.video_stale) {
            out.health_reason = ZoomOutputHealthReason::StaleFrame;
        } else if (out.observed_width == 0 || out.observed_height == 0) {
            out.health_reason = ZoomOutputHealthReason::WaitingForFirstFrame;
        } else if (output_signal_below_requested(out)) {
            out.health_reason = ZoomOutputHealthReason::ZoomDeliveredLowerResolution;
        }
    }
}
