#include "zoom-source.h"
#include "shm-resubscribe.h"
#include "speaker-director.h"
#include "speaker-settings-merge.h"
#include "zoom-engine-client.h"
#include "zoom-iso-recorder.h"
#include "zoom-settings.h"
#include <media-io/audio-io.h>
#include <media-io/video-io.h>
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

#define PROP_OUTPUT_DISPLAY_NAME  "output_display_name"
#define PROP_PARTICIPANT_ID       "participant_id"
#define PROP_ACTIVE_SPEAKER       "active_speaker_mode"
#define PROP_SPEAKER_SENSITIVITY  "speaker_sensitivity_ms"
#define PROP_SPEAKER_HOLD         "speaker_hold_ms"
#define PROP_SPEAKER_EXCLUDE_1    "speaker_exclude_participant_1"
#define PROP_SPEAKER_EXCLUDE_2    "speaker_exclude_participant_2"
#define PROP_VIDEO_LOSS_MODE      "video_loss_mode"
#define PROP_ISOLATE_AUDIO        "isolate_audio"
#define PROP_AUDIENCE_AUDIO       "audience_audio"
#define PROP_ACTIVE_AUDIO_ROLE    "active_speaker_audio_role"
#define PROP_AUDIO_CHANNELS       "audio_channels"
#define PROP_RESOLUTION           "resolution"
#define PROP_STATUS               "status_label"
#define PROP_LIVE_STATUS          "live_status"
#define PROP_HW_ACCEL_MODE        "hw_accel_mode"
#define PROP_ASSIGNMENT_MODE      "assignment_mode"
#define PROP_SPOTLIGHT_SLOT       "spotlight_slot"
#define PROP_FAILOVER_PARTICIPANT "failover_participant_id"

#define VIDEO_LOSS_LAST_FRAME 0
#define VIDEO_LOSS_BLACK      1
#define AUDIO_CH_MONO   0
#define AUDIO_CH_STEREO 1
#define ACTIVE_AUDIO_MIX      0
#define ACTIVE_AUDIO_ISOLATED 1
#define ACTIVE_AUDIO_AUDIENCE 2
#define RES_360P  0
#define RES_720P  1
#define RES_1080P 2

static constexpr uint32_t kDefaultWidth = 1280;
static constexpr uint32_t kDefaultHeight = 720;

static std::atomic<uint64_t> s_source_counter{1};
static constexpr uint32_t kZoomBytesPerSample = sizeof(int16_t);
static constexpr uint64_t kPreviewIntervalNs = 200'000'000ULL;
static constexpr uint64_t kStaleVideoNs = 3'000'000'000ULL;
static constexpr uint64_t kNoSignalRecoverNs = 5'000'000'000ULL;
static constexpr uint64_t kStaleRecoverCooldownNs = 10'000'000'000ULL;
// After this many consecutive failed recovery attempts a feed is treated as
// "stalled": recovery keeps trying but at the slow max cadence, and the
// operator sees a clear status instead of an invisible retry storm (a feed
// once retried 128x every 10s — 2026-08-09). A frame arriving resets this.
static constexpr uint32_t kStaleRecoverStalledAfter = 6;

// Exponential backoff for stale recovery: 10s, 20s, 40s, 80s, capped at
// ~2.5 min, so a feed that cannot come back (camera off, phone-only) stops
// hammering the SDK while still retrying periodically if it recovers.
static uint64_t stale_recover_cooldown_ns(uint32_t attempts)
{
    const uint32_t shift = std::min<uint32_t>(attempts, 4);
    return kStaleRecoverCooldownNs << shift;
}
static constexpr uint64_t kQualityUpgradeStableNs = 20'000'000'000ULL;
static constexpr uint64_t kQualityUpgradeBaseCooldownNs = 60'000'000'000ULL;

static uint64_t quality_upgrade_cooldown_ns(uint32_t completed_attempts)
{
    // Keep trying during long meetings, but cap automatic retry pressure at
    // eight minutes. A feed that reaches the requested resolution resets this.
    const uint32_t shift = std::min<uint32_t>(completed_attempts, 3);
    return kQualityUpgradeBaseCooldownNs << shift;
}

static AudioChannelMode audio_mode_from_data(obs_data_t *s)
{
    return obs_data_get_int(s, PROP_AUDIO_CHANNELS) == AUDIO_CH_STEREO
        ? AudioChannelMode::Stereo : AudioChannelMode::Mono;
}

static VideoResolution resolution_from_data(obs_data_t *s)
{
    switch (obs_data_get_int(s, PROP_RESOLUTION)) {
    case RES_360P: return VideoResolution::P360;
    case RES_1080P: return VideoResolution::P1080;
    case RES_720P:
    default:       return VideoResolution::P720;
    }
}

static const char *meeting_state_to_text(MeetingState state)
{
    switch (state) {
    case MeetingState::Idle:       return "ZoomStatus.Idle";
    case MeetingState::Joining:    return "ZoomStatus.Joining";
    case MeetingState::InMeeting:  return "ZoomStatus.InMeeting";
    case MeetingState::Leaving:    return "ZoomStatus.Leaving";
    case MeetingState::Recovering: return "ZoomStatus.Joining"; // reuse "Joining..." locale string
    case MeetingState::Failed:     return "ZoomStatus.Failed";
    }
    return "ZoomStatus.Idle";
}

static std::string make_source_uuid()
{
    return "source_" + std::to_string(os_gettime_ns()) + "_" +
        std::to_string(s_source_counter.fetch_add(1, std::memory_order_relaxed));
}

static uint32_t effective_participant_id(uint32_t configured_participant_id,
                                         bool active_speaker_mode)
{
    if (!active_speaker_mode) return configured_participant_id;
    const uint32_t active = ZoomEngineClient::instance().active_speaker_id();
    return active != 0 ? active : configured_participant_id;
}

static bool is_active_speaker_assignment(AssignmentMode mode)
{
    return mode == AssignmentMode::ActiveSpeaker;
}

static std::vector<uint32_t> speaker_exclusions_from_settings(
    const ZoomPluginSettings &settings)
{
    std::vector<uint32_t> ids;
    if (settings.speaker_exclude_participant_1 != 0)
        ids.push_back(settings.speaker_exclude_participant_1);
    if (settings.speaker_exclude_participant_2 != 0 &&
        settings.speaker_exclude_participant_2 !=
            settings.speaker_exclude_participant_1)
        ids.push_back(settings.speaker_exclude_participant_2);
    return ids;
}

static void configure_speaker_director_from_settings(
    const ZoomPluginSettings &settings)
{
    SpeakerDirector::instance().configure(
        settings.speaker_sensitivity_ms, settings.speaker_hold_ms,
        settings.speaker_require_video,
        speaker_exclusions_from_settings(settings));
}

static uint32_t width_for_resolution(VideoResolution res)
{
    switch (res) {
    case VideoResolution::P360: return 640;
    case VideoResolution::P720: return 1280;
    case VideoResolution::P1080:
    default: return 1920;
    }
}

static uint32_t height_for_resolution(VideoResolution res)
{
    switch (res) {
    case VideoResolution::P360: return 360;
    case VideoResolution::P720: return 720;
    case VideoResolution::P1080:
    default: return 1080;
    }
}

static bool observed_satisfies_resolution(uint32_t observed_w,
                                          uint32_t observed_h,
                                          VideoResolution requested)
{
    return observed_signal_satisfies_requested(observed_w, observed_h,
                                               requested);
}

static uint8_t clamp_u8(int value)
{
    return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

static void scale_plane_nearest(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                uint32_t src_stride, uint8_t *dst,
                                uint32_t dst_w, uint32_t dst_h,
                                uint32_t dst_stride)
{
    if (!src || !dst || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0)
        return;

    for (uint32_t y = 0; y < dst_h; ++y) {
        const uint32_t sy = std::min<uint32_t>(
            static_cast<uint32_t>((static_cast<uint64_t>(y) * src_h) / dst_h),
            src_h - 1);
        const uint8_t *src_row = src + sy * src_stride;
        uint8_t *dst_row = dst + y * dst_stride;
        for (uint32_t x = 0; x < dst_w; ++x) {
            const uint32_t sx = std::min<uint32_t>(
                static_cast<uint32_t>((static_cast<uint64_t>(x) * src_w) / dst_w),
                src_w - 1);
            dst_row[x] = src_row[sx];
        }
    }
}

static void scale_i420_letterbox(const uint8_t *src_y,
                                 const uint8_t *src_u,
                                 const uint8_t *src_v,
                                 uint32_t src_w,
                                 uint32_t src_h,
                                 uint8_t *dst,
                                 uint32_t dst_w,
                                 uint32_t dst_h)
{
    const size_t dst_y_size = static_cast<size_t>(dst_w) * dst_h;
    const size_t dst_uv_size = dst_y_size / 4;
    uint8_t *dst_y = dst;
    uint8_t *dst_u = dst_y + dst_y_size;
    uint8_t *dst_v = dst_u + dst_uv_size;

    std::memset(dst_y, 0, dst_y_size);
    std::memset(dst_u, 128, dst_uv_size);
    std::memset(dst_v, 128, dst_uv_size);

    if (!src_y || !src_u || !src_v || src_w == 0 || src_h == 0 ||
        dst_w == 0 || dst_h == 0)
        return;

    uint32_t fit_w = dst_w;
    uint32_t fit_h = static_cast<uint32_t>(
        (static_cast<uint64_t>(dst_w) * src_h) / src_w);
    if (fit_h > dst_h) {
        fit_h = dst_h;
        fit_w = static_cast<uint32_t>(
            (static_cast<uint64_t>(dst_h) * src_w) / src_h);
    }

    fit_w = std::max<uint32_t>(2, fit_w & ~1u);
    fit_h = std::max<uint32_t>(2, fit_h & ~1u);
    const uint32_t off_x = ((dst_w - fit_w) / 2) & ~1u;
    const uint32_t off_y = ((dst_h - fit_h) / 2) & ~1u;

    scale_plane_nearest(src_y, src_w, src_h, src_w,
                        dst_y + off_y * dst_w + off_x,
                        fit_w, fit_h, dst_w);
    scale_plane_nearest(src_u, src_w / 2, src_h / 2, src_w / 2,
                        dst_u + (off_y / 2) * (dst_w / 2) + off_x / 2,
                        fit_w / 2, fit_h / 2, dst_w / 2);
    scale_plane_nearest(src_v, src_w / 2, src_h / 2, src_w / 2,
                        dst_v + (off_y / 2) * (dst_w / 2) + off_x / 2,
                        fit_w / 2, fit_h / 2, dst_w / 2);
}

static void rgb_to_i420_values(uint8_t r, uint8_t g, uint8_t b,
                               uint8_t &y, uint8_t &u, uint8_t &v)
{
    y = clamp_u8((77 * r + 150 * g + 29 * b + 128) >> 8);
    u = clamp_u8(((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128);
    v = clamp_u8(((128 * r - 107 * g - 21 * b + 128) >> 8) + 128);
}

static int active_audio_role_from_settings(obs_data_t *settings)
{
    if (obs_data_get_bool(settings, PROP_ISOLATE_AUDIO))
        return ACTIVE_AUDIO_ISOLATED;
    if (obs_data_get_bool(settings, PROP_AUDIENCE_AUDIO))
        return ACTIVE_AUDIO_AUDIENCE;
    return ACTIVE_AUDIO_MIX;
}

static AssignmentMode assignment_mode_from_settings(obs_data_t *settings)
{
    const int mode_int = static_cast<int>(
        obs_data_get_int(settings, PROP_ASSIGNMENT_MODE));
    if (mode_int == static_cast<int>(AssignmentMode::ActiveSpeaker))
        return AssignmentMode::ActiveSpeaker;
    if (mode_int == static_cast<int>(AssignmentMode::SpotlightIndex))
        return AssignmentMode::SpotlightIndex;
    if (mode_int == static_cast<int>(AssignmentMode::ScreenShare))
        return AssignmentMode::ScreenShare;
    return AssignmentMode::Participant;
}

static bool update_assignment_property_visibility(obs_properties_t *props,
                                                  obs_data_t *settings)
{
    const AssignmentMode mode = assignment_mode_from_settings(settings);
    const bool participant_mode = mode == AssignmentMode::Participant;
    const bool active_mode = mode == AssignmentMode::ActiveSpeaker;
    const bool spotlight_mode = mode == AssignmentMode::SpotlightIndex;
    const bool screen_share_mode = mode == AssignmentMode::ScreenShare;

    auto set_visible = [props](const char *name, bool visible) {
        if (obs_property_t *prop = obs_properties_get(props, name))
            obs_property_set_visible(prop, visible);
    };

    set_visible(PROP_PARTICIPANT_ID, participant_mode);
    set_visible(PROP_FAILOVER_PARTICIPANT, participant_mode);
    set_visible(PROP_SPOTLIGHT_SLOT, spotlight_mode);
    set_visible(PROP_ACTIVE_SPEAKER, false);
    set_visible(PROP_SPEAKER_SENSITIVITY, active_mode);
    set_visible(PROP_SPEAKER_HOLD, active_mode);
    set_visible(PROP_SPEAKER_EXCLUDE_1, active_mode);
    set_visible(PROP_SPEAKER_EXCLUDE_2, active_mode);
    set_visible(PROP_ISOLATE_AUDIO, !screen_share_mode);
    set_visible(PROP_AUDIENCE_AUDIO, !screen_share_mode);
    set_visible(PROP_AUDIO_CHANNELS, !screen_share_mode);
    set_visible(PROP_RESOLUTION, !screen_share_mode);
    return true;
}

static std::string participant_label(uint32_t participant_id)
{
    if (participant_id == 0)
        return "none";
    for (const auto &p : ZoomEngineClient::instance().roster()) {
        if (p.user_id != participant_id)
            continue;
        if (!p.display_name.empty())
            return p.display_name + " (" + std::to_string(participant_id) + ")";
        break;
    }
    return "ID " + std::to_string(participant_id);
}

static std::string audio_route_label(const ZoomOutputInfo &info)
{
    if (info.audience_audio)
        return "Audience fallback";
    if (info.isolate_audio)
        return "Isolated";
    return "Program mix";
}

static std::string assignment_status_label(const ZoomOutputInfo &info)
{
    switch (info.assignment) {
    case AssignmentMode::ActiveSpeaker:
        return "Active speaker -> " +
            participant_label(ZoomEngineClient::instance().active_speaker_id());
    case AssignmentMode::SpotlightIndex:
        return "Spotlight slot " + std::to_string(info.spotlight_slot);
    case AssignmentMode::ScreenShare:
        return "Active screen share";
    case AssignmentMode::Participant:
    default:
        return "Participant -> " + participant_label(info.participant_id);
    }
}

static std::string video_status_label(const ZoomOutputInfo &info)
{
    if (info.observed_width == 0 || info.observed_height == 0)
        return "No video yet";

    std::string text = std::to_string(info.observed_width) + "x" +
        std::to_string(info.observed_height);
    if (info.observed_fps > 0.01) {
        char fps[32] = {};
        snprintf(fps, sizeof(fps), " %.1f fps", info.observed_fps);
        text += fps;
    }
    if (info.video_stale)
        text += " stale";
    else
        text += " live";
    return text;
}

static std::string recovery_status_label(const ZoomOutputInfo &info)
{
    if (info.assignment == AssignmentMode::ScreenShare)
        return "Screen share follows Zoom's active share feed.";
    if (output_signal_below_requested(info)) {
        std::string text = "Using best available feed below requested canvas";
        if (info.quality_upgrade_cooldown_ms > 0)
            text += "; next quality retry in " +
                std::to_string(info.quality_upgrade_cooldown_ms) + " ms";
        else if (info.quality_upgrade_attempts > 0)
            text += "; quality retry attempted " +
                std::to_string(info.quality_upgrade_attempts) + " time(s)";
        return text;
    }
    if (info.video_stale) {
        std::string text = "Video stale";
        if (info.stale_recovery_cooldown_ms > 0)
            text += "; next recovery retry in " +
                std::to_string(info.stale_recovery_cooldown_ms) + " ms";
        return text;
    }
    if (info.observed_width == 0 || info.observed_height == 0)
        return "Waiting for Zoom media.";
    return "Feed matches current source state.";
}

static std::string live_status_text(const ZoomOutputInfo &info)
{
    return "Assigned: " + assignment_status_label(info) +
        "\nVideo: " + video_status_label(info) +
        "\nAudio: " + audio_route_label(info) +
        "\nRecovery: " + recovery_status_label(info);
}

static bool source_wants_subscription(AssignmentMode mode,
                                      uint32_t participant_id)
{
    if (mode == AssignmentMode::ScreenShare ||
        mode == AssignmentMode::SpotlightIndex ||
        mode == AssignmentMode::ActiveSpeaker)
        return true;

    return participant_id != 0;
}

static void set_yuv_frame_color_info(obs_source_frame &frame)
{
    frame.full_range = true;
    video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_FULL,
                                           frame.format, frame.color_matrix,
                                           frame.color_range_min,
                                           frame.color_range_max);
}

static uint8_t sample_center_luma(const uint8_t *y_ptr, uint32_t w, uint32_t h,
                                  uint32_t stride_y)
{
    if (!y_ptr || w == 0 || h == 0 || stride_y == 0) return 0;
    return y_ptr[static_cast<size_t>(h / 2) * stride_y + (w / 2)];
}

void ZoomSource::apply_settings(obs_data_t *settings)
{
    const uint32_t old_participant_id = participant_id;
    const bool old_active_speaker_mode = active_speaker_mode;
    const AudioChannelMode old_audio_mode = audio_mode;
    const VideoResolution old_resolution = resolution;
    const int old_hw_accel_override = hw_accel_override;
    const AssignmentMode old_assignment = assignment.load(std::memory_order_acquire);
    const uint32_t old_spotlight_slot = spotlight_slot.load(std::memory_order_acquire);
    const uint32_t old_failover = failover_participant_id.load(std::memory_order_acquire);
    // Snapshot audio routing flags so a toggle re-subscribes — without this
    // the engine keeps the previous flag and the source plays the wrong feed
    // (e.g. enabling isolate after the fact would silently leave the source
    // on the full mix).
    const bool old_isolate_audio = isolate_audio;
    const bool old_audience_audio = audience_audio;

    output_display_name = obs_data_get_string(settings, PROP_OUTPUT_DISPLAY_NAME);
    participant_id = static_cast<uint32_t>(obs_data_get_int(settings, PROP_PARTICIPANT_ID));
    const bool legacy_active_speaker = obs_data_get_bool(settings, PROP_ACTIVE_SPEAKER);
    speaker_sensitivity_ms = static_cast<uint32_t>(
        obs_data_get_int(settings, PROP_SPEAKER_SENSITIVITY));
    speaker_hold_ms = static_cast<uint32_t>(obs_data_get_int(settings, PROP_SPEAKER_HOLD));
    if (dedicated_active_speaker_source ||
        obs_data_get_int(settings, PROP_ASSIGNMENT_MODE) ==
            static_cast<int>(AssignmentMode::ActiveSpeaker)) {
        ZoomPluginSettings plugin_settings = ZoomPluginSettings::load();
        // Only push values this source actually carries. apply_settings() runs
        // on every source load and property refresh, not just on a user edit,
        // so mirroring unset properties into the global settings let a source
        // that never had an exclusion configured wipe the one the dock holds.
        // The precedence rule and the on-air consequence of getting it wrong
        // are in tests/speaker-settings-merge-test.cpp.
        SpeakerSourceValues carried;
        carried.has_sensitivity =
            obs_data_has_user_value(settings, PROP_SPEAKER_SENSITIVITY);
        carried.sensitivity_ms = speaker_sensitivity_ms;
        carried.has_hold = obs_data_has_user_value(settings, PROP_SPEAKER_HOLD);
        carried.hold_ms = speaker_hold_ms;
        carried.has_exclude_1 =
            obs_data_has_user_value(settings, PROP_SPEAKER_EXCLUDE_1);
        carried.exclude_1 = static_cast<uint32_t>(
            obs_data_get_int(settings, PROP_SPEAKER_EXCLUDE_1));
        carried.has_exclude_2 =
            obs_data_has_user_value(settings, PROP_SPEAKER_EXCLUDE_2);
        carried.exclude_2 = static_cast<uint32_t>(
            obs_data_get_int(settings, PROP_SPEAKER_EXCLUDE_2));

        SpeakerGlobalSettings merged;
        merged.sensitivity_ms = plugin_settings.speaker_sensitivity_ms;
        merged.hold_ms        = plugin_settings.speaker_hold_ms;
        merged.exclude_1      = plugin_settings.speaker_exclude_participant_1;
        merged.exclude_2      = plugin_settings.speaker_exclude_participant_2;
        merged = merge_speaker_settings(merged, carried);

        plugin_settings.speaker_sensitivity_ms         = merged.sensitivity_ms;
        plugin_settings.speaker_hold_ms                = merged.hold_ms;
        plugin_settings.speaker_exclude_participant_1  = merged.exclude_1;
        plugin_settings.speaker_exclude_participant_2  = merged.exclude_2;
        plugin_settings.save();
        configure_speaker_director_from_settings(plugin_settings);
    }
    isolate_audio = obs_data_get_bool(settings, PROP_ISOLATE_AUDIO);
    audience_audio = obs_data_get_bool(settings, PROP_AUDIENCE_AUDIO);
    if (dedicated_active_speaker_source &&
        obs_data_has_user_value(settings, PROP_ACTIVE_AUDIO_ROLE)) {
        const int role = static_cast<int>(
            obs_data_get_int(settings, PROP_ACTIVE_AUDIO_ROLE));
        isolate_audio = role == ACTIVE_AUDIO_ISOLATED;
        audience_audio = role == ACTIVE_AUDIO_AUDIENCE;
        obs_data_set_bool(settings, PROP_ISOLATE_AUDIO, isolate_audio);
        obs_data_set_bool(settings, PROP_AUDIENCE_AUDIO, audience_audio);
    }
    // isolate wins if both were saved together (legacy scenes).
    if (isolate_audio) audience_audio = false;
    audio_mode = audio_mode_from_data(settings);
    resolution = resolution_from_data(settings);
    video_loss_mode = obs_data_get_int(settings, PROP_VIDEO_LOSS_MODE) == VIDEO_LOSS_BLACK
        ? VideoLossMode::Black : VideoLossMode::LastFrame;
    hw_accel_override = static_cast<int>(obs_data_get_int(settings, PROP_HW_ACCEL_MODE));
    {
        const int mode_int = static_cast<int>(
            obs_data_get_int(settings, PROP_ASSIGNMENT_MODE));
        AssignmentMode mode = AssignmentMode::Participant;
        if (mode_int == 1) mode = AssignmentMode::ActiveSpeaker;
        else if (mode_int == 2) mode = AssignmentMode::SpotlightIndex;
        else if (mode_int == 3) mode = AssignmentMode::ScreenShare;
        if (dedicated_active_speaker_source)
            mode = AssignmentMode::ActiveSpeaker;
        // Back-compat: the older bool PROP_ACTIVE_SPEAKER promotes plain
        // participant mode to ActiveSpeaker. After this point assignment mode
        // is the source of truth for overlapping source/output-manager state.
        if (mode == AssignmentMode::Participant && legacy_active_speaker)
            mode = AssignmentMode::ActiveSpeaker;
        assignment.store(mode, std::memory_order_release);
        active_speaker_mode.store(is_active_speaker_assignment(mode),
                                  std::memory_order_release);
        obs_data_set_bool(settings, PROP_ACTIVE_SPEAKER,
                          is_active_speaker_assignment(mode));
        obs_data_set_int(settings, PROP_ASSIGNMENT_MODE, static_cast<int>(mode));
        spotlight_slot.store(static_cast<uint32_t>(std::max<int64_t>(1,
            obs_data_get_int(settings, PROP_SPOTLIGHT_SLOT))),
            std::memory_order_release);
        failover_participant_id.store(static_cast<uint32_t>(
            obs_data_get_int(settings, PROP_FAILOVER_PARTICIPANT)),
            std::memory_order_release);
    }

    const AssignmentMode new_assignment = assignment.load(std::memory_order_acquire);
    const uint32_t new_spot = spotlight_slot.load(std::memory_order_acquire);
    const uint32_t new_failover = failover_participant_id.load(std::memory_order_acquire);

    if (m_subscribed && (old_participant_id != participant_id ||
        old_active_speaker_mode != active_speaker_mode ||
        old_audio_mode != audio_mode || old_resolution != resolution ||
        old_assignment != new_assignment ||
        old_spotlight_slot != new_spot ||
        old_failover != new_failover ||
        old_isolate_audio != isolate_audio ||
        old_audience_audio != audience_audio)) {
        subscribe();
    }

    if (!m_subscribed &&
        source_wants_subscription(new_assignment, participant_id)) {
        subscribe();
    }

    if (hw_accel_override != old_hw_accel_override) {
        // Serialize against the IPC reader thread which uses m_hw_pipeline in
        // on_engine_frame(). Without this lock, shutdown() can free filter
        // graph state while another thread is mid-process() → use-after-free.
        std::lock_guard<std::mutex> lk(m_mtx);
        m_hw_pipeline.shutdown();
        const HwAccelMode effective = hw_accel_override >= 0
            ? static_cast<HwAccelMode>(hw_accel_override)
            : ZoomPluginSettings::load().hw_accel_mode;
        if (effective != HwAccelMode::None)
            m_hw_pipeline.init(effective);
    }

    ZoomIsoRecorder::instance().on_output_updated(output_info());
}

std::string ZoomSource::output_name() const
{
    const char *name = source ? obs_source_get_name(source) : nullptr;
    return name ? name : std::string{};
}

ZoomOutputInfo ZoomSource::output_info() const
{
    ZoomOutputInfo info;
    info.source_uuid = source_uuid;
    info.source_name = output_name();
    info.display_name = output_display_name;
    info.participant_id = participant_id;
    const AssignmentMode mode = assignment.load(std::memory_order_acquire);
    info.active_speaker = is_active_speaker_assignment(mode);
    info.isolate_audio = isolate_audio;
    info.audience_audio = audience_audio;
    info.audio_mode = audio_mode;
    info.video_resolution = resolution;
    info.observed_width = m_width.load(std::memory_order_relaxed);
    info.observed_height = m_height.load(std::memory_order_relaxed);
    info.observed_fps =
        static_cast<double>(m_observed_fps_x100.load(std::memory_order_relaxed)) / 100.0;
    const uint64_t last_frame_ns = m_last_frame_ns.load(std::memory_order_relaxed);
    const uint64_t now_ns = os_gettime_ns();
    if (last_frame_ns != 0) {
        const uint64_t age_ns = now_ns > last_frame_ns ? now_ns - last_frame_ns : 0;
        info.last_frame_age_ms = age_ns / 1'000'000ULL;
        info.video_stale = age_ns > kStaleVideoNs;
        const uint64_t last_recover_ns =
            m_last_stale_recover_ns.load(std::memory_order_relaxed);
        if (last_recover_ns != 0) {
            const uint64_t recover_age_ns =
                now_ns > last_recover_ns ? now_ns - last_recover_ns : 0;
            const uint64_t recover_cooldown_ns = stale_recover_cooldown_ns(
                m_stale_recover_attempts.load(std::memory_order_relaxed));
            if (recover_age_ns < recover_cooldown_ns) {
                info.stale_recovery_cooldown_ms =
                    (recover_cooldown_ns - recover_age_ns) / 1'000'000ULL;
            }
        }
        const uint64_t last_upgrade_ns =
            m_last_quality_upgrade_ns.load(std::memory_order_relaxed);
        if (last_upgrade_ns != 0) {
            const uint64_t upgrade_age_ns =
                now_ns > last_upgrade_ns ? now_ns - last_upgrade_ns : 0;
            const uint64_t cooldown_ns = quality_upgrade_cooldown_ns(
                m_quality_upgrade_attempts.load(std::memory_order_relaxed));
            if (upgrade_age_ns < cooldown_ns) {
                info.quality_upgrade_cooldown_ms =
                    (cooldown_ns - upgrade_age_ns) / 1'000'000ULL;
            }
        }
    }
    const uint64_t last_subscribe_ns =
        m_last_subscribe_ns.load(std::memory_order_relaxed);
    if (last_subscribe_ns != 0 && now_ns > last_subscribe_ns)
        info.subscribed_age_ms = (now_ns - last_subscribe_ns) / 1'000'000ULL;
    info.stale_recovery_attempts =
        m_stale_recover_attempts.load(std::memory_order_relaxed);
    info.recovery_stalled =
        info.stale_recovery_attempts >= kStaleRecoverStalledAfter;
    info.quality_upgrade_attempts =
        m_quality_upgrade_attempts.load(std::memory_order_relaxed);
    info.assignment = mode;
    info.spotlight_slot = spotlight_slot.load(std::memory_order_acquire);
    info.failover_participant_id = failover_participant_id.load(std::memory_order_acquire);
    return info;
}

void ZoomSource::configure_output(uint32_t new_participant_id,
                                  bool new_active_speaker_mode,
                                  bool new_isolate_audio,
                                  AudioChannelMode new_audio_mode,
                                  VideoResolution new_resolution,
                                  bool new_audience_audio)
{
    // isolate wins if both somehow got requested — mirrors apply_settings().
    if (new_isolate_audio) new_audience_audio = false;
    if (source) {
        obs_data_t *settings = obs_source_get_settings(source);
        const AssignmentMode mode = new_active_speaker_mode
            ? AssignmentMode::ActiveSpeaker
            : AssignmentMode::Participant;
        obs_data_set_int(settings, PROP_ASSIGNMENT_MODE, static_cast<int>(mode));
        obs_data_set_int(settings, PROP_PARTICIPANT_ID, new_participant_id);
        obs_data_set_bool(settings, PROP_ACTIVE_SPEAKER, new_active_speaker_mode);
        obs_data_set_bool(settings, PROP_ISOLATE_AUDIO, new_isolate_audio);
        obs_data_set_bool(settings, PROP_AUDIENCE_AUDIO, new_audience_audio);
        obs_data_set_int(settings, PROP_AUDIO_CHANNELS,
                         new_audio_mode == AudioChannelMode::Stereo
                         ? AUDIO_CH_STEREO : AUDIO_CH_MONO);
        obs_data_set_int(settings, PROP_RESOLUTION,
                         static_cast<int>(new_resolution));
        obs_source_update(source, settings);
        obs_data_release(settings);
        return;
    }

    assignment.store(new_active_speaker_mode
                     ? AssignmentMode::ActiveSpeaker
                     : AssignmentMode::Participant,
                     std::memory_order_release);
    participant_id = new_participant_id;
    active_speaker_mode = new_active_speaker_mode;
    isolate_audio = new_isolate_audio;
    audience_audio = new_audience_audio;
    audio_mode = new_audio_mode;
    resolution = new_resolution;
    if (m_subscribed) subscribe();
}

void ZoomSource::configure_output_ex(AssignmentMode mode,
                                     uint32_t new_participant_id,
                                     uint32_t new_spotlight_slot,
                                     uint32_t new_failover_participant_id,
                                     bool new_isolate_audio,
                                     AudioChannelMode new_audio_mode,
                                     VideoResolution new_resolution,
                                     bool new_audience_audio)
{
    if (dedicated_active_speaker_source) {
        mode = AssignmentMode::ActiveSpeaker;
        new_participant_id = 0;
        new_spotlight_slot = 1;
        new_failover_participant_id = 0;
    }
    if (new_isolate_audio) new_audience_audio = false;
    const bool active_speaker = (mode == AssignmentMode::ActiveSpeaker);
    if (source) {
        obs_data_t *settings = obs_source_get_settings(source);
        obs_data_set_int(settings, PROP_ASSIGNMENT_MODE, static_cast<int>(mode));
        obs_data_set_int(settings, PROP_PARTICIPANT_ID, new_participant_id);
        obs_data_set_bool(settings, PROP_ACTIVE_SPEAKER, active_speaker);
        obs_data_set_int(settings, PROP_SPOTLIGHT_SLOT, new_spotlight_slot);
        obs_data_set_int(settings, PROP_FAILOVER_PARTICIPANT, new_failover_participant_id);
        obs_data_set_bool(settings, PROP_ISOLATE_AUDIO, new_isolate_audio);
        obs_data_set_bool(settings, PROP_AUDIENCE_AUDIO, new_audience_audio);
        obs_data_set_int(settings, PROP_AUDIO_CHANNELS,
                         new_audio_mode == AudioChannelMode::Stereo
                         ? AUDIO_CH_STEREO : AUDIO_CH_MONO);
        obs_data_set_int(settings, PROP_RESOLUTION,
                         static_cast<int>(new_resolution));
        obs_source_update(source, settings);
        obs_data_release(settings);
        return;
    }

    assignment.store(mode, std::memory_order_release);
    participant_id = new_participant_id;
    active_speaker_mode = active_speaker;
    spotlight_slot.store(new_spotlight_slot, std::memory_order_release);
    failover_participant_id.store(new_failover_participant_id, std::memory_order_release);
    isolate_audio = new_isolate_audio;
    audience_audio = new_audience_audio;
    audio_mode = new_audio_mode;
    resolution = new_resolution;
    if (m_subscribed) subscribe();
}

bool ZoomSource::wants_subscription() const
{
    if (!m_active.load(std::memory_order_acquire))
        return false;
    return source_wants_subscription(
        assignment.load(std::memory_order_acquire),
        participant_id.load(std::memory_order_acquire));
}

void ZoomSource::subscribe()
{
    if (source_uuid.empty()) source_uuid = make_source_uuid();
    m_last_roster_video_state.store(-1, std::memory_order_release);
    if (!ZoomEngineClient::instance().is_running()) {
        m_subscribed = false;
        m_current_subscription_id = 0;
        m_director_preview_subscription_id = 0;
        m_last_subscribe_ns.store(0, std::memory_order_release);
        return;
    }

    const AssignmentMode mode = assignment.load(std::memory_order_acquire);
    switch (mode) {
    case AssignmentMode::SpotlightIndex: {
        const uint32_t slot = spotlight_slot.load(std::memory_order_acquire);
        // Release BEFORE the subscribe reaches the engine, every time, not just
        // when we can see the target change. Any subscribe for a uuid the
        // engine already holds can make it destroy and rebuild that uuid's
        // SourceTarget — whose SHM generation then restarts at the legacy
        // unsuffixed name — and a live mapping of ours blocks the recreate.
        // See shm-resubscribe.h. Cost when the engine treats the subscribe as a
        // no-op is one remap on the next frame event; the on-air cost of
        // getting this wrong is a white flash on every speaker change.
        release_video_shm();
        ZoomEngineClient::instance().subscribe_spotlight(source_uuid, slot);
        // Use a sentinel so on_roster_changed re-subscribes only when we
        // actually want to change the dispatch.
        m_current_subscription_id = 0xFFFF0000u | slot;
        m_last_subscribe_ns.store(os_gettime_ns(), std::memory_order_release);
        m_subscribed = true;
        return;
    }
    case AssignmentMode::ScreenShare:
        release_video_shm();  // before the subscribe — see shm-resubscribe.h
        ZoomEngineClient::instance().subscribe_screenshare(source_uuid);
        m_current_subscription_id = 0xFFFFFFFFu;
        m_last_subscribe_ns.store(os_gettime_ns(), std::memory_order_release);
        m_subscribed = true;
        return;
    case AssignmentMode::ActiveSpeaker:
    case AssignmentMode::Participant:
    default: {
        if (dedicated_active_speaker_source) {
            maybe_update_director_subscription();
            m_subscribed = true;
            return;
        }
        // Resolve target participant. If primary is missing from the current
        // roster and we have a failover ID, use it.
        const bool use_active_speaker = is_active_speaker_assignment(mode);
        if (use_active_speaker) {
            const ZoomPluginSettings settings = ZoomPluginSettings::load();
            configure_speaker_director_from_settings(settings);
        }
        uint32_t target = effective_participant_id(participant_id,
                                                    use_active_speaker);
        const uint32_t failover = failover_participant_id.load(std::memory_order_acquire);
        if (target != 0 && failover != 0 && !use_active_speaker) {
            const auto roster = ZoomEngineClient::instance().roster();
            const bool primary_present = std::any_of(
                roster.begin(), roster.end(),
                [&](const ParticipantInfo &p) { return p.user_id == target; });
            if (!primary_present) target = failover;
        }
        if (target == 0 && !use_active_speaker) {
            // Assignment cleared ("None"). Tear down any previous engine
            // subscription: skipping this left the old feed streaming into
            // shared memory while the UI showed None, and flipping
            // m_subscribed below made unsubscribe() a permanent no-op, so
            // the leaked subscription could never be cleaned up.
            const bool had_subscription =
                m_subscribed.load(std::memory_order_acquire) ||
                m_current_subscription_id.load(std::memory_order_acquire) != 0;
            if (had_subscription) {
                blog(LOG_INFO,
                     "[obs-zoom-plugin] Assignment cleared; unsubscribing Zoom source: source=%s uuid=%s",
                     output_name().c_str(), source_uuid.c_str());
                clear_subscription_state();
            }
            m_subscribed = false;
            m_current_subscription_id = 0;
            return;
        }
        if (target != 0) {
            release_video_shm();  // before the subscribe — see shm-resubscribe.h
            ZoomEngineClient::instance().subscribe(source_uuid, target,
                                                   isolate_audio, audience_audio,
                                                   resolution);
        }
        blog(LOG_INFO,
             "[obs-zoom-plugin] Zoom source subscription: source=%s uuid=%s participant_id=%u",
             output_name().c_str(), source_uuid.c_str(), target);
        m_current_subscription_id = target;
        m_subscribed = target != 0 || use_active_speaker;
        if (target != 0)
            m_last_subscribe_ns.store(os_gettime_ns(),
                                      std::memory_order_release);
        return;
    }
    }

    if (m_active.load(std::memory_order_acquire) &&
        m_width.load(std::memory_order_relaxed) == 0)
        output_placeholder_frame(true);
}

void ZoomSource::unsubscribe()
{
    if (!m_subscribed) return;
    clear_subscription_state();
}

void ZoomSource::clear_subscription_state()
{
    ZoomEngineClient::instance().unsubscribe(source_uuid);
    if (!m_director_preview_uuid.empty())
        ZoomEngineClient::instance().unsubscribe(m_director_preview_uuid);
    // Drop our SHM mappings: a held mapping keeps the named section alive,
    // which blocks the engine from ever recreating it at a larger size —
    // and makes clearing the assignment an operator recovery tool.
    release_shared_memory();
    m_subscribed = false;
    m_current_subscription_id = 0;
    m_director_preview_subscription_id = 0;
    m_width.store(0, std::memory_order_relaxed);
    m_height.store(0, std::memory_order_relaxed);
    m_observed_fps_x100.store(0, std::memory_order_relaxed);
    m_last_frame_ns.store(0, std::memory_order_relaxed);
    m_last_subscribe_ns.store(0, std::memory_order_relaxed);
    m_last_stale_recover_ns.store(0, std::memory_order_relaxed);
    m_stale_recover_attempts.store(0, std::memory_order_relaxed);
    m_last_quality_upgrade_ns.store(0, std::memory_order_relaxed);
    m_quality_upgrade_attempts.store(0, std::memory_order_relaxed);
    m_last_roster_video_state.store(-1, std::memory_order_relaxed);
}

bool ZoomSource::recover_stale_video(uint64_t now_ns, bool force)
{
    if (!m_active.load(std::memory_order_acquire))
        return false;
    if (!ZoomEngineClient::instance().is_running() ||
        ZoomEngineClient::instance().state() != MeetingState::InMeeting ||
        !ZoomEngineClient::instance().is_media_active())
        return false;
    // A source that wants a feed but lost its subscription (engine
    // reconnect, a cleared flag) has no other automatic way back — the
    // periodic recovery must be able to re-establish it, not just refresh
    // an already-subscribed one. Otherwise only a manual Apply/Recover
    // restored video (2026-08-09).
    if (!m_subscribed.load(std::memory_order_acquire)) {
        if (!wants_subscription())
            return false;
        blog(LOG_INFO,
             "[obs-zoom-plugin] Recovery re-subscribing dropped source: source=%s uuid=%s",
             output_name().c_str(), source_uuid.c_str());
        subscribe();
        return true;
    }

    const uint64_t last_frame_ns =
        m_last_frame_ns.load(std::memory_order_acquire);
    const uint64_t last_subscribe_ns =
        m_last_subscribe_ns.load(std::memory_order_acquire);
    bool missing_initial_frame = false;
    uint64_t age_ns = 0;
    if (last_frame_ns == 0) {
        if (last_subscribe_ns == 0 || now_ns <= last_subscribe_ns ||
            now_ns - last_subscribe_ns <= kNoSignalRecoverNs)
            return false;
        missing_initial_frame = true;
        age_ns = now_ns - last_subscribe_ns;
    } else {
        if (now_ns <= last_frame_ns || now_ns - last_frame_ns <= kStaleVideoNs)
            return false;
        age_ns = now_ns - last_frame_ns;
    }

    const uint64_t last_recover_ns =
        m_last_stale_recover_ns.load(std::memory_order_acquire);
    const uint64_t cooldown_ns = stale_recover_cooldown_ns(
        m_stale_recover_attempts.load(std::memory_order_relaxed));
    if (!force && last_recover_ns != 0 &&
        now_ns - last_recover_ns < cooldown_ns)
        return false;

    uint64_t expected = last_recover_ns;
    if (!m_last_stale_recover_ns.compare_exchange_strong(
            expected, now_ns, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }

    const uint32_t attempt =
        m_stale_recover_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
    blog(LOG_WARNING,
         "[obs-zoom-plugin] Recovering %s Zoom video subscription: source=%s uuid=%s age_ms=%llu attempt=%u",
         missing_initial_frame ? "no-signal" : "stale",
         output_name().c_str(), source_uuid.c_str(),
         static_cast<unsigned long long>(age_ns / 1'000'000ULL),
         attempt);
    subscribe();
    return true;
}

bool ZoomSource::upgrade_low_quality_video(uint64_t now_ns, bool force)
{
    if (!m_active.load(std::memory_order_acquire) ||
        !m_subscribed.load(std::memory_order_acquire))
        return false;
    if (!ZoomEngineClient::instance().is_running() ||
        ZoomEngineClient::instance().state() != MeetingState::InMeeting ||
        !ZoomEngineClient::instance().is_media_active())
        return false;

    const uint32_t observed_w = m_width.load(std::memory_order_relaxed);
    const uint32_t observed_h = m_height.load(std::memory_order_relaxed);
    if (observed_w == 0 || observed_h == 0 ||
        observed_satisfies_resolution(observed_w, observed_h, resolution))
        return false;

    const uint32_t requested_w = width_for_resolution(resolution);
    const uint32_t requested_h = height_for_resolution(resolution);
    const uint64_t last_frame_ns =
        m_last_frame_ns.load(std::memory_order_acquire);
    if (last_frame_ns == 0 || now_ns <= last_frame_ns ||
        now_ns - last_frame_ns > kStaleVideoNs)
        return false;

    const uint64_t last_subscribe_ns =
        m_last_subscribe_ns.load(std::memory_order_acquire);
    if (!force && (last_subscribe_ns == 0 || now_ns <= last_subscribe_ns ||
        now_ns - last_subscribe_ns < kQualityUpgradeStableNs))
        return false;

    const uint64_t last_upgrade_ns =
        m_last_quality_upgrade_ns.load(std::memory_order_acquire);
    const uint32_t completed_attempts =
        m_quality_upgrade_attempts.load(std::memory_order_acquire);
    if (!force && last_upgrade_ns != 0 &&
        now_ns - last_upgrade_ns < quality_upgrade_cooldown_ns(completed_attempts))
        return false;

    uint64_t expected = last_upgrade_ns;
    if (!m_last_quality_upgrade_ns.compare_exchange_strong(
            expected, now_ns, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }

    const uint32_t attempt =
        m_quality_upgrade_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
    blog(LOG_INFO,
         "[obs-zoom-plugin] Retrying Zoom video quality upgrade: source=%s uuid=%s observed=%ux%u requested=%ux%u attempt=%u automatic=%s",
         output_name().c_str(), source_uuid.c_str(), observed_w, observed_h,
         requested_w, requested_h, attempt, force ? "false" : "true");
    subscribe();
    return true;
}

// Subscriptions belong to the ASSIGNMENT (the Output Manager row), not to
// scene visibility. Tearing the Zoom subscription down on every scene exit
// and rebuilding it on entry meant a scene jump churned the SDK's raw-data
// pipeline for every CoreVideo source at once — rapid scene switching
// (2026-08-09) drove the SDK into an internal fatal that killed the engine
// process (exit code 1), and every cut showed placeholder frames while
// feeds resubscribed. Feeds now stay warm while the source exists and is
// assigned; only assignment changes, source destruction, or engine
// teardown end a subscription.
void ZoomSource::activate()
{
    m_active.store(true, std::memory_order_release);
    if (m_width.load(std::memory_order_relaxed) == 0)
        output_placeholder_frame(true);
    if (m_subscribed.load(std::memory_order_acquire))
        return; // feed already warm — a scene switch must not touch it
    const AssignmentMode mode = assignment.load(std::memory_order_acquire);
    if (source_wants_subscription(mode, participant_id.load(std::memory_order_acquire)))
        subscribe();
}

void ZoomSource::deactivate()
{
    m_active.store(false, std::memory_order_release);
    // Intentionally NOT unsubscribing: the feed stays warm for the next cut.
}

void ZoomSource::on_roster_changed()
{
    const AssignmentMode mode = assignment.load(std::memory_order_acquire);
    const auto roster = ZoomEngineClient::instance().roster();
    const auto participant_has_video = [&roster](uint32_t id, bool &has_video) {
        const auto it = std::find_if(
            roster.begin(), roster.end(),
            [id](const ParticipantInfo &p) { return p.user_id == id; });
        if (it == roster.end())
            return false;
        has_video = it->has_video;
        return true;
    };
    auto resubscribe_if_video_resumed = [this, &participant_has_video](
        uint32_t target_id, const char *reason) {
        if (target_id == 0) {
            m_last_roster_video_state.store(-1, std::memory_order_release);
            return false;
        }

        bool has_video = false;
        if (!participant_has_video(target_id, has_video)) {
            m_last_roster_video_state.store(-1, std::memory_order_release);
            return false;
        }

        const int previous =
            m_last_roster_video_state.exchange(has_video ? 1 : 0,
                                               std::memory_order_acq_rel);
        if (previous != 0 || !has_video)
            return false;

        const uint64_t now_ns = os_gettime_ns();
        const uint64_t last_frame_ns =
            m_last_frame_ns.load(std::memory_order_acquire);
        const bool missing_or_stale =
            last_frame_ns == 0 ||
            (now_ns > last_frame_ns && now_ns - last_frame_ns > kStaleVideoNs);
        if (!missing_or_stale)
            return false;

        m_last_stale_recover_ns.store(0, std::memory_order_release);
        m_stale_recover_attempts.store(0, std::memory_order_release);
        blog(LOG_INFO,
             "[obs-zoom-plugin] Participant video resumed; resubscribing Zoom source: source=%s uuid=%s participant_id=%u reason=%s",
             output_name().c_str(), source_uuid.c_str(), target_id, reason);
        subscribe();
        return true;
    };

    if (!m_subscribed) {
        if (m_active.load(std::memory_order_acquire) &&
            source_wants_subscription(
                mode, participant_id.load(std::memory_order_acquire)))
            subscribe();
        return;
    }

    // Spotlight & screen-share assignments are resolved by the engine on
    // every roster change; nothing for the plugin to do here.
    if (mode == AssignmentMode::SpotlightIndex) {
        const uint32_t slot = spotlight_slot.load(std::memory_order_acquire);
        const auto it = std::find_if(
            roster.begin(), roster.end(),
            [slot](const ParticipantInfo &p) {
                return p.spotlight_index == slot;
            });
        if (it != roster.end())
            resubscribe_if_video_resumed(it->user_id, "spotlight_video_resumed");
        else
            m_last_roster_video_state.store(-1, std::memory_order_release);
        return;
    }
    if (mode == AssignmentMode::ScreenShare) {
        m_last_roster_video_state.store(-1, std::memory_order_release);
        return;
    }

    if (is_active_speaker_assignment(mode)) {
        if (dedicated_active_speaker_source) {
            maybe_update_director_subscription();
            resubscribe_if_video_resumed(
                m_current_subscription_id.load(std::memory_order_acquire),
                "director_video_resumed");
            return;
        }
        const uint32_t subscribe_id =
            effective_participant_id(participant_id, true);
        if (resubscribe_if_video_resumed(
                subscribe_id, "active_speaker_video_resumed")) {
            return;
        }
        if (subscribe_id == 0 || subscribe_id == m_current_subscription_id) return;
        subscribe();
        return;
    }

    // Failover handling: if our primary participant is no longer in the
    // roster but we have a configured failover, re-subscribe.
    const uint32_t primary = participant_id.load(std::memory_order_acquire);
    if (primary == 0) return;
    if (resubscribe_if_video_resumed(
            m_current_subscription_id.load(std::memory_order_acquire),
            "participant_video_resumed")) {
        return;
    }

    const uint32_t failover = failover_participant_id.load(std::memory_order_acquire);
    if (failover == 0) return;
    const bool primary_present = std::any_of(
        roster.begin(), roster.end(),
        [primary](const ParticipantInfo &p) { return p.user_id == primary; });
    const uint32_t want = primary_present ? primary : failover;
    if (resubscribe_if_video_resumed(want, "participant_video_resumed"))
        return;
    if (want != m_current_subscription_id) subscribe();
}

void ZoomSource::maybe_update_director_subscription()
{
    if (!dedicated_active_speaker_source)
        return;
    if (m_director_preview_uuid.empty())
        m_director_preview_uuid = make_source_uuid();

    // No settings reload here. on_roster_changed() drives this ~9x/second, and
    // ZoomPluginSettings::load() goes through obs_frontend_get_global_config()
    // — an obs_frontend call, off the UI thread, once per roster update. Every
    // one of those re-applied the whole director config, so any momentarily
    // stale read republished a config the dock had already moved on from. The
    // director is configured where it changes instead: apply_settings(),
    // subscribe(), the dock, and the control/OSC servers.
    const uint32_t directed_id = ZoomEngineClient::instance().active_speaker_id();
    const uint32_t current_id =
        m_current_subscription_id.load(std::memory_order_acquire);
    const uint32_t preview_id =
        m_director_preview_subscription_id.load(std::memory_order_acquire);

    if (directed_id == 0 || directed_id == current_id ||
        directed_id == preview_id) {
        return;
    }

    ZoomEngineClient::instance().unsubscribe(m_director_preview_uuid);
    // Between the unsubscribe and the subscribe, so the preview region's name
    // is free when the engine rebuilds it (shm-resubscribe.h). This already
    // dropped the mapping; it now also forgets the generation and takes m_mtx
    // while doing so, because on_director_preview_frame() reads this same
    // region under m_mtx. The lock is scoped to the release alone — m_mtx is
    // never held across the engine writes either side of it.
    release_director_preview_shm();
    ZoomEngineClient::instance().subscribe(m_director_preview_uuid, directed_id,
                                           isolate_audio, audience_audio,
                                           resolution);
    m_director_preview_subscription_id.store(directed_id,
                                             std::memory_order_release);
    blog(LOG_INFO,
         "[obs-zoom-plugin] Active speaker preview subscription: source=%s current=%u next=%u preview_uuid=%s",
         output_name().c_str(), current_id, directed_id,
         m_director_preview_uuid.c_str());
}

void ZoomSource::on_engine_frame(uint32_t event_width, uint32_t event_height,
                                 uint32_t resolved_participant_id,
                                 uint32_t shm_generation)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    output_video_from_shared_memory(source_uuid, m_video_shm, m_video_shm_gen,
                                    m_video_buf, m_scaled_video_buf,
                                    event_width, event_height,
                                    resolved_participant_id, shm_generation,
                                    false);
}

void ZoomSource::on_director_preview_frame(uint32_t event_width,
                                           uint32_t event_height,
                                           uint32_t resolved_participant_id,
                                           uint32_t shm_generation)
{
    // The preview slot only exists to warm the next speaker's region so the cut
    // publishes a real frame instead of a gap. But output_video_from_shared_
    // memory() pushes whatever it is handed straight to the program output, so
    // a frame from anyone other than the participant we are currently waiting
    // on puts the wrong face on air. Two of those arrive routinely: frames
    // still in flight for the previous target after
    // maybe_update_director_subscription() re-points the slot, and frames the
    // engine had already queued when the cut unsubscribed it below. On air that
    // read as a participant flashing up with no speaker change behind it, and
    // when such a late frame carried a different id than the one we had just
    // cut to, the commit below cut straight back to it — the sub-second A->B->A
    // pairs. Publish only the frame that belongs to the live preview target;
    // that frame, and only that frame, is the cut.
    const uint32_t awaited =
        m_director_preview_subscription_id.load(std::memory_order_acquire);
    if (awaited == 0 || resolved_participant_id != awaited) {
        if (cv_zoom_verbose_logging()) {
            blog(LOG_INFO,
                 "[obs-zoom-plugin] Dropped stale director preview frame: source=%s participant=%u awaited=%u",
                 output_name().c_str(), resolved_participant_id, awaited);
        }
        return;
    }
    std::lock_guard<std::mutex> lk(m_mtx);
    output_video_from_shared_memory(m_director_preview_uuid,
                                    m_director_preview_shm,
                                    m_director_preview_shm_gen,
                                    m_director_preview_buf,
                                    m_director_preview_scaled_buf,
                                    event_width, event_height,
                                    resolved_participant_id, shm_generation,
                                    true);
}

bool ZoomSource::output_video_from_shared_memory(
    const std::string &uuid,
    ShmRegion &video_shm,
    uint32_t &video_shm_gen,
    std::vector<uint8_t> &video_buf,
    std::vector<uint8_t> &scaled_video_buf,
    uint32_t event_width,
    uint32_t event_height,
    uint32_t resolved_participant_id,
    uint32_t event_shm_gen,
    bool commit_director_cut)
{
    if (event_width == 0 || event_height == 0 || !source) return false;

    // From generation 2 on, the engine creates the region under a
    // generation-suffixed name so a resize is never blocked by our old
    // mapping. shm_read_i420_frame() handles the reopen; log it first, while
    // the old generation is still readable.
    if (video_shm.ptr && event_shm_gen != 0 && event_shm_gen != video_shm_gen) {
        blog(LOG_INFO,
             "[obs-zoom-plugin] Re-opening video shared memory after engine generation change: source=%s uuid=%s gen %u -> %u",
             output_name().c_str(), uuid.c_str(), video_shm_gen, event_shm_gen);
    }

    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t y_len = 0;
    const ShmFrameRead read_status =
        shm_read_i420_frame(video_shm, IPC_SHM_PREFIX + uuid, event_width,
                            event_height, event_shm_gen, video_shm_gen,
                            video_buf, w, h, y_len);
    if (read_status != ShmFrameRead::Ok) {
        if (m_frame_count == 0) {
            switch (read_status) {
            case ShmFrameRead::OpenFailed:
                blog(LOG_WARNING,
                     "[obs-zoom-plugin] Failed to open video shared memory: source=%s uuid=%s w=%u h=%u bytes=%zu gen=%u",
                     output_name().c_str(), uuid.c_str(), event_width,
                     event_height,
                     sizeof(ShmFrameHeader) +
                         static_cast<size_t>(event_width) * event_height * 3 / 2,
                     event_shm_gen);
                break;
            case ShmFrameRead::TooSmall:
                blog(LOG_WARNING,
                     "[obs-zoom-plugin] Video shared memory too small: source=%s uuid=%s need=%zu have=%zu",
                     output_name().c_str(), uuid.c_str(),
                     sizeof(ShmFrameHeader) + static_cast<size_t>(y_len) +
                         static_cast<size_t>(y_len) / 2,
                     video_shm.size);
                break;
            default:
                // `uuid`, the parameter, not the source_uuid member: this
                // helper also serves the director-preview feed, whose uuid is
                // m_director_preview_uuid. Matches both sibling branches.
                blog(LOG_WARNING,
                     "[obs-zoom-plugin] Incomplete video frame skipped: source=%s uuid=%s w=%u h=%u y_len=%u",
                     output_name().c_str(), uuid.c_str(), w, h, y_len);
                break;
            }
        }
        return false;
    }

    const auto *y_ptr = video_buf.data();
    const auto *u_ptr = y_ptr + y_len;
    const auto *v_ptr = u_ptr + y_len / 4;
    const uint32_t observed_w = w;
    const uint32_t observed_h = h;
    const uint8_t *obs_y_ptr = y_ptr;
    const uint8_t *obs_u_ptr = u_ptr;
    const uint8_t *obs_v_ptr = v_ptr;
    uint32_t obs_w = w;
    uint32_t obs_h = h;
    uint32_t obs_stride_y = w;
    uint32_t obs_stride_uv = w / 2;

    uint32_t canvas_w = width_for_resolution(resolution);
    uint32_t canvas_h = height_for_resolution(resolution);
    if (canvas_w == 0 || canvas_h == 0) {
        canvas_w = kDefaultWidth;
        canvas_h = kDefaultHeight;
    }
    if (canvas_w != w || canvas_h != h) {
        const size_t scaled_size =
            static_cast<size_t>(canvas_w) * canvas_h * 3 / 2;
        if (scaled_video_buf.size() < scaled_size)
            scaled_video_buf.resize(scaled_size);
        scale_i420_letterbox(y_ptr, u_ptr, v_ptr, w, h,
                             scaled_video_buf.data(), canvas_w, canvas_h);
        obs_y_ptr = scaled_video_buf.data();
        obs_u_ptr = obs_y_ptr + static_cast<size_t>(canvas_w) * canvas_h;
        obs_v_ptr = obs_u_ptr + static_cast<size_t>(canvas_w) * canvas_h / 4;
        obs_w = canvas_w;
        obs_h = canvas_h;
        obs_stride_y = canvas_w;
        obs_stride_uv = canvas_w / 2;
    }
    const uint64_t ts = os_gettime_ns();

    // Try hardware-accelerated I420→NV12 conversion; fall back to CPU I420 path.
    obs_source_frame frame = {};
    frame.timestamp = ts;

    auto configure_cpu_frame = [&]() {
        frame.format     = VIDEO_FORMAT_I420;
        frame.width      = obs_w;
        frame.height     = obs_h;
        frame.data[0]    = const_cast<uint8_t *>(obs_y_ptr);
        frame.data[1]    = const_cast<uint8_t *>(obs_u_ptr);
        frame.data[2]    = const_cast<uint8_t *>(obs_v_ptr);
        frame.linesize[0] = obs_stride_y;
        frame.linesize[1] = obs_stride_uv;
        frame.linesize[2] = obs_stride_uv;
    };
#ifdef COREVIDEO_HW_ACCEL
    bool hw_ok = false;
    if (m_hw_pipeline.active_mode() != HwAccelMode::None &&
        obs_w == w && obs_h == h) {
        hw_ok = m_hw_pipeline.process(obs_y_ptr, obs_u_ptr, obs_v_ptr,
                                      static_cast<int>(obs_w), static_cast<int>(obs_h),
                                      static_cast<int>(obs_stride_y),
                                      static_cast<int>(obs_stride_uv),
                                      static_cast<int>(obs_stride_uv), frame);
    }

    if (!hw_ok)
        configure_cpu_frame();
#else
    configure_cpu_frame();
#endif
    set_yuv_frame_color_info(frame);

    obs_source_output_video(source, &frame);
    if (commit_director_cut && dedicated_active_speaker_source) {
        const uint32_t previous_id =
            m_current_subscription_id.load(std::memory_order_acquire);
        if (previous_id != resolved_participant_id) {
            ZoomEngineClient::instance().unsubscribe(source_uuid);
            // The on-air defect (2026-08-10): this cut re-points source_uuid at
            // a new participant, so the engine destroys and rebuilds that
            // uuid's SourceTarget and its SHM region — but we went on holding
            // the old mapping, which blocks the recreate at a larger size and
            // leaves the read side on a region that no longer matches. That is
            // why participants flashed white on every speaker change. Release
            // first; see shm-resubscribe.h.
            //
            // Audio too, and ONLY here. The unsubscribe on the line above makes
            // the engine destroy this uuid's AudioTarget as well, so its region
            // is rebuilt from generation 1 under the legacy unsuffixed
            // "..._audio" name exactly as the video one is. If it needs to grow,
            // our live mapping blocks the create and the source goes
            // permanently silent — a failed ensure_shm() emits no audio event,
            // so nothing would ever prompt us to reopen. See
            // release_audio_shm_locked(). The three sends in subscribe() do not
            // need this: with no unsubscribe the engine updates the AudioTarget
            // in place.
            //
            // _locked: our caller (on_director_preview_frame) holds m_mtx for
            // the whole call, and m_mtx is what guards both regions against the
            // frame/audio callbacks. These only unmap memory — no engine IPC —
            // so they add nothing blocking under the lock. Neither region is
            // the one this frame was read from (that is m_director_preview_shm),
            // so the frame just published above is unaffected.
            release_video_shm_locked();
            release_audio_shm_locked();
            ZoomEngineClient::instance().subscribe(source_uuid,
                                                   resolved_participant_id,
                                                   isolate_audio,
                                                   audience_audio,
                                                   resolution);
            m_current_subscription_id.store(resolved_participant_id,
                                            std::memory_order_release);
            ZoomEngineClient::instance().unsubscribe(m_director_preview_uuid);
            m_director_preview_subscription_id.store(0,
                                                     std::memory_order_release);
            blog(LOG_INFO,
                 "[obs-zoom-plugin] Active speaker clean cut: source=%s from=%u to=%u",
                 output_name().c_str(), previous_id, resolved_participant_id);
        }
    }
    ZoomIsoRecorder::instance().record_video_frame(
        output_info(), resolved_participant_id, w, h, y_ptr, u_ptr, v_ptr,
        w, w / 2, ts);
    ++m_frame_count;
    if (m_fps_window_start_ns == 0) {
        m_fps_window_start_ns = ts;
        m_fps_window_frames = 0;
    }
    ++m_fps_window_frames;
    const uint64_t fps_elapsed_ns = ts - m_fps_window_start_ns;
    if (fps_elapsed_ns >= 1'000'000'000ULL) {
        const double fps = static_cast<double>(m_fps_window_frames) *
            1'000'000'000.0 / static_cast<double>(fps_elapsed_ns);
        m_observed_fps_x100.store(static_cast<uint32_t>(fps * 100.0 + 0.5),
                                  std::memory_order_relaxed);
        m_fps_window_start_ns = ts;
        m_fps_window_frames = 0;
    }
    if (cv_zoom_verbose_logging() &&
        (m_frame_count == 1 || m_frame_count % 120 == 0)) {
        blog(LOG_INFO,
             "[obs-zoom-plugin] Output Zoom video frame: source=%s uuid=%s count=%llu w=%u h=%u fmt=%d y_center=%u y_tl=%u",
             output_name().c_str(), source_uuid.c_str(),
             static_cast<unsigned long long>(m_frame_count), obs_w, obs_h,
             static_cast<int>(frame.format),
             static_cast<unsigned>(sample_center_luma(obs_y_ptr, obs_w, obs_h,
                                                      obs_stride_y)),
             static_cast<unsigned>(obs_y_ptr ? obs_y_ptr[0] : 0));
    }
    m_width.store(observed_w, std::memory_order_relaxed);
    m_height.store(observed_h, std::memory_order_relaxed);
    m_last_frame_ns.store(ts, std::memory_order_relaxed);
    m_last_stale_recover_ns.store(0, std::memory_order_relaxed);
    m_stale_recover_attempts.store(0, std::memory_order_relaxed);
    if (observed_satisfies_resolution(observed_w, observed_h, resolution)) {
        m_last_quality_upgrade_ns.store(0, std::memory_order_relaxed);
        m_quality_upgrade_attempts.store(0, std::memory_order_relaxed);
    }

    const uint64_t now_ns = os_gettime_ns();
    if (m_preview_cb && now_ns - m_preview_last_ns >= kPreviewIntervalNs) {
        m_preview_last_ns = now_ns;
        m_preview_cb(w, h, y_ptr, u_ptr, v_ptr, w, w / 2);
    }
    return true;
}

void ZoomSource::on_engine_audio(uint32_t event_byte_len,
                                 uint32_t resolved_participant_id,
                                 uint32_t event_shm_gen)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    const std::string audio_base = IPC_SHM_PREFIX + source_uuid + "_audio";
    const std::string shm_name = shm_region_name(audio_base, event_shm_gen);
    if (event_byte_len == 0) return;
    const size_t audio_bytes = sizeof(ShmAudioHeader) + event_byte_len;
    const bool gen_changed = event_shm_gen != 0 &&
                             event_shm_gen != m_audio_shm_gen;
    if (m_audio_shm.ptr && gen_changed)
        shm_region_destroy(m_audio_shm);
    if (!m_audio_shm.ptr || m_audio_shm.size < audio_bytes) {
        if (!shm_region_open_read(m_audio_shm, shm_name, audio_bytes) &&
            // Engines predating suffixed names recreate the legacy name for
            // every generation — fall back to it.
            (event_shm_gen <= 1 ||
             !shm_region_open_read(m_audio_shm, audio_base, audio_bytes))) {
            if (m_audio_frame_count == 0) {
                blog(LOG_WARNING,
                     "[obs-zoom-plugin] Failed to open audio shared memory: source=%s uuid=%s bytes=%zu gen=%u",
                     output_name().c_str(), source_uuid.c_str(), audio_bytes,
                     event_shm_gen);
            }
            return;
        }
        m_audio_shm_gen = event_shm_gen;
    }

    auto *hdr = static_cast<const ShmAudioHeader *>(m_audio_shm.ptr);
    uint32_t byte_len = 0;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    bool copied = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t seq1 = hdr->sequence;
        std::atomic_thread_fence(std::memory_order_acquire);
        if ((seq1 & 1u) != 0) continue;
        byte_len = hdr->byte_len;
        sample_rate = hdr->sample_rate;
        channels = hdr->channels;
        if (!source || byte_len == 0) return;
        if (sizeof(ShmAudioHeader) + byte_len > m_audio_shm.size) return;
        const auto *pcm_src = static_cast<const uint8_t *>(m_audio_shm.ptr) +
            sizeof(ShmAudioHeader);
        if (m_audio_buf.size() < byte_len)
            m_audio_buf.resize(byte_len);
        std::memcpy(m_audio_buf.data(), pcm_src, byte_len);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t seq2 = hdr->sequence;
        if (seq1 == seq2 && (seq2 & 1u) == 0) {
            copied = true;
            break;
        }
    }
    if (!copied) return;
    const auto *pcm = reinterpret_cast<const int16_t *>(m_audio_buf.data());
    const uint64_t ts = os_gettime_ns();

    obs_source_audio audio = {};
    audio.samples_per_sec = sample_rate;
    audio.timestamp = ts;

    if (audio_mode == AudioChannelMode::Stereo && channels == 1) {
        const uint32_t mono_frames = byte_len / kZoomBytesPerSample;
        const uint32_t stereo_count = mono_frames * 2;
        if (m_stereo_buf.size() < stereo_count)
            m_stereo_buf.resize(stereo_count);
        for (uint32_t i = 0; i < mono_frames; ++i) {
            m_stereo_buf[i * 2] = pcm[i];
            m_stereo_buf[i * 2 + 1] = pcm[i];
        }
        audio.data[0] = reinterpret_cast<const uint8_t *>(m_stereo_buf.data());
        audio.frames = mono_frames;
        audio.format = AUDIO_FORMAT_16BIT;
        audio.speakers = SPEAKERS_STEREO;
    } else {
        audio.data[0] = reinterpret_cast<const uint8_t *>(pcm);
        audio.frames = byte_len / (kZoomBytesPerSample * std::max<uint16_t>(channels, 1));
        audio.format = AUDIO_FORMAT_16BIT;
        audio.speakers = channels == 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
    }
    obs_source_output_audio(source, &audio);
    ZoomIsoRecorder::instance().record_audio_frame(
        output_info(), resolved_participant_id, m_audio_buf.data(), byte_len,
        sample_rate, channels, ts);
    ++m_audio_frame_count;
    if (cv_zoom_verbose_logging() &&
        (m_audio_frame_count == 1 || m_audio_frame_count % 250 == 0)) {
        int peak = 0;
        const uint32_t sample_count = byte_len / kZoomBytesPerSample;
        for (uint32_t i = 0; i < sample_count; ++i)
            peak = std::max<int>(peak, std::abs(static_cast<int>(pcm[i])));
        blog(LOG_INFO,
             "[obs-zoom-plugin] Output Zoom audio frame: source=%s uuid=%s count=%llu frames=%u sample_rate=%u channels=%u byte_len=%u peak=%d",
             output_name().c_str(), source_uuid.c_str(),
             static_cast<unsigned long long>(m_audio_frame_count),
             audio.frames, sample_rate, channels, byte_len, peak);
    }
}

uint32_t ZoomSource::width() const
{
    const uint32_t configured = width_for_resolution(resolution);
    return configured != 0 ? configured : kDefaultWidth;
}

uint32_t ZoomSource::height() const
{
    const uint32_t configured = height_for_resolution(resolution);
    return configured != 0 ? configured : kDefaultHeight;
}

void ZoomSource::output_placeholder_frame(bool color_bars)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!source) return;

    uint32_t w = width_for_resolution(resolution);
    uint32_t h = height_for_resolution(resolution);
    if (w == 0 || h == 0) {
        w = kDefaultWidth;
        h = kDefaultHeight;
    }

    const size_t y_size = static_cast<size_t>(w) * h;
    const size_t uv_size = y_size / 4;
    const size_t total = y_size + uv_size * 2;
    if (m_placeholder_buf.size() != total)
        m_placeholder_buf.resize(total);

    uint8_t *y_plane = m_placeholder_buf.data();
    uint8_t *u_plane = y_plane + y_size;
    uint8_t *v_plane = u_plane + uv_size;

    if (!color_bars) {
        std::fill(y_plane, y_plane + y_size, 0);
        std::fill(u_plane, u_plane + uv_size, 128);
        std::fill(v_plane, v_plane + uv_size, 128);
    } else {
        struct Bar { uint8_t r, g, b; };
        static constexpr Bar bars[] = {
            {191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
            {191, 0, 191},   {191, 0, 0},   {0, 0, 191},   {0, 0, 0}
        };

        for (uint32_t yy = 0; yy < h; ++yy) {
            for (uint32_t xx = 0; xx < w; ++xx) {
                const Bar &bar = bars[std::min<size_t>(
                    (static_cast<size_t>(xx) * std::size(bars)) / w,
                    std::size(bars) - 1)];
                uint8_t yv, uv, vv;
                rgb_to_i420_values(bar.r, bar.g, bar.b, yv, uv, vv);
                y_plane[static_cast<size_t>(yy) * w + xx] = yv;
            }
        }

        for (uint32_t yy = 0; yy < h / 2; ++yy) {
            for (uint32_t xx = 0; xx < w / 2; ++xx) {
                const Bar &bar = bars[std::min<size_t>(
                    (static_cast<size_t>(xx * 2) * std::size(bars)) / w,
                    std::size(bars) - 1)];
                uint8_t yv, uv, vv;
                rgb_to_i420_values(bar.r, bar.g, bar.b, yv, uv, vv);
                const size_t off = static_cast<size_t>(yy) * (w / 2) + xx;
                u_plane[off] = uv;
                v_plane[off] = vv;
            }
        }
    }

    obs_source_frame frame = {};
    frame.format = VIDEO_FORMAT_I420;
    frame.width = w;
    frame.height = h;
    frame.timestamp = os_gettime_ns();
    frame.data[0] = y_plane;
    frame.data[1] = u_plane;
    frame.data[2] = v_plane;
    frame.linesize[0] = w;
    frame.linesize[1] = w / 2;
    frame.linesize[2] = w / 2;
    set_yuv_frame_color_info(frame);
    obs_source_output_video(source, &frame);
}

void ZoomSource::set_preview_cb(ZoomPreviewCallback cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_preview_cb = std::move(cb);
}

void ZoomSource::clear_preview_cb()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_preview_cb = nullptr;
}

// Teardown: clearing the assignment, and zoom_source_destroy(). Deliberately
// silent, and deliberately NOT routed through the logging _locked forms below.
// Those call output_name() -> obs_source_get_name(), and this runs inside
// obs_source_info::destroy, where the source is mid-teardown. The log line
// would say nothing useful on a path where the source is going away anyway, so
// there is nothing to weigh against touching the source object there.
//
// The engine-restart release is a different case — the trail is the whole point
// there — and has its own entry point below.
void ZoomSource::release_shared_memory()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    shm_release_for_resubscribe(m_video_shm, m_video_shm_gen);
    shm_release_for_resubscribe(m_director_preview_shm, m_director_preview_shm_gen);
    shm_release_for_resubscribe(m_audio_shm, m_audio_shm_gen);
}

// A new ZoomObsEngine process is about to serve this source, and its SHM
// generation table is empty: its first create for any of our three regions asks
// for generation 1, the legacy unsuffixed name, whatever the dead engine had
// climbed to. Every mapping we hold is standing on a name it is about to ask
// for, so all three go — video, director preview and audio alike. Audio matters
// most: no subscribe path releases it, and a blocked audio create publishes no
// event, so nothing would ever prompt us to reopen and the source would stay
// silent for the rest of the session. See ZoomEngineClient::SourceCallbacks.
//
// Logged per region, through the _locked forms. When a source goes quiet after
// an engine restart, "which mappings did we actually drop, and at which
// generation" is the first question and the operator's log has to answer it.
void ZoomSource::release_shared_memory_for_new_engine()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    release_video_shm_locked();
    release_director_preview_shm_locked();
    release_audio_shm_locked();
}

// Each of these logs the generation it dropped. Without that line a release is
// invisible: it nulls the mapping, and the "Re-opening video shared memory
// after engine generation change: gen N -> M" line below is guarded on a
// non-null pointer, so it stops firing. That reopen line is how the 2026-08-10
// white-flash incident was root-caused — keep a trail here or the next
// recurrence has none.
void ZoomSource::release_video_shm_locked()
{
    const uint32_t dropped_gen = m_video_shm_gen;
    if (!shm_release_for_resubscribe(m_video_shm, m_video_shm_gen)) return;
    blog(LOG_INFO,
         "[obs-zoom-plugin] Released video shared memory before re-subscribe: source=%s uuid=%s dropped_gen=%u",
         output_name().c_str(), source_uuid.c_str(), dropped_gen);
}

void ZoomSource::release_video_shm()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    release_video_shm_locked();
}

// Audio has the SAME defect as video, but ONLY on paths that send an explicit
// unsubscribe first. The engine handles Unsubscribe with
// EngineAudio::instance().remove(uuid) (engine/src/main.cpp), which erases the
// AudioTarget and with it its shm_gen (engine/src/engine-audio.cpp). The
// replacement target therefore restarts at 0, and ensure_shm() creates
// generation 1 — the legacy unsuffixed "..._audio" name we are still mapping.
// When the new participant's buffers are larger the create fails, the engine
// emits audio_shm_create_failed, and because a failed ensure_shm() publishes no
// audio event there is nothing left to make us reopen: that source goes
// PERMANENTLY silent. Equal-or-smaller buffers reuse the section and look fine,
// which is why this survives casual testing.
//
// A bare subscribe (no unsubscribe) is different: EngineAudio::subscribe_if_needed()
// updates an existing AudioTarget in place, so nothing is recreated and no
// release is needed. That is why this is NOT called from subscribe().
//
// The other caller is release_shared_memory_for_new_engine(), for the case a
// per-subscribe release could never cover: a new engine PROCESS. Its table
// is empty, so its first create for this region asks for generation 1 — the
// legacy unsuffixed name we are still holding — whatever the previous engine
// had climbed to. See ZoomEngineClient::SourceCallbacks.
void ZoomSource::release_audio_shm_locked()
{
    const uint32_t dropped_gen = m_audio_shm_gen;
    if (!shm_release_for_resubscribe(m_audio_shm, m_audio_shm_gen)) return;
    blog(LOG_INFO,
         "[obs-zoom-plugin] Released audio shared memory before re-subscribe: source=%s uuid=%s dropped_gen=%u",
         output_name().c_str(), source_uuid.c_str(), dropped_gen);
}

void ZoomSource::release_director_preview_shm_locked()
{
    const uint32_t dropped_gen = m_director_preview_shm_gen;
    if (!shm_release_for_resubscribe(m_director_preview_shm,
                                     m_director_preview_shm_gen))
        return;
    blog(LOG_INFO,
         "[obs-zoom-plugin] Released director preview shared memory before re-subscribe: source=%s uuid=%s dropped_gen=%u",
         output_name().c_str(), m_director_preview_uuid.c_str(), dropped_gen);
}

void ZoomSource::release_director_preview_shm()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    release_director_preview_shm_locked();
}

static const char *zoom_source_get_name(void *)
{
    return obs_module_text("ZoomSource.Name");
}

static const char *zoom_active_speaker_source_get_name(void *)
{
    return obs_module_text("ZoomActiveSpeakerSource.Name");
}

static const char *zoom_share_source_get_name(void *)
{
    return obs_module_text("ZoomShareSource.Name");
}

static void *zoom_source_create_common(obs_data_t *settings, obs_source_t *source,
                                       bool dedicated_active_speaker)
{
    auto *ctx = new ZoomSource();
    ctx->source = source;
    ctx->source_uuid = make_source_uuid();
    ctx->dedicated_active_speaker_source = dedicated_active_speaker;
    // Render each frame the instant it arrives instead of buffering. A
    // buffered async source discards and rebuilds its frame buffer every
    // time a scene reactivates it, holding the last frame frozen until the
    // buffer refills — the per-scene-switch freeze/stutter operators saw.
    // Unbuffered also cuts latency, which is the point of this product.
    obs_source_set_async_unbuffered(source, true);
    if (dedicated_active_speaker) {
        ctx->m_director_preview_uuid = make_source_uuid();
        obs_data_set_int(settings, PROP_ASSIGNMENT_MODE,
                         static_cast<int>(AssignmentMode::ActiveSpeaker));
        obs_data_set_bool(settings, PROP_ACTIVE_SPEAKER, true);
        obs_data_set_int(settings, PROP_PARTICIPANT_ID, 0);
    }
    ctx->apply_settings(settings);

    ZoomEngineClient::instance().register_source(ctx->source_uuid, {
        [ctx, gate = ctx->m_callback_gate](uint32_t w, uint32_t h,
                                           uint32_t participant_id,
                                           uint32_t shm_gen) {
            std::lock_guard<std::mutex> callback_lock(gate->mtx);
            if (!gate->alive) return;
            ctx->on_engine_frame(w, h, participant_id, shm_gen);
        },
        [ctx, gate = ctx->m_callback_gate](uint32_t byte_len,
                                           uint32_t participant_id,
                                           uint32_t shm_gen) {
            std::lock_guard<std::mutex> callback_lock(gate->mtx);
            if (!gate->alive) return;
            ctx->on_engine_audio(byte_len, participant_id, shm_gen);
        },
        // A new engine process restarts the SHM generation counter, so every
        // name we hold is one it is about to reuse. Drop all three mappings —
        // audio included, which no subscribe path releases. Same lock order as
        // the two callbacks above: the gate, then the source's own m_mtx inside.
        [ctx, gate = ctx->m_callback_gate]() {
            std::lock_guard<std::mutex> callback_lock(gate->mtx);
            if (!gate->alive) return;
            ctx->release_shared_memory_for_new_engine();
        }
    });
    if (dedicated_active_speaker) {
        ZoomEngineClient::instance().register_source(ctx->m_director_preview_uuid, {
            [ctx, gate = ctx->m_callback_gate](uint32_t w, uint32_t h,
                                               uint32_t participant_id,
                                               uint32_t shm_gen) {
                std::lock_guard<std::mutex> callback_lock(gate->mtx);
                if (!gate->alive) return;
                ctx->on_director_preview_frame(w, h, participant_id, shm_gen);
            },
            [gate = ctx->m_callback_gate](uint32_t, uint32_t, uint32_t) {
                std::lock_guard<std::mutex> callback_lock(gate->mtx);
                if (!gate->alive) return;
                // Audio cuts follow the committed current slot. The preview
                // slot is video-only so it can warm a frame before the cut.
            },
            // Deliberately the same wholesale release as the main uuid above,
            // not a preview-only one. It is idempotent — the second call finds
            // nothing mapped, drops nothing and logs nothing — and registering
            // it on both uuids means neither registration silently depends on
            // the other still existing.
            [ctx, gate = ctx->m_callback_gate]() {
                std::lock_guard<std::mutex> callback_lock(gate->mtx);
                if (!gate->alive) return;
                ctx->release_shared_memory_for_new_engine();
            }
        });
    }
    ZoomEngineClient::instance().add_roster_callback(ctx,
        [ctx, gate = ctx->m_callback_gate]() {
            std::lock_guard<std::mutex> callback_lock(gate->mtx);
            if (!gate->alive) return;
            ctx->on_roster_changed();
        });
    ZoomOutputManager::instance().register_source(ctx);

    const ZoomPluginSettings s = ZoomPluginSettings::load();

    {
        const HwAccelMode effective = ctx->hw_accel_override >= 0
            ? static_cast<HwAccelMode>(ctx->hw_accel_override)
            : s.hw_accel_mode;
        if (effective != HwAccelMode::None)
            ctx->m_hw_pipeline.init(effective);
    }

    // ── Register per-source OBS hotkeys ────────────────────────────────────
    ctx->m_hk_active_on_id = obs_hotkey_register_source(source,
        "ZoomSource.Hotkey.ActiveOn",
        obs_module_text("ZoomSource.Hotkey.ActiveOn"),
        [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
            if (!pressed) return;
            auto *c = static_cast<ZoomSource *>(data);
            c->configure_output_ex(AssignmentMode::ActiveSpeaker, 0, 1,
                                   c->failover_participant_id.load(),
                                   c->isolate_audio, c->audio_mode);
        }, ctx);
    ctx->m_hk_active_off_id = obs_hotkey_register_source(source,
        "ZoomSource.Hotkey.ActiveOff",
        obs_module_text("ZoomSource.Hotkey.ActiveOff"),
        [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
            if (!pressed) return;
            auto *c = static_cast<ZoomSource *>(data);
            c->configure_output_ex(AssignmentMode::Participant,
                                   c->participant_id.load(), 1,
                                   c->failover_participant_id.load(),
                                   c->isolate_audio, c->audio_mode);
        }, ctx);

    return ctx;
}

static void *zoom_source_create(obs_data_t *settings, obs_source_t *source)
{
    return zoom_source_create_common(settings, source, false);
}

static void *zoom_active_speaker_source_create(obs_data_t *settings,
                                               obs_source_t *source)
{
    return zoom_source_create_common(settings, source, true);
}

static void *zoom_share_source_create(obs_data_t *settings, obs_source_t *source)
{
    obs_data_set_int(settings, PROP_ASSIGNMENT_MODE,
                     static_cast<int>(AssignmentMode::ScreenShare));
    obs_data_set_int(settings, PROP_PARTICIPANT_ID, 0);
    obs_data_set_string(settings, PROP_OUTPUT_DISPLAY_NAME,
                        "Zoom Screen Share");
    return zoom_source_create_common(settings, source, false);
}

static void zoom_source_destroy(void *data)
{
    auto *ctx = static_cast<ZoomSource *>(data);
    ctx->clear_preview_cb();
    {
        std::lock_guard<std::mutex> callback_lock(ctx->m_callback_gate->mtx);
        ctx->m_callback_gate->alive = false;
    }
    ZoomEngineClient::instance().remove_roster_callback(ctx);
    if (!ctx->m_director_preview_uuid.empty())
        ZoomEngineClient::instance().unregister_source(ctx->m_director_preview_uuid);
    ZoomEngineClient::instance().unregister_source(ctx->source_uuid);
    if (ctx->m_hk_active_on_id  != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->m_hk_active_on_id);
    if (ctx->m_hk_active_off_id != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->m_hk_active_off_id);
    ZoomOutputManager::instance().unregister_source(ctx);
    ZoomIsoRecorder::instance().on_output_removed(ctx->source_uuid);
    ctx->unsubscribe();
    ctx->release_shared_memory();
    ctx->m_hw_pipeline.shutdown();
    delete ctx;
}

static void zoom_source_update(void *data, obs_data_t *settings)
{
    static_cast<ZoomSource *>(data)->apply_settings(settings);
}

static void zoom_source_activate(void *data)
{
    static_cast<ZoomSource *>(data)->activate();
}

static void zoom_source_deactivate(void *data)
{
    static_cast<ZoomSource *>(data)->deactivate();
}

static uint32_t zoom_source_get_width(void *data)
{
    return static_cast<ZoomSource *>(data)->width();
}

static uint32_t zoom_source_get_height(void *data)
{
    return static_cast<ZoomSource *>(data)->height();
}

static obs_properties_t *zoom_source_get_properties(void *data)
{
    auto *ctx = static_cast<ZoomSource *>(data);
    obs_properties_t *props = obs_properties_create();

    obs_property_t *status_prop = obs_properties_add_text(
        props, PROP_STATUS, obs_module_text("ZoomSource.Status"), OBS_TEXT_DEFAULT);
    obs_data_t *settings = obs_source_get_settings(ctx->source);
    obs_data_set_string(settings, PROP_STATUS,
                        obs_module_text(meeting_state_to_text(
                            ZoomEngineClient::instance().state())));
    obs_data_release(settings);
    obs_property_set_enabled(status_prop, false);

    obs_property_t *live_status_prop = obs_properties_add_text(
        props, PROP_LIVE_STATUS, obs_module_text("ZoomSource.LiveStatus"),
        OBS_TEXT_MULTILINE);
    obs_data_t *live_settings = obs_source_get_settings(ctx->source);
    obs_data_set_string(live_settings, PROP_LIVE_STATUS,
                        live_status_text(ctx->output_info()).c_str());
    obs_data_release(live_settings);
    obs_property_set_enabled(live_status_prop, false);

    obs_properties_add_text(props, PROP_OUTPUT_DISPLAY_NAME,
        obs_module_text("ZoomSource.OutputDisplayName"), OBS_TEXT_DEFAULT);

    if (ctx->dedicated_active_speaker_source) {
        obs_property_t *mode_note = obs_properties_add_text(
            props, "active_speaker_note",
            obs_module_text("ZoomActiveSpeakerSource.Mode"),
            OBS_TEXT_DEFAULT);
        obs_data_t *settings_note = obs_source_get_settings(ctx->source);
        obs_data_set_string(settings_note, "active_speaker_note",
                            "Follows the CoreVideo Active Speaker Director");
        obs_data_release(settings_note);
        obs_property_set_enabled(mode_note, false);
    }

    obs_property_t *participant = obs_properties_add_list(props, PROP_PARTICIPANT_ID,
        obs_module_text("ZoomSource.ParticipantId"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(participant, obs_module_text("ZoomSource.NoParticipant"), 0);
    for (const auto &p : ZoomEngineClient::instance().roster()) {
        std::string label = p.display_name.empty()
            ? "ID " + std::to_string(p.user_id)
            : p.display_name + " (" + std::to_string(p.user_id) + ")";
        if (p.is_talking) label += " *";
        obs_property_list_add_int(participant, label.c_str(),
                                  static_cast<long long>(p.user_id));
    }
    obs_property_t *legacy_active = obs_properties_add_bool(props, PROP_ACTIVE_SPEAKER,
        obs_module_text("ZoomSource.ActiveSpeaker"));
    obs_properties_add_int(props, PROP_SPEAKER_SENSITIVITY,
        obs_module_text("ZoomSource.SpeakerSensitivity"), 0, 3000, 50);
    obs_properties_add_int(props, PROP_SPEAKER_HOLD,
        obs_module_text("ZoomSource.SpeakerHold"), 0, 10000, 100);

    obs_property_t *exclude_1 = obs_properties_add_list(
        props, PROP_SPEAKER_EXCLUDE_1,
        obs_module_text("ZoomSource.SpeakerExclude1"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_t *exclude_2 = obs_properties_add_list(
        props, PROP_SPEAKER_EXCLUDE_2,
        obs_module_text("ZoomSource.SpeakerExclude2"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(exclude_1,
        obs_module_text("ZoomSource.SpeakerExcludeNone"), 0);
    obs_property_list_add_int(exclude_2,
        obs_module_text("ZoomSource.SpeakerExcludeNone"), 0);
    for (const auto &p : ZoomEngineClient::instance().roster()) {
        std::string label = p.display_name.empty()
            ? "ID " + std::to_string(p.user_id)
            : p.display_name + " (" + std::to_string(p.user_id) + ")";
        obs_property_list_add_int(exclude_1, label.c_str(),
                                  static_cast<long long>(p.user_id));
        obs_property_list_add_int(exclude_2, label.c_str(),
                                  static_cast<long long>(p.user_id));
    }

    // ── ZoomISO-style assignment options ─────────────────────────────────────
    obs_property_t *amode = obs_properties_add_list(props, PROP_ASSIGNMENT_MODE,
        obs_module_text("ZoomSource.AssignmentMode"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(amode,
        obs_module_text("ZoomSource.Assignment.Participant"),
        static_cast<long long>(AssignmentMode::Participant));
    obs_property_list_add_int(amode,
        obs_module_text("ZoomSource.Assignment.ActiveSpeaker"),
        static_cast<long long>(AssignmentMode::ActiveSpeaker));
    obs_property_list_add_int(amode,
        obs_module_text("ZoomSource.Assignment.SpotlightIndex"),
        static_cast<long long>(AssignmentMode::SpotlightIndex));
    obs_property_list_add_int(amode,
        obs_module_text("ZoomSource.Assignment.ScreenShare"),
        static_cast<long long>(AssignmentMode::ScreenShare));
    obs_property_set_modified_callback(amode,
        [](obs_properties_t *props, obs_property_t *, obs_data_t *settings) -> bool {
            return update_assignment_property_visibility(props, settings);
        });

    obs_property_t *spotlight_slot = obs_properties_add_int(props, PROP_SPOTLIGHT_SLOT,
        obs_module_text("ZoomSource.SpotlightSlot"), 1, 8, 1);

    obs_property_t *failover = obs_properties_add_list(props, PROP_FAILOVER_PARTICIPANT,
        obs_module_text("ZoomSource.Failover"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(failover,
        obs_module_text("ZoomSource.NoFailover"), 0);
    for (const auto &p : ZoomEngineClient::instance().roster()) {
        std::string label = p.display_name.empty()
            ? "ID " + std::to_string(p.user_id)
            : p.display_name + " (" + std::to_string(p.user_id) + ")";
        obs_property_list_add_int(failover, label.c_str(),
                                  static_cast<long long>(p.user_id));
    }
    obs_property_t *active_audio_role = nullptr;
    if (ctx->dedicated_active_speaker_source) {
        active_audio_role = obs_properties_add_list(props, PROP_ACTIVE_AUDIO_ROLE,
            obs_module_text("ZoomSource.ActiveSpeakerAudioRole"),
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        obs_property_list_add_int(active_audio_role,
            obs_module_text("ZoomSource.ActiveSpeakerAudioRole.Mixed"),
            ACTIVE_AUDIO_MIX);
        obs_property_list_add_int(active_audio_role,
            obs_module_text("ZoomSource.ActiveSpeakerAudioRole.Isolated"),
            ACTIVE_AUDIO_ISOLATED);
        obs_property_list_add_int(active_audio_role,
            obs_module_text("ZoomSource.ActiveSpeakerAudioRole.Audience"),
            ACTIVE_AUDIO_AUDIENCE);
        obs_data_t *role_settings = obs_source_get_settings(ctx->source);
        obs_data_set_int(role_settings, PROP_ACTIVE_AUDIO_ROLE,
                         active_audio_role_from_settings(role_settings));
        obs_data_release(role_settings);
        obs_property_set_long_description(active_audio_role,
            "Choose what audio follows this Active Speaker source. Use Audience fallback for a residual mic that ignores isolated host/question-reader sources.");
    } else {
        // Mutually exclusive audio routing toggles. Default is the full meeting
        // mix; isolate plays a single participant; audience plays the residual
        // active speaker (everyone not bound to an isolate target). A modified
        // callback enforces "only one checked" for clarity, with isolate winning
        // server-side if both somehow get set.
        obs_property_t *iso_prop = obs_properties_add_bool(props, PROP_ISOLATE_AUDIO,
            obs_module_text("ZoomSource.IsolateAudio"));
        obs_property_t *aud_prop = obs_properties_add_bool(props, PROP_AUDIENCE_AUDIO,
            obs_module_text("ZoomSource.AudienceAudio"));
        obs_property_set_modified_callback(iso_prop,
            [](obs_properties_t *, obs_property_t *, obs_data_t *settings) -> bool {
                if (obs_data_get_bool(settings, PROP_ISOLATE_AUDIO))
                    obs_data_set_bool(settings, PROP_AUDIENCE_AUDIO, false);
                return false;
            });
        obs_property_set_modified_callback(aud_prop,
            [](obs_properties_t *, obs_property_t *, obs_data_t *settings) -> bool {
                if (obs_data_get_bool(settings, PROP_AUDIENCE_AUDIO))
                    obs_data_set_bool(settings, PROP_ISOLATE_AUDIO, false);
                return false;
            });
    }

    obs_property_t *ch = obs_properties_add_list(props, PROP_AUDIO_CHANNELS,
        obs_module_text("ZoomSource.AudioChannels"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(ch, obs_module_text("ZoomSource.AudioMono"), AUDIO_CH_MONO);
    obs_property_list_add_int(ch, obs_module_text("ZoomSource.AudioStereo"), AUDIO_CH_STEREO);

    obs_property_t *res = obs_properties_add_list(props, PROP_RESOLUTION,
        obs_module_text("ZoomSource.Resolution"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(res, obs_module_text("ZoomSource.Resolution360P"), RES_360P);
    obs_property_list_add_int(res, obs_module_text("ZoomSource.Resolution720P"), RES_720P);
    obs_property_list_add_int(res, obs_module_text("ZoomSource.Resolution1080P"), RES_1080P);

    obs_property_t *loss = obs_properties_add_list(props, PROP_VIDEO_LOSS_MODE,
        obs_module_text("ZoomSource.VideoLossMode"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(loss, obs_module_text("ZoomSource.VideoLossLastFrame"),
                              VIDEO_LOSS_LAST_FRAME);
    obs_property_list_add_int(loss, obs_module_text("ZoomSource.VideoLossBlack"),
                              VIDEO_LOSS_BLACK);

    obs_property_t *hw = obs_properties_add_list(props, PROP_HW_ACCEL_MODE,
        obs_module_text("ZoomSource.HwAccelMode"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
#ifdef COREVIDEO_HW_ACCEL
    obs_property_list_add_int(hw, obs_module_text("ZoomSource.HwAccelGlobal"),  -1);
    obs_property_list_add_int(hw, obs_module_text("ZoomSource.HwAccelNone"),
                              static_cast<int>(HwAccelMode::None));
    obs_property_list_add_int(hw, obs_module_text("ZoomSource.HwAccelAuto"),
                              static_cast<int>(HwAccelMode::Auto));
    obs_property_list_add_int(hw, obs_module_text("ZoomSource.HwAccelCuda"),
                              static_cast<int>(HwAccelMode::Cuda));
    obs_property_list_add_int(hw, obs_module_text("ZoomSource.HwAccelVaapi"),
                              static_cast<int>(HwAccelMode::Vaapi));
    obs_property_list_add_int(hw, obs_module_text("ZoomSource.HwAccelVideoToolbox"),
                              static_cast<int>(HwAccelMode::VideoToolbox));
    obs_property_list_add_int(hw, obs_module_text("ZoomSource.HwAccelQsv"),
                              static_cast<int>(HwAccelMode::Qsv));
#else
    // Plugin compiled without ENABLE_FFMPEG_HW_ACCEL — no backend can run.
    // Show a single read-only entry and disable the control so the operator
    // doesn't pick a mode that will silently fall back to CPU.
    obs_property_list_add_int(hw, "Unavailable (build without ENABLE_FFMPEG_HW_ACCEL)",
                              static_cast<int>(HwAccelMode::None));
    obs_property_set_enabled(hw, false);
    obs_property_set_long_description(hw,
        "Rebuild the plugin with -DENABLE_FFMPEG_HW_ACCEL=ON and FFmpeg "
        "dev libraries present to enable hardware video conversion.");
#endif

    obs_properties_add_button(props, "btn_refresh",
        obs_module_text("ZoomSource.RefreshParticipants"),
        [](obs_properties_t *, obs_property_t *, void *) -> bool { return true; });

    if (ctx->dedicated_active_speaker_source) {
        obs_property_set_visible(participant, false);
        obs_property_set_visible(legacy_active, false);
        obs_property_set_visible(amode, false);
        obs_property_set_visible(spotlight_slot, false);
        obs_property_set_visible(failover, false);
    } else {
        obs_property_set_visible(legacy_active, false);
        obs_data_t *visibility_settings = obs_source_get_settings(ctx->source);
        update_assignment_property_visibility(props, visibility_settings);
        obs_data_release(visibility_settings);
    }

    return props;
}

static void zoom_source_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_bool(settings, PROP_ACTIVE_SPEAKER, false);
    obs_data_set_default_int(settings, PROP_SPEAKER_SENSITIVITY, 300);
    obs_data_set_default_int(settings, PROP_SPEAKER_HOLD, 2000);
    const ZoomPluginSettings plugin_settings = ZoomPluginSettings::load();
    obs_data_set_default_int(settings, PROP_SPEAKER_EXCLUDE_1,
                             plugin_settings.speaker_exclude_participant_1);
    obs_data_set_default_int(settings, PROP_SPEAKER_EXCLUDE_2,
                             plugin_settings.speaker_exclude_participant_2);
    obs_data_set_default_bool(settings, PROP_ISOLATE_AUDIO, false);
    obs_data_set_default_bool(settings, PROP_AUDIENCE_AUDIO, false);
    obs_data_set_default_int(settings, PROP_ACTIVE_AUDIO_ROLE, ACTIVE_AUDIO_MIX);
    obs_data_set_default_int(settings, PROP_PARTICIPANT_ID, 0);
    obs_data_set_default_int(settings, PROP_AUDIO_CHANNELS, AUDIO_CH_MONO);
    obs_data_set_default_int(settings, PROP_RESOLUTION, RES_1080P);
    obs_data_set_default_int(settings, PROP_VIDEO_LOSS_MODE, VIDEO_LOSS_LAST_FRAME);
    obs_data_set_default_int(settings, PROP_HW_ACCEL_MODE, -1);  // use global
    obs_data_set_default_int(settings, PROP_ASSIGNMENT_MODE,
                             static_cast<int>(AssignmentMode::Participant));
    obs_data_set_default_int(settings, PROP_SPOTLIGHT_SLOT, 1);
    obs_data_set_default_int(settings, PROP_FAILOVER_PARTICIPANT, 0);
}

static void zoom_active_speaker_source_get_defaults(obs_data_t *settings)
{
    zoom_source_get_defaults(settings);
    obs_data_set_default_bool(settings, PROP_ACTIVE_SPEAKER, true);
    obs_data_set_default_int(settings, PROP_ASSIGNMENT_MODE,
                             static_cast<int>(AssignmentMode::ActiveSpeaker));
    obs_data_set_default_string(settings, PROP_OUTPUT_DISPLAY_NAME,
                                "CoreVideo Active Speaker");
}

static void zoom_share_source_get_defaults(obs_data_t *settings)
{
    zoom_source_get_defaults(settings);
    obs_data_set_default_int(settings, PROP_ASSIGNMENT_MODE,
                             static_cast<int>(AssignmentMode::ScreenShare));
    obs_data_set_default_int(settings, PROP_PARTICIPANT_ID, 0);
    obs_data_set_default_string(settings, PROP_OUTPUT_DISPLAY_NAME,
                                "Zoom Screen Share");
}

void zoom_source_register()
{
    obs_source_info info = {};
    info.id = "zoom_participant_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = zoom_source_get_name;
    info.create = zoom_source_create;
    info.destroy = zoom_source_destroy;
    info.update = zoom_source_update;
    info.activate = zoom_source_activate;
    info.deactivate = zoom_source_deactivate;
    info.get_width = zoom_source_get_width;
    info.get_height = zoom_source_get_height;
    info.get_properties = zoom_source_get_properties;
    info.get_defaults = zoom_source_get_defaults;
    obs_register_source(&info);

    obs_source_info active_info = info;
    active_info.id = "corevideo_active_speaker_source";
    active_info.get_name = zoom_active_speaker_source_get_name;
    active_info.create = zoom_active_speaker_source_create;
    active_info.get_defaults = zoom_active_speaker_source_get_defaults;
    obs_register_source(&active_info);

    obs_source_info share_info = info;
    share_info.id = "zoom_share_source";
    share_info.get_name = zoom_share_source_get_name;
    share_info.create = zoom_share_source_create;
    share_info.get_defaults = zoom_share_source_get_defaults;
    obs_register_source(&share_info);
}
