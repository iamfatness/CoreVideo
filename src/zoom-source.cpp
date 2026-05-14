#include "zoom-source.h"
#include "zoom-engine-client.h"
#include "zoom-settings.h"
#include <media-io/audio-io.h>
#include <media-io/video-io.h>
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <climits>
#include <cstring>

#define PROP_MEETING_ID           "meeting_id"
#define PROP_PASSCODE             "passcode"
#define PROP_DISPLAY_NAME         "display_name"
#define PROP_OUTPUT_DISPLAY_NAME  "output_display_name"
#define PROP_PARTICIPANT_ID       "participant_id"
#define PROP_ACTIVE_SPEAKER       "active_speaker_mode"
#define PROP_SPEAKER_SENSITIVITY  "speaker_sensitivity_ms"
#define PROP_SPEAKER_HOLD         "speaker_hold_ms"
#define PROP_VIDEO_LOSS_MODE      "video_loss_mode"
#define PROP_ISOLATE_AUDIO        "isolate_audio"
#define PROP_AUTO_JOIN            "auto_join"
#define PROP_AUDIO_CHANNELS       "audio_channels"
#define PROP_RESOLUTION           "resolution"
#define PROP_STATUS               "status_label"
#define PROP_HW_ACCEL_MODE        "hw_accel_mode"

#define VIDEO_LOSS_LAST_FRAME 0
#define VIDEO_LOSS_BLACK      1
#define AUDIO_CH_MONO   0
#define AUDIO_CH_STEREO 1
#define RES_360P  0
#define RES_720P  1
#define RES_1080P 2

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
    case RES_720P: return VideoResolution::P720;
    default:       return VideoResolution::P1080;
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

void ZoomSource::apply_settings(obs_data_t *settings)
{
    const uint32_t old_participant_id = participant_id;
    const bool old_active_speaker_mode = active_speaker_mode;
    const AudioChannelMode old_audio_mode = audio_mode;
    const VideoResolution old_resolution = resolution;
    const int old_hw_accel_override = hw_accel_override;

    meeting_id = obs_data_get_string(settings, PROP_MEETING_ID);
    passcode = obs_data_get_string(settings, PROP_PASSCODE);
    display_name = obs_data_get_string(settings, PROP_DISPLAY_NAME);
    output_display_name = obs_data_get_string(settings, PROP_OUTPUT_DISPLAY_NAME);
    participant_id = static_cast<uint32_t>(obs_data_get_int(settings, PROP_PARTICIPANT_ID));
    auto_join = obs_data_get_bool(settings, PROP_AUTO_JOIN);
    active_speaker_mode = obs_data_get_bool(settings, PROP_ACTIVE_SPEAKER);
    speaker_sensitivity_ms = static_cast<uint32_t>(
        obs_data_get_int(settings, PROP_SPEAKER_SENSITIVITY));
    speaker_hold_ms = static_cast<uint32_t>(obs_data_get_int(settings, PROP_SPEAKER_HOLD));
    isolate_audio = obs_data_get_bool(settings, PROP_ISOLATE_AUDIO);
    audio_mode = audio_mode_from_data(settings);
    resolution = resolution_from_data(settings);
    video_loss_mode = obs_data_get_int(settings, PROP_VIDEO_LOSS_MODE) == VIDEO_LOSS_BLACK
        ? VideoLossMode::Black : VideoLossMode::LastFrame;
    hw_accel_override = static_cast<int>(obs_data_get_int(settings, PROP_HW_ACCEL_MODE));

    if (m_subscribed && (old_participant_id != participant_id ||
        old_active_speaker_mode != active_speaker_mode ||
        old_audio_mode != audio_mode || old_resolution != resolution)) {
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
    info.active_speaker = active_speaker_mode;
    info.isolate_audio = isolate_audio;
    info.audio_mode = audio_mode;
    return info;
}

void ZoomSource::configure_output(uint32_t new_participant_id,
                                  bool new_active_speaker_mode,
                                  bool new_isolate_audio,
                                  AudioChannelMode new_audio_mode)
{
    if (source) {
        obs_data_t *settings = obs_source_get_settings(source);
        obs_data_set_int(settings, PROP_PARTICIPANT_ID, new_participant_id);
        obs_data_set_bool(settings, PROP_ACTIVE_SPEAKER, new_active_speaker_mode);
        obs_data_set_bool(settings, PROP_ISOLATE_AUDIO, new_isolate_audio);
        obs_data_set_int(settings, PROP_AUDIO_CHANNELS,
                         new_audio_mode == AudioChannelMode::Stereo
                         ? AUDIO_CH_STEREO : AUDIO_CH_MONO);
        obs_source_update(source, settings);
        obs_data_release(settings);
        return;
    }

    participant_id = new_participant_id;
    active_speaker_mode = new_active_speaker_mode;
    isolate_audio = new_isolate_audio;
    audio_mode = new_audio_mode;
    if (m_subscribed) subscribe();
}

void ZoomSource::subscribe()
{
    if (source_uuid.empty()) source_uuid = make_source_uuid();
    const uint32_t subscribe_id =
        effective_participant_id(participant_id, active_speaker_mode);
    ZoomEngineClient::instance().subscribe(source_uuid, subscribe_id);
    m_current_subscription_id = subscribe_id;
    m_subscribed = true;
}

void ZoomSource::unsubscribe()
{
    if (!m_subscribed) return;
    ZoomEngineClient::instance().unsubscribe(source_uuid);
    m_subscribed = false;
    m_current_subscription_id = 0;
}

void ZoomSource::on_roster_changed()
{
    if (!m_subscribed || !active_speaker_mode) return;
    const uint32_t subscribe_id =
        effective_participant_id(participant_id, active_speaker_mode);
    if (subscribe_id == 0 || subscribe_id == m_current_subscription_id) return;
    subscribe();
}

void ZoomSource::on_engine_frame(uint32_t event_width, uint32_t event_height)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    const std::string shm_name = IPC_SHM_PREFIX + source_uuid;
    if (event_width == 0 || event_height == 0) return;
    const size_t frame_bytes = sizeof(ShmFrameHeader) +
        static_cast<size_t>(event_width) * event_height * 3 / 2;
    if ((!m_video_shm.ptr || m_video_shm.size < frame_bytes) &&
        !shm_region_open_read(m_video_shm, shm_name, frame_bytes))
        return;

    auto *hdr = static_cast<const ShmFrameHeader *>(m_video_shm.ptr);
    const uint32_t w = hdr->width;
    const uint32_t h = hdr->height;
    const uint32_t y_len = hdr->y_len;
    if (!source || w == 0 || h == 0 || y_len != w * h) return;

    // Bound the read: the header may claim a frame larger than the region
    // we have mapped (engine wrote a bigger frame than the size we used to
    // open the SHM). Reject those rather than walking past the mapping.
    const size_t needed_bytes = sizeof(ShmFrameHeader) +
        static_cast<size_t>(y_len) + static_cast<size_t>(y_len) / 2;
    if (needed_bytes > m_video_shm.size) return;

    const auto *pixels = static_cast<const uint8_t *>(m_video_shm.ptr) + sizeof(ShmFrameHeader);
    const auto *y_ptr = pixels;
    const auto *u_ptr = pixels + y_len;
    const auto *v_ptr = pixels + y_len + y_len / 4;
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

    obs_source_output_video(source, &frame);
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
        !shm_region_open_read(m_audio_shm, shm_name, audio_bytes))
        return;

    auto *hdr = static_cast<const ShmAudioHeader *>(m_audio_shm.ptr);
    const uint32_t byte_len = hdr->byte_len;
    if (!source || byte_len == 0) return;
    if (sizeof(ShmAudioHeader) + byte_len > m_audio_shm.size) return;
    const auto *pcm = reinterpret_cast<const int16_t *>(
        static_cast<const uint8_t *>(m_audio_shm.ptr) + sizeof(ShmAudioHeader));

    obs_source_audio audio = {};
    audio.samples_per_sec = hdr->sample_rate;
    audio.timestamp = os_gettime_ns();

    if (audio_mode == AudioChannelMode::Stereo && hdr->channels == 1) {
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
        audio.frames = byte_len / (kZoomBytesPerSample * std::max<uint16_t>(hdr->channels, 1));
        audio.format = AUDIO_FORMAT_16BIT;
        audio.speakers = hdr->channels == 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
    }
    obs_source_output_audio(source, &audio);
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

    if (ctx->auto_join && !ctx->meeting_id.empty()) {
        if (ZoomEngineClient::instance().start(s.jwt_token)) {
            ZoomEngineClient::instance().join(ctx->meeting_id, ctx->passcode,
                ctx->display_name.empty() ? "OBS" : ctx->display_name);
            ctx->subscribe();
        }
    }
    return ctx;
}

static void zoom_source_destroy(void *data)
{
    auto *ctx = static_cast<ZoomSource *>(data);
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
    obs_properties_add_text(props, PROP_MEETING_ID,
        obs_module_text("ZoomSource.MeetingId"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, PROP_PASSCODE,
        obs_module_text("ZoomSource.Passcode"), OBS_TEXT_PASSWORD);
    obs_properties_add_text(props, PROP_DISPLAY_NAME,
        obs_module_text("ZoomSource.DisplayName"), OBS_TEXT_DEFAULT);

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
    obs_properties_add_bool(props, PROP_ISOLATE_AUDIO,
        obs_module_text("ZoomSource.IsolateAudio"));
    obs_properties_add_bool(props, PROP_AUTO_JOIN,
        obs_module_text("ZoomSource.AutoJoin"));

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

    obs_properties_add_button(props, "btn_join",
        obs_module_text("ZoomSource.JoinMeeting"),
        [](obs_properties_t *, obs_property_t *, void *d) -> bool {
            auto *c = static_cast<ZoomSource *>(d);
            const ZoomPluginSettings s = ZoomPluginSettings::load();
            if (ZoomEngineClient::instance().start(s.jwt_token) &&
                ZoomEngineClient::instance().join(c->meeting_id, c->passcode,
                    c->display_name.empty() ? "OBS" : c->display_name)) {
                c->subscribe();
            }
            return true;
        });

    obs_properties_add_button(props, "btn_leave",
        obs_module_text("ZoomSource.LeaveMeeting"),
        [](obs_properties_t *, obs_property_t *, void *) -> bool {
            ZoomEngineClient::instance().leave();
            return false;
        });

    obs_properties_add_button(props, "btn_refresh",
        obs_module_text("ZoomSource.RefreshParticipants"),
        [](obs_properties_t *, obs_property_t *, void *) -> bool { return true; });

    return props;
}

static void zoom_source_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, PROP_DISPLAY_NAME, "OBS");
    obs_data_set_default_bool(settings, PROP_AUTO_JOIN, false);
    obs_data_set_default_bool(settings, PROP_ACTIVE_SPEAKER, false);
    obs_data_set_default_int(settings, PROP_SPEAKER_SENSITIVITY, 300);
    obs_data_set_default_int(settings, PROP_SPEAKER_HOLD, 2000);
    obs_data_set_default_bool(settings, PROP_ISOLATE_AUDIO, false);
    obs_data_set_default_int(settings, PROP_PARTICIPANT_ID, 0);
    obs_data_set_default_int(settings, PROP_AUDIO_CHANNELS, AUDIO_CH_MONO);
    obs_data_set_default_int(settings, PROP_RESOLUTION, RES_1080P);
    obs_data_set_default_int(settings, PROP_VIDEO_LOSS_MODE, VIDEO_LOSS_LAST_FRAME);
    obs_data_set_default_int(settings, PROP_HW_ACCEL_MODE, -1);  // use global
}

void zoom_source_register()
{
    obs_source_info info = {};
    info.id = "zoom_participant_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = zoom_source_get_name;
    info.create = zoom_source_create;
    info.destroy = zoom_source_destroy;
    info.update = zoom_source_update;
    info.get_width = zoom_source_get_width;
    info.get_height = zoom_source_get_height;
    info.get_properties = zoom_source_get_properties;
    info.get_defaults = zoom_source_get_defaults;
    obs_register_source(&info);
}
