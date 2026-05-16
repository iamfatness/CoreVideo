#include "zoom-source.h"
#include "zoom-engine-client.h"
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
#define PROP_VIDEO_LOSS_MODE      "video_loss_mode"
#define PROP_ISOLATE_AUDIO        "isolate_audio"
#define PROP_AUDIO_CHANNELS       "audio_channels"
#define PROP_RESOLUTION           "resolution"
#define PROP_STATUS               "status_label"
#define PROP_HW_ACCEL_MODE        "hw_accel_mode"
#define PROP_ASSIGNMENT_MODE      "assignment_mode"
#define PROP_SPOTLIGHT_SLOT       "spotlight_slot"
#define PROP_FAILOVER_PARTICIPANT "failover_participant_id"

#define VIDEO_LOSS_LAST_FRAME 0
#define VIDEO_LOSS_BLACK      1
#define AUDIO_CH_MONO   0
#define AUDIO_CH_STEREO 1
#define RES_360P  0
#define RES_720P  1
#define RES_1080P 2

static constexpr uint32_t kDefaultWidth = 1280;
static constexpr uint32_t kDefaultHeight = 720;

static std::atomic<uint64_t> s_source_counter{1};
static constexpr uint32_t kZoomBytesPerSample = sizeof(int16_t);
static constexpr uint64_t kPreviewIntervalNs = 200'000'000ULL;

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

static uint8_t clamp_u8(int value)
{
    return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

static void rgb_to_i420_values(uint8_t r, uint8_t g, uint8_t b,
                               uint8_t &y, uint8_t &u, uint8_t &v)
{
    y = clamp_u8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    u = clamp_u8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
    v = clamp_u8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
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
    frame.full_range = false;
    video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_PARTIAL,
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

    output_display_name = obs_data_get_string(settings, PROP_OUTPUT_DISPLAY_NAME);
    participant_id = static_cast<uint32_t>(obs_data_get_int(settings, PROP_PARTICIPANT_ID));
    const bool legacy_active_speaker = obs_data_get_bool(settings, PROP_ACTIVE_SPEAKER);
    speaker_sensitivity_ms = static_cast<uint32_t>(
        obs_data_get_int(settings, PROP_SPEAKER_SENSITIVITY));
    speaker_hold_ms = static_cast<uint32_t>(obs_data_get_int(settings, PROP_SPEAKER_HOLD));
    isolate_audio = obs_data_get_bool(settings, PROP_ISOLATE_AUDIO);
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
        old_failover != new_failover)) {
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
}

std::string ZoomSource::output_name() const
{
    const char *name = source ? obs_source_get_name(source) : nullptr;
    return name ? name : std::string{};
}

ZoomOutputInfo ZoomSource::output_info() const
{
    ZoomOutputInfo info;
    info.source_name = output_name();
    info.display_name = output_display_name;
    info.participant_id = participant_id;
    const AssignmentMode mode = assignment.load(std::memory_order_acquire);
    info.active_speaker = is_active_speaker_assignment(mode);
    info.isolate_audio = isolate_audio;
    info.audio_mode = audio_mode;
    info.video_resolution = resolution;
    info.observed_width = m_width.load(std::memory_order_relaxed);
    info.observed_height = m_height.load(std::memory_order_relaxed);
    info.observed_fps =
        static_cast<double>(m_observed_fps_x100.load(std::memory_order_relaxed)) / 100.0;
    info.assignment = mode;
    info.spotlight_slot = spotlight_slot.load(std::memory_order_acquire);
    info.failover_participant_id = failover_participant_id.load(std::memory_order_acquire);
    return info;
}

void ZoomSource::configure_output(uint32_t new_participant_id,
                                  bool new_active_speaker_mode,
                                  bool new_isolate_audio,
                                  AudioChannelMode new_audio_mode,
                                  VideoResolution new_resolution)
{
    if (source) {
        obs_data_t *settings = obs_source_get_settings(source);
        const AssignmentMode mode = new_active_speaker_mode
            ? AssignmentMode::ActiveSpeaker
            : AssignmentMode::Participant;
        obs_data_set_int(settings, PROP_ASSIGNMENT_MODE, static_cast<int>(mode));
        obs_data_set_int(settings, PROP_PARTICIPANT_ID, new_participant_id);
        obs_data_set_bool(settings, PROP_ACTIVE_SPEAKER, new_active_speaker_mode);
        obs_data_set_bool(settings, PROP_ISOLATE_AUDIO, new_isolate_audio);
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
                                     VideoResolution new_resolution)
{
    const bool active_speaker = (mode == AssignmentMode::ActiveSpeaker);
    if (source) {
        obs_data_t *settings = obs_source_get_settings(source);
        obs_data_set_int(settings, PROP_ASSIGNMENT_MODE, static_cast<int>(mode));
        obs_data_set_int(settings, PROP_PARTICIPANT_ID, new_participant_id);
        obs_data_set_bool(settings, PROP_ACTIVE_SPEAKER, active_speaker);
        obs_data_set_int(settings, PROP_SPOTLIGHT_SLOT, new_spotlight_slot);
        obs_data_set_int(settings, PROP_FAILOVER_PARTICIPANT, new_failover_participant_id);
        obs_data_set_bool(settings, PROP_ISOLATE_AUDIO, new_isolate_audio);
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
    audio_mode = new_audio_mode;
    resolution = new_resolution;
    if (m_subscribed) subscribe();
}

void ZoomSource::subscribe()
{
    if (source_uuid.empty()) source_uuid = make_source_uuid();

    const AssignmentMode mode = assignment.load(std::memory_order_acquire);
    switch (mode) {
    case AssignmentMode::SpotlightIndex: {
        const uint32_t slot = spotlight_slot.load(std::memory_order_acquire);
        ZoomEngineClient::instance().subscribe_spotlight(source_uuid, slot);
        // Use a sentinel so on_roster_changed re-subscribes only when we
        // actually want to change the dispatch.
        m_current_subscription_id = 0xFFFF0000u | slot;
        m_subscribed = true;
        return;
    }
    case AssignmentMode::ScreenShare:
        ZoomEngineClient::instance().subscribe_screenshare(source_uuid);
        m_current_subscription_id = 0xFFFFFFFFu;
        m_subscribed = true;
        return;
    case AssignmentMode::ActiveSpeaker:
    case AssignmentMode::Participant:
    default: {
        // Resolve target participant. If primary is missing from the current
        // roster and we have a failover ID, use it.
        const bool use_active_speaker = is_active_speaker_assignment(mode);
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
        if (target != 0)
            ZoomEngineClient::instance().subscribe(source_uuid, target,
                                                   isolate_audio, resolution);
        blog(LOG_INFO,
             "[obs-zoom-plugin] Zoom source subscription: source=%s uuid=%s participant_id=%u",
             output_name().c_str(), source_uuid.c_str(), target);
        m_current_subscription_id = target;
        m_subscribed = target != 0 || use_active_speaker;
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
    ZoomEngineClient::instance().unsubscribe(source_uuid);
    m_subscribed = false;
    m_current_subscription_id = 0;
}

void ZoomSource::activate()
{
    m_active.store(true, std::memory_order_release);
    if (m_width.load(std::memory_order_relaxed) == 0)
        output_placeholder_frame(true);
    const AssignmentMode mode = assignment.load(std::memory_order_acquire);
    if (source_wants_subscription(mode, participant_id.load(std::memory_order_acquire)))
        subscribe();
}

void ZoomSource::deactivate()
{
    m_active.store(false, std::memory_order_release);
    unsubscribe();
}

void ZoomSource::on_roster_changed()
{
    if (!m_subscribed) return;

    const AssignmentMode mode = assignment.load(std::memory_order_acquire);
    // Spotlight & screen-share assignments are resolved by the engine on
    // every roster change; nothing for the plugin to do here.
    if (mode == AssignmentMode::SpotlightIndex ||
        mode == AssignmentMode::ScreenShare) return;

    if (is_active_speaker_assignment(mode)) {
        const uint32_t subscribe_id =
            effective_participant_id(participant_id, true);
        if (subscribe_id == 0 || subscribe_id == m_current_subscription_id) return;
        subscribe();
        return;
    }

    // Failover handling: if our primary participant is no longer in the
    // roster but we have a configured failover, re-subscribe.
    const uint32_t failover = failover_participant_id.load(std::memory_order_acquire);
    if (failover == 0) return;
    const uint32_t primary = participant_id.load(std::memory_order_acquire);
    if (primary == 0) return;

    const auto roster = ZoomEngineClient::instance().roster();
    const bool primary_present = std::any_of(
        roster.begin(), roster.end(),
        [primary](const ParticipantInfo &p) { return p.user_id == primary; });
    const uint32_t want = primary_present ? primary : failover;
    if (want != m_current_subscription_id) subscribe();
}

void ZoomSource::on_engine_frame(uint32_t event_width, uint32_t event_height)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    const std::string shm_name = IPC_SHM_PREFIX + source_uuid;
    if (event_width == 0 || event_height == 0) return;
    const size_t frame_bytes = sizeof(ShmFrameHeader) +
        static_cast<size_t>(event_width) * event_height * 3 / 2;
    if ((!m_video_shm.ptr || m_video_shm.size < frame_bytes) &&
        !shm_region_open_read(m_video_shm, shm_name, frame_bytes)) {
        if (m_frame_count == 0) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] Failed to open video shared memory: source=%s uuid=%s w=%u h=%u bytes=%zu",
                 output_name().c_str(), source_uuid.c_str(), event_width,
                 event_height, frame_bytes);
        }
        return;
    }

    auto *hdr = static_cast<const ShmFrameHeader *>(m_video_shm.ptr);
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t y_len = 0;
    bool copied = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t seq1 = hdr->sequence;
        std::atomic_thread_fence(std::memory_order_acquire);
        if ((seq1 & 1u) != 0) continue;
        w = hdr->width;
        h = hdr->height;
        y_len = hdr->y_len;
        if (!source || w == 0 || h == 0 || y_len != w * h) break;
        const size_t needed_bytes = sizeof(ShmFrameHeader) +
            static_cast<size_t>(y_len) + static_cast<size_t>(y_len) / 2;
        if (needed_bytes > m_video_shm.size) {
            if (m_frame_count == 0) {
                blog(LOG_WARNING,
                     "[obs-zoom-plugin] Video shared memory too small: source=%s uuid=%s need=%zu have=%zu",
                     output_name().c_str(), source_uuid.c_str(), needed_bytes,
                     m_video_shm.size);
            }
            return;
        }
        const auto *pixels = static_cast<const uint8_t *>(m_video_shm.ptr) +
            sizeof(ShmFrameHeader);
        const size_t payload_size = static_cast<size_t>(y_len) +
            static_cast<size_t>(y_len) / 2;
        if (m_video_buf.size() < payload_size)
            m_video_buf.resize(payload_size);
        std::memcpy(m_video_buf.data(), pixels, payload_size);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t seq2 = hdr->sequence;
        if (seq1 == seq2 && (seq2 & 1u) == 0) {
            copied = true;
            break;
        }
    }
    if (!copied) {
        if (m_frame_count == 0) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] Incomplete video frame skipped: source=%s uuid=%s w=%u h=%u y_len=%u",
                 output_name().c_str(), source_uuid.c_str(), w, h, y_len);
        }
        return;
    }

    const auto *y_ptr = m_video_buf.data();
    const auto *u_ptr = y_ptr + y_len;
    const auto *v_ptr = u_ptr + y_len / 4;
    const uint64_t ts = os_gettime_ns();

    // Try hardware-accelerated I420→NV12 conversion; fall back to CPU I420 path.
    obs_source_frame frame = {};
    frame.timestamp = ts;

    bool hw_ok = false;
#ifdef COREVIDEO_HW_ACCEL
    if (m_hw_pipeline.active_mode() != HwAccelMode::None) {
        hw_ok = m_hw_pipeline.process(y_ptr, u_ptr, v_ptr,
                                      static_cast<int>(w), static_cast<int>(h),
                                      static_cast<int>(w), static_cast<int>(w / 2),
                                      static_cast<int>(w / 2), frame);
    }
#endif

    if (!hw_ok) {
        frame.format     = VIDEO_FORMAT_I420;
        frame.width      = w;
        frame.height     = h;
        frame.data[0]    = const_cast<uint8_t *>(y_ptr);
        frame.data[1]    = const_cast<uint8_t *>(u_ptr);
        frame.data[2]    = const_cast<uint8_t *>(v_ptr);
        frame.linesize[0] = w;
        frame.linesize[1] = w / 2;
        frame.linesize[2] = w / 2;
    }
    set_yuv_frame_color_info(frame);

    obs_source_output_video(source, &frame);
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
    if (m_frame_count == 1 || m_frame_count % 120 == 0) {
        blog(LOG_INFO,
             "[obs-zoom-plugin] Output Zoom video frame: source=%s uuid=%s count=%llu w=%u h=%u fmt=%d y_center=%u y_tl=%u",
             output_name().c_str(), source_uuid.c_str(),
             static_cast<unsigned long long>(m_frame_count), w, h,
             static_cast<int>(frame.format),
             static_cast<unsigned>(sample_center_luma(y_ptr, w, h, w)),
             static_cast<unsigned>(y_ptr ? y_ptr[0] : 0));
    }
    m_width.store(w, std::memory_order_relaxed);
    m_height.store(h, std::memory_order_relaxed);

    const uint64_t now_ns = os_gettime_ns();
    if (m_preview_cb && now_ns - m_preview_last_ns >= kPreviewIntervalNs) {
        m_preview_last_ns = now_ns;
        m_preview_cb(w, h, y_ptr, u_ptr, v_ptr, w, w / 2);
    }
}

void ZoomSource::on_engine_audio(uint32_t event_byte_len)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    const std::string shm_name = IPC_SHM_PREFIX + source_uuid + "_audio";
    if (event_byte_len == 0) return;
    const size_t audio_bytes = sizeof(ShmAudioHeader) + event_byte_len;
    if ((!m_audio_shm.ptr || m_audio_shm.size < audio_bytes) &&
        !shm_region_open_read(m_audio_shm, shm_name, audio_bytes)) {
        if (m_audio_frame_count == 0) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] Failed to open audio shared memory: source=%s uuid=%s bytes=%zu",
                 output_name().c_str(), source_uuid.c_str(), audio_bytes);
        }
        return;
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

    obs_source_audio audio = {};
    audio.samples_per_sec = sample_rate;
    audio.timestamp = os_gettime_ns();

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
    ++m_audio_frame_count;
    if (m_audio_frame_count == 1 || m_audio_frame_count % 250 == 0) {
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
    const uint32_t w = m_width.load(std::memory_order_relaxed);
    if (w != 0) return w;
    const uint32_t configured = width_for_resolution(resolution);
    return configured != 0 ? configured : kDefaultWidth;
}

uint32_t ZoomSource::height() const
{
    const uint32_t h = m_height.load(std::memory_order_relaxed);
    if (h != 0) return h;
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
        std::fill(y_plane, y_plane + y_size, 16);
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

void ZoomSource::release_shared_memory()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    shm_region_destroy(m_video_shm);
    shm_region_destroy(m_audio_shm);
}

static const char *zoom_source_get_name(void *)
{
    return obs_module_text("ZoomSource.Name");
}

static void *zoom_source_create(obs_data_t *settings, obs_source_t *source)
{
    auto *ctx = new ZoomSource();
    ctx->source = source;
    ctx->source_uuid = make_source_uuid();
    ctx->apply_settings(settings);

    ZoomEngineClient::instance().register_source(ctx->source_uuid, {
        [ctx](uint32_t w, uint32_t h) { ctx->on_engine_frame(w, h); },
        [ctx](uint32_t byte_len) { ctx->on_engine_audio(byte_len); }
    });
    ZoomEngineClient::instance().add_roster_callback(ctx,
        [ctx]() { ctx->on_roster_changed(); });
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

static void zoom_source_destroy(void *data)
{
    auto *ctx = static_cast<ZoomSource *>(data);
    if (ctx->m_hk_active_on_id  != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->m_hk_active_on_id);
    if (ctx->m_hk_active_off_id != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->m_hk_active_off_id);
    ZoomOutputManager::instance().unregister_source(ctx);
    ctx->unsubscribe();
    ZoomEngineClient::instance().remove_roster_callback(ctx);
    ZoomEngineClient::instance().unregister_source(ctx->source_uuid);
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

    obs_properties_add_text(props, PROP_OUTPUT_DISPLAY_NAME,
        obs_module_text("ZoomSource.OutputDisplayName"), OBS_TEXT_DEFAULT);

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
    obs_properties_add_bool(props, PROP_ACTIVE_SPEAKER,
        obs_module_text("ZoomSource.ActiveSpeaker"));
    obs_properties_add_int(props, PROP_SPEAKER_SENSITIVITY,
        obs_module_text("ZoomSource.SpeakerSensitivity"), 0, 3000, 50);
    obs_properties_add_int(props, PROP_SPEAKER_HOLD,
        obs_module_text("ZoomSource.SpeakerHold"), 0, 10000, 100);

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

    obs_properties_add_int(props, PROP_SPOTLIGHT_SLOT,
        obs_module_text("ZoomSource.SpotlightSlot"), 1, 9, 1);

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
    obs_properties_add_bool(props, PROP_ISOLATE_AUDIO,
        obs_module_text("ZoomSource.IsolateAudio"));

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

    obs_properties_add_button(props, "btn_refresh",
        obs_module_text("ZoomSource.RefreshParticipants"),
        [](obs_properties_t *, obs_property_t *, void *) -> bool { return true; });

    return props;
}

static void zoom_source_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_bool(settings, PROP_ACTIVE_SPEAKER, false);
    obs_data_set_default_int(settings, PROP_SPEAKER_SENSITIVITY, 300);
    obs_data_set_default_int(settings, PROP_SPEAKER_HOLD, 2000);
    obs_data_set_default_bool(settings, PROP_ISOLATE_AUDIO, false);
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
}
