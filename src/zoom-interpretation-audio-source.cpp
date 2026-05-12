#include "zoom-interpretation-audio-source.h"
#include "zoom-audio-router.h"
#include "zoom-meeting.h"
#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/platform.h>

#define PROP_LANGUAGE       "language"
#define PROP_AUDIO_CHANNELS "audio_channels"
#define AUDIO_CH_MONO   0
#define AUDIO_CH_STEREO 1

void ZoomInterpAudioSource::apply_settings(obs_data_t *settings)
{
    const std::string new_lang =
        obs_data_get_string(settings, PROP_LANGUAGE);
    if (new_lang != language && registered) {
        do_unregister();
        language = new_lang;
        do_register();
    } else {
        language = new_lang;
    }
}

void ZoomInterpAudioSource::do_register()
{
    if (registered || language.empty()) return;
    if (ZoomMeeting::instance().state() != MeetingState::InMeeting) return;

    obs_source_t *src  = source;
    std::string   lang = language;
    auto          live = alive;

    ZoomAudioRouter::instance().add_interp_sink(this,
        [src, lang, live](AudioRawData *data, const std::string &incoming_lang) {
            if (!live->load(std::memory_order_acquire)) return;
            if (incoming_lang != lang) return;
            if (!data || !src) return;
            const uint32_t byte_len = data->GetBufferLen();
            if (byte_len == 0) return;
            obs_source_audio audio = {};
            audio.data[0]         = reinterpret_cast<const uint8_t *>(data->GetBuffer());
            audio.frames          = byte_len / sizeof(int16_t);
            audio.format          = AUDIO_FORMAT_16BIT;
            audio.samples_per_sec = data->GetSampleRate();
            audio.speakers        = SPEAKERS_MONO;
            audio.timestamp       = os_gettime_ns();
            obs_source_output_audio(src, &audio);
        });

    registered = true;
    blog(LOG_INFO, "[obs-zoom-plugin] Interpretation audio source registered for '%s'",
         language.c_str());
}

void ZoomInterpAudioSource::do_unregister()
{
    if (!registered) return;
    ZoomAudioRouter::instance().remove_interp_sink(this);
    registered = false;
}

// ── OBS source callbacks ──────────────────────────────────────────────────────

static const char *ia_get_name(void *)
{
    return obs_module_text("ZoomInterpAudio.Name");
}

static void *ia_create(obs_data_t *settings, obs_source_t *source)
{
    auto *ctx   = new ZoomInterpAudioSource();
    ctx->source = source;
    ctx->apply_settings(settings);

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

static void ia_destroy(void *data)
{
    auto *ctx = static_cast<ZoomInterpAudioSource *>(data);
    ZoomMeeting::instance().remove_state_change(ctx);
    ctx->alive->store(false, std::memory_order_release);
    ctx->do_unregister();
    delete ctx;
}

static void ia_update(void *data, obs_data_t *settings)
{
    static_cast<ZoomInterpAudioSource *>(data)->apply_settings(settings);
}

static obs_properties_t *ia_get_properties(void *)
{
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_text(props, PROP_LANGUAGE,
        obs_module_text("ZoomInterpAudio.Language"), OBS_TEXT_DEFAULT);

    obs_properties_add_button(props, "btn_refresh",
        obs_module_text("ZoomInterpAudio.Refresh"),
        [](obs_properties_t *, obs_property_t *, void *) -> bool {
            return true;
        });

    return props;
}

static void ia_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, PROP_LANGUAGE, "");
}

void zoom_interpretation_audio_source_register()
{
    struct obs_source_info info = {};
    info.id             = "zoom_interpretation_audio_source";
    info.type           = OBS_SOURCE_TYPE_INPUT;
    info.output_flags   = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name       = ia_get_name;
    info.create         = ia_create;
    info.destroy        = ia_destroy;
    info.update         = ia_update;
    info.get_properties = ia_get_properties;
    info.get_defaults   = ia_get_defaults;
    obs_register_source(&info);
}
