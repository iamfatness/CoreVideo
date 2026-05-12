#include "zoom-participant-audio-source.h"
#include "zoom-audio-router.h"
#include "zoom-participants.h"
#include "zoom-meeting.h"
#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/platform.h>
#include <vector>
#include <cstring>

#define PROP_PARTICIPANT_ID "participant_id"
#define PROP_AUDIO_CHANNELS "audio_channels"
#define AUDIO_CH_MONO   0
#define AUDIO_CH_STEREO 1

static constexpr uint32_t ZOOM_BYTES_PER_SAMPLE = sizeof(int16_t);

// ── Per-source audio output ───────────────────────────────────────────────────

static void push_mono(obs_source_t *src, AudioRawData *data)
{
    const uint32_t byte_len = data->GetBufferLen();
    if (byte_len == 0) return;

    obs_source_audio audio = {};
    audio.data[0]         = reinterpret_cast<const uint8_t *>(data->GetBuffer());
    audio.frames          = byte_len / ZOOM_BYTES_PER_SAMPLE;
    audio.format          = AUDIO_FORMAT_16BIT;
    audio.samples_per_sec = data->GetSampleRate();
    audio.speakers        = SPEAKERS_MONO;
    audio.timestamp       = os_gettime_ns();
    obs_source_output_audio(src, &audio);
}

static void push_stereo(obs_source_t *src, AudioRawData *data,
                        std::vector<int16_t> &buf)
{
    const uint32_t byte_len    = data->GetBufferLen();
    if (byte_len == 0) return;
    const uint32_t mono_frames  = byte_len / ZOOM_BYTES_PER_SAMPLE;
    const uint32_t stereo_count = mono_frames * 2;

    if (buf.size() < stereo_count) buf.resize(stereo_count);

    const int16_t *in  = reinterpret_cast<const int16_t *>(data->GetBuffer());
    int16_t       *out = buf.data();
    for (uint32_t i = 0; i < mono_frames; ++i) {
        out[i * 2]     = in[i];
        out[i * 2 + 1] = in[i];
    }

    obs_source_audio audio = {};
    audio.data[0]         = reinterpret_cast<const uint8_t *>(out);
    audio.frames          = mono_frames;
    audio.format          = AUDIO_FORMAT_16BIT;
    audio.samples_per_sec = data->GetSampleRate();
    audio.speakers        = SPEAKERS_STEREO;
    audio.timestamp       = os_gettime_ns();
    obs_source_output_audio(src, &audio);
}

// ── ZoomParticipantAudioSource methods ────────────────────────────────────────

void ZoomParticipantAudioSource::apply_settings(obs_data_t *settings)
{
    const uint32_t new_id = static_cast<uint32_t>(
        obs_data_get_int(settings, PROP_PARTICIPANT_ID));
    const AudioChannelMode new_mode =
        obs_data_get_int(settings, PROP_AUDIO_CHANNELS) == AUDIO_CH_STEREO
        ? AudioChannelMode::Stereo : AudioChannelMode::Mono;

    if (registered && (new_id != participant_id || new_mode != audio_mode)) {
        do_unregister();
        participant_id = new_id;
        audio_mode     = new_mode;
        do_register();
    } else {
        participant_id = new_id;
        audio_mode     = new_mode;
    }
}

void ZoomParticipantAudioSource::do_register()
{
    if (registered || participant_id == 0) return;
    if (ZoomMeeting::instance().state() != MeetingState::InMeeting) return;

    obs_source_t       *src  = source;
    AudioChannelMode    mode = audio_mode;
    auto buf  = std::make_shared<std::vector<int16_t>>();
    auto live = alive; // shared_ptr copy kept alive by the lambda

    ZoomAudioRouter::instance().add_participant_sink(
        participant_id,
        this,
        [src, mode, buf, live](AudioRawData *data) {
            if (!live->load(std::memory_order_acquire)) return;
            if (mode == AudioChannelMode::Stereo)
                push_stereo(src, data, *buf);
            else
                push_mono(src, data);
        });

    registered = true;
    blog(LOG_INFO, "[obs-zoom-plugin] Participant audio source registered for user %u",
         participant_id);
}

void ZoomParticipantAudioSource::do_unregister()
{
    if (!registered || participant_id == 0) return;
    ZoomAudioRouter::instance().remove_participant_sink(participant_id, this);
    registered = false;
}

// ── OBS source callbacks ──────────────────────────────────────────────────────

static const char *pa_get_name(void *)
{
    return obs_module_text("ZoomParticipantAudio.Name");
}

static void *pa_create(obs_data_t *settings, obs_source_t *source)
{
    auto *ctx   = new ZoomParticipantAudioSource();
    ctx->source = source;
    ctx->apply_settings(settings);

    // Subscribe if we join in after the source is created mid-meeting.
    ZoomMeeting::instance().on_state_change(ctx,
        [ctx](MeetingState s) {
            if (s == MeetingState::InMeeting)
                ctx->do_register();
            else if (s == MeetingState::Idle || s == MeetingState::Failed)
                ctx->do_unregister();
        });

    if (ZoomMeeting::instance().state() == MeetingState::InMeeting)
        ctx->do_register();

    return ctx;
}

static void pa_destroy(void *data)
{
    auto *ctx = static_cast<ZoomParticipantAudioSource *>(data);
    ZoomMeeting::instance().remove_state_change(ctx);
    ctx->alive->store(false, std::memory_order_release); // invalidate before removing
    ctx->do_unregister();
    delete ctx;
}

static void pa_update(void *data, obs_data_t *settings)
{
    static_cast<ZoomParticipantAudioSource *>(data)->apply_settings(settings);
}

static obs_properties_t *pa_get_properties(void *)
{
    obs_properties_t *props = obs_properties_create();

    // Participant list
    obs_property_t *plist = obs_properties_add_list(props, PROP_PARTICIPANT_ID,
        obs_module_text("ZoomParticipantAudio.ParticipantId"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(plist,
        obs_module_text("ZoomParticipantAudio.NoParticipant"), 0);
    for (const auto &p : ZoomParticipants::instance().roster()) {
        std::string label = p.display_name.empty()
                            ? ("ID " + std::to_string(p.user_id))
                            : p.display_name;
        if (p.is_talking) label += " \xe2\x97\x8f";
        if (p.has_video)  label += " [video]";
        obs_property_list_add_int(plist, label.c_str(),
                                  static_cast<long long>(p.user_id));
    }

    // Audio channels
    obs_property_t *ch = obs_properties_add_list(props, PROP_AUDIO_CHANNELS,
        obs_module_text("ZoomParticipantAudio.AudioChannels"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(ch,
        obs_module_text("ZoomParticipantAudio.AudioMono"),   AUDIO_CH_MONO);
    obs_property_list_add_int(ch,
        obs_module_text("ZoomParticipantAudio.AudioStereo"), AUDIO_CH_STEREO);

    // Refresh roster
    obs_properties_add_button(props, "btn_refresh",
        obs_module_text("ZoomParticipantAudio.RefreshParticipants"),
        [](obs_properties_t *, obs_property_t *, void *) -> bool {
            return true;
        });

    return props;
}

static void pa_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, PROP_PARTICIPANT_ID, 0);
    obs_data_set_default_int(settings, PROP_AUDIO_CHANNELS, AUDIO_CH_MONO);
}

void zoom_participant_audio_source_register()
{
    struct obs_source_info info = {};
    info.id           = "zoom_participant_audio_source";
    info.type         = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name     = pa_get_name;
    info.create       = pa_create;
    info.destroy      = pa_destroy;
    info.update       = pa_update;
    info.get_properties = pa_get_properties;
    info.get_defaults   = pa_get_defaults;
    obs_register_source(&info);
}
