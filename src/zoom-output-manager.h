#pragma once

#include "zoom-types.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct ZoomSource;

enum class ZoomOutputHealthReason {
    Ok = 0,
    RawMediaNotReady,
    ParticipantMissing,
    ParticipantVideoOff,
    WaitingForFirstFrame,
    StaleFrame,
    ZoomDeliveredLowerResolution,
    DuplicateAssignment,
    ScreenShareUnavailable,
    ActiveSpeakerUnavailable,
    SpotlightUnavailable,
};

struct ZoomOutputInfo {
    std::string source_uuid;
    std::string source_name;
    std::string display_name; // user-editable label; falls back to source_name if empty
    uint32_t participant_id = 0;
    bool active_speaker = false;
    bool isolate_audio = false;
    bool audience_audio = false;
    AudioChannelMode audio_mode = AudioChannelMode::Mono;
    VideoResolution video_resolution = VideoResolution::P720;
    uint32_t observed_width = 0;
    uint32_t observed_height = 0;
    double observed_fps = 0.0;
    uint64_t last_frame_age_ms = 0;
    bool video_stale = false;
    uint32_t stale_recovery_attempts = 0;
    uint64_t stale_recovery_cooldown_ms = 0;
    // True once recovery has failed repeatedly: the feed is still retried at
    // the slow backoff cadence, but the operator should be told it can't
    // currently be subscribed (participant likely camera-off / phone-only).
    bool recovery_stalled = false;
    uint32_t quality_upgrade_attempts = 0;
    uint64_t quality_upgrade_cooldown_ms = 0;
    uint64_t subscribed_age_ms = 0;
    int negotiated_resolution = -1;
    int last_set_resolution_code = -1;
    int last_video_subscribe_code = -1;
    int last_raw_status = -1;
    uint64_t last_quality_event_age_ms = 0;
    bool subscription_downgraded = false;
    std::string last_quality_stage;
    bool duplicate_participant_assignment = false;
    ZoomOutputHealthReason health_reason = ZoomOutputHealthReason::Ok;
    AssignmentMode   assignment = AssignmentMode::Participant;
    uint32_t         spotlight_slot = 1;     // used when assignment == SpotlightIndex
    uint32_t         failover_participant_id = 0; // 0 = none
    // Engine capture to OBS publish, microseconds. 0 = not yet measured.
    uint64_t audio_latency_us = 0;
    uint64_t video_latency_us = 0;
    // Per-source override of ZoomPluginSettings::audio_delay_ms. 0-500 ms.
    // Applied to THIS ZoomSource's own embedded audio only -- see the
    // audio_delay_ms parameter comment on configure_output()/configure_output_ex()
    // below for how it is set, and zoom-source.cpp's on_engine_audio() for
    // where it is applied.
    uint32_t audio_delay_ms = 0;
};

// Sentinel for the `audio_delay_ms` parameter of configure_output() and
// configure_output_ex(): "leave this output's current delay override
// unchanged." Every existing caller of these two functions (OSC server,
// per-source hotkeys, ZoomSource's own active-speaker self-reconfigure)
// fully re-specifies every OTHER field on each call, so a real (non-sentinel)
// default here would silently reset an operator-set delay back to 0 on every
// unrelated reconfigure -- e.g. simply switching which participant an output
// shows. UINT32_MAX is never a valid clamped delay (range is 0-500), so it
// cannot collide with a real value.
inline constexpr uint32_t kAudioDelayKeepCurrentMs = 0xFFFFFFFFu;

inline const char *output_health_reason_id(ZoomOutputHealthReason reason)
{
    switch (reason) {
    case ZoomOutputHealthReason::Ok: return "ok";
    case ZoomOutputHealthReason::RawMediaNotReady: return "raw_media_not_ready";
    case ZoomOutputHealthReason::ParticipantMissing: return "participant_missing";
    case ZoomOutputHealthReason::ParticipantVideoOff: return "participant_video_off";
    case ZoomOutputHealthReason::WaitingForFirstFrame: return "waiting_for_first_frame";
    case ZoomOutputHealthReason::StaleFrame: return "stale_frame";
    case ZoomOutputHealthReason::ZoomDeliveredLowerResolution: return "zoom_delivered_lower_resolution";
    case ZoomOutputHealthReason::DuplicateAssignment: return "duplicate_assignment";
    case ZoomOutputHealthReason::ScreenShareUnavailable: return "screen_share_unavailable";
    case ZoomOutputHealthReason::ActiveSpeakerUnavailable: return "active_speaker_unavailable";
    case ZoomOutputHealthReason::SpotlightUnavailable: return "spotlight_unavailable";
    }
    return "unknown";
}

inline const char *output_health_reason_label(ZoomOutputHealthReason reason)
{
    switch (reason) {
    case ZoomOutputHealthReason::Ok: return "OK";
    case ZoomOutputHealthReason::RawMediaNotReady: return "Raw media not ready";
    case ZoomOutputHealthReason::ParticipantMissing: return "Participant missing";
    case ZoomOutputHealthReason::ParticipantVideoOff: return "Video off";
    case ZoomOutputHealthReason::WaitingForFirstFrame: return "Waiting for first frame";
    case ZoomOutputHealthReason::StaleFrame: return "Stale frame";
    case ZoomOutputHealthReason::ZoomDeliveredLowerResolution: return "Zoom delivered lower resolution";
    case ZoomOutputHealthReason::DuplicateAssignment: return "Duplicate assignment";
    case ZoomOutputHealthReason::ScreenShareUnavailable: return "Screen share unavailable";
    case ZoomOutputHealthReason::ActiveSpeakerUnavailable: return "Active speaker unavailable";
    case ZoomOutputHealthReason::SpotlightUnavailable: return "Spotlight slot unavailable";
    }
    return "Unknown";
}

inline uint32_t video_resolution_width(VideoResolution resolution)
{
    switch (resolution) {
    case VideoResolution::P360: return 640;
    case VideoResolution::P1080: return 1920;
    case VideoResolution::P720:
    default: return 1280;
    }
}

inline uint32_t video_resolution_height(VideoResolution resolution)
{
    switch (resolution) {
    case VideoResolution::P360: return 360;
    case VideoResolution::P1080: return 1080;
    case VideoResolution::P720:
    default: return 720;
    }
}

inline bool observed_signal_has_1080_lines(uint32_t observed_height)
{
    return observed_height + 8 >= video_resolution_height(VideoResolution::P1080);
}

inline bool observed_signal_satisfies_requested(uint32_t observed_width,
                                                uint32_t observed_height,
                                                VideoResolution requested)
{
    if (observed_width == 0 || observed_height == 0)
        return false;
    if (requested == VideoResolution::P1080 &&
        observed_signal_has_1080_lines(observed_height)) {
        return true;
    }
    return observed_width + 8 >= video_resolution_width(requested) &&
           observed_height + 8 >= video_resolution_height(requested);
}

inline bool observed_signal_below_requested(uint32_t observed_width,
                                            uint32_t observed_height,
                                            VideoResolution requested)
{
    if (observed_width == 0 || observed_height == 0)
        return false;
    return !observed_signal_satisfies_requested(
        observed_width, observed_height, requested);
}

inline bool output_signal_below_requested(const ZoomOutputInfo &output)
{
    return observed_signal_below_requested(output.observed_width,
                                           output.observed_height,
                                           output.video_resolution);
}

inline bool output_signal_missing_or_stale(const ZoomOutputInfo &output)
{
    return output.observed_width == 0 || output.observed_height == 0 ||
           output.video_stale;
}

class ZoomOutputManager {
public:
    static ZoomOutputManager &instance();

    void register_source(ZoomSource *source);
    void unregister_source(ZoomSource *source);

    std::vector<ZoomOutputInfo> outputs() const;
    bool configure_output(const std::string &source_name,
                          uint32_t participant_id,
                          bool active_speaker,
                          bool isolate_audio,
                          AudioChannelMode audio_mode,
                          VideoResolution video_resolution = VideoResolution::P720,
                          bool audience_audio = false,
                          // 0-500, or kAudioDelayKeepCurrentMs to leave the
                          // output's current per-source delay untouched.
                          uint32_t audio_delay_ms = kAudioDelayKeepCurrentMs);
    // Extended variant supporting ZoomISO-style assignment modes (spotlight,
    // screen share) plus failover. Returns true if the output was found.
    bool configure_output_ex(const std::string &source_name,
                             AssignmentMode mode,
                             uint32_t participant_id,
                             uint32_t spotlight_slot,
                             uint32_t failover_participant_id,
                             bool isolate_audio,
                             AudioChannelMode audio_mode,
                             VideoResolution video_resolution = VideoResolution::P720,
                             bool audience_audio = false,
                             uint32_t audio_delay_ms = kAudioDelayKeepCurrentMs);

    // Re-send subscribe commands for all active sources after engine recovery.
    void resubscribe_all();
    uint32_t recover_stale_sources(bool force = false);
    uint32_t upgrade_low_quality_sources(bool force = false);

    // Preview callbacks - call from the UI thread only.
    void set_preview_cb(const std::string &source_name,
                        ZoomPreviewCallback cb);
    void clear_preview_cb(const std::string &source_name);
    void clear_all_preview_cbs();

private:
    ZoomOutputManager() = default;

    mutable std::mutex m_mtx;
    std::vector<ZoomSource *> m_sources;
};
