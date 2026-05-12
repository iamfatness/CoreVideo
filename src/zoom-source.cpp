#include "zoom-source.h"
#include "zoom-meeting.h"
#include "zoom-participants.h"
#include "zoom-auth.h"
#include "zoom-settings.h"
#include <obs-module.h>

#define PROP_MEETING_ID        "meeting_id"
#define PROP_PASSCODE          "passcode"
#define PROP_PARTICIPANT_ID    "participant_id"
#define PROP_ACTIVE_SPEAKER    "active_speaker_mode"
#define PROP_AUTO_JOIN         "auto_join"
#define PROP_AUDIO_CHANNELS    "audio_channels"
#define AUDIO_CH_MONO   0
#define AUDIO_CH_STEREO 1

static AudioChannelMode audio_mode_from_data(obs_data_t *s)
{
    return obs_data_get_int(s, PROP_AUDIO_CHANNELS) == AUDIO_CH_STEREO
        ? AudioChannelMode::Stereo : AudioChannelMode::Mono;
}

void ZoomSource::apply_settings(obs_data_t *settings)
{
    meeting_id          = obs_data_get_string(settings, PROP_MEETING_ID);
    passcode            = obs_data_get_string(settings, PROP_PASSCODE);
    participant_id      = static_cast<uint32_t>(
                            obs_data_get_int(settings, PROP_PARTICIPANT_ID));
    auto_join           = obs_data_get_bool(settings, PROP_AUTO_JOIN);
    active_speaker_mode = obs_data_get_bool(settings, PROP_ACTIVE_SPEAKER);
    audio_mode          = audio_mode_from_data(settings);
    if (audio_delegate) audio_delegate->set_channel_mode(audio_mode);
}

void ZoomSource::on_meeting_state(MeetingState state)
{
    switch (state) {
    case MeetingState::InMeeting:
        if (!active_speaker_mode)
            video_delegate->subscribe(participant_id);
        audio_delegate->subscribe();
        break;
    case MeetingState::Idle:
    case MeetingState::Failed:
        video_delegate->unsubscribe();
        audio_delegate->unsubscribe();
        break;
    default:
        break;
    }
}

void ZoomSource::on_active_speaker_changed()
{
    if (!active_speaker_mode) return;
    if (ZoomMeeting::instance().state() != MeetingState::InMeeting) return;

    uint32_t spk = ZoomParticipants::instance().active_speaker_id();
    if (spk == 0 || spk == participant_id) return;

    participant_id = spk;
    video_delegate->unsubscribe();
    video_delegate->subscribe(spk);
}

// ── OBS source callbacks ──────────────────────────────────────────────────────

static const char *zoom_source_get_name(void *) { return obs_module_text("ZoomSource.Name"); }

static void *zoom_source_create(obs_data_t *settings, obs_source_t *source)
{
    auto *ctx = new ZoomSource();
    ctx->source = source;
    ctx->apply_settings(settings);
    ctx->video_delegate = std::make_unique<ZoomVideoDelegate>(source);
    ctx->audio_delegate = std::make_unique<ZoomAudioDelegate>(source, ctx->audio_mode);

    ZoomMeeting::instance().on_state_change(
        [ctx](MeetingState s) { ctx->on_meeting_state(s); });

    ZoomParticipants::instance().on_roster_change(
        [ctx]() { ctx->on_active_speaker_changed(); });

    if (ctx->auto_join && !ctx->meeting_id.empty()) {
        if (ZoomAuth::instance().state() != ZoomAuthState::Authenticated) {
            auto s = ZoomPluginSettings::load();
            if (!s.sdk_key.empty())
                ZoomAuth::instance().init(s.sdk_key, s.sdk_secret);
            if (!s.jwt_token.empty())
                ZoomAuth::instance().authenticate(s.jwt_token);
        }
        ZoomMeeting::instance().join(ctx->meeting_id, ctx->passcode);
    }

    if (ZoomMeeting::instance().state() == MeetingState::InMeeting)
        ctx->on_meeting_state(MeetingState::InMeeting);

    return ctx;
}

static void zoom_source_destroy(void *data)
{
    auto *ctx = static_cast<ZoomSource *>(data);
    ZoomMeeting::instance().on_state_change(nullptr);
    ZoomParticipants::instance().on_roster_change(nullptr);
    delete ctx;
}

static void zoom_source_update(void *data, obs_data_t *settings)
{
    static_cast<ZoomSource *>(data)->apply_settings(settings);
}

static uint32_t zoom_source_get_width(void *)  { return 0; }
static uint32_t zoom_source_get_height(void *) { return 0; }

static obs_properties_t *zoom_source_get_properties(void *data)
{
    auto *ctx = static_cast<ZoomSource *>(data);
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_text(props, PROP_MEETING_ID,
        obs_module_text("ZoomSource.MeetingId"),   OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, PROP_PASSCODE,
        obs_module_text("ZoomSource.Passcode"),    OBS_TEXT_PASSWORD);

    // Active speaker toggle
    obs_property_t *asp = obs_properties_add_bool(props, PROP_ACTIVE_SPEAKER,
        obs_module_text("ZoomSource.ActiveSpeaker"));
    obs_property_set_modified_callback(asp,
        [](obs_properties_t *pp, obs_property_t *, obs_data_t *settings) -> bool {
            bool on = obs_data_get_bool(settings, PROP_ACTIVE_SPEAKER);
            obs_property_t *p = obs_properties_get(pp, PROP_PARTICIPANT_ID);
            obs_property_set_enabled(p, !on);
            return true;
        });

    // Participant list — populated from live roster; falls back to "(none / ID 0)"
    obs_property_t *plist = obs_properties_add_list(props, PROP_PARTICIPANT_ID,
        obs_module_text("ZoomSource.ParticipantId"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(plist,
        obs_module_text("ZoomSource.NoParticipant"), 0);
    for (const auto &p : ZoomParticipants::instance().roster()) {
        std::string label = p.display_name.empty()
                            ? ("ID " + std::to_string(p.user_id))
                            : p.display_name;
        if (p.is_talking) label += " \xe2\x97\x8f"; // UTF-8 black circle
        if (p.has_video)  label += " [video]";
        obs_property_list_add_int(plist, label.c_str(),
                                  static_cast<long long>(p.user_id));
    }
    obs_property_set_enabled(plist, !ctx->active_speaker_mode);

    obs_properties_add_bool(props, PROP_AUTO_JOIN,
        obs_module_text("ZoomSource.AutoJoin"));

    obs_property_t *ch = obs_properties_add_list(props, PROP_AUDIO_CHANNELS,
        obs_module_text("ZoomSource.AudioChannels"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(ch,
        obs_module_text("ZoomSource.AudioMono"),   AUDIO_CH_MONO);
    obs_property_list_add_int(ch,
        obs_module_text("ZoomSource.AudioStereo"), AUDIO_CH_STEREO);

    obs_properties_add_button(props, "btn_join",
        obs_module_text("ZoomSource.JoinMeeting"),
        [](obs_properties_t *, obs_property_t *, void *d) -> bool {
            auto *c = static_cast<ZoomSource *>(d);
            if (ZoomAuth::instance().state() != ZoomAuthState::Authenticated) {
                auto s = ZoomPluginSettings::load();
                if (!s.sdk_key.empty())
                    ZoomAuth::instance().init(s.sdk_key, s.sdk_secret);
                if (!s.jwt_token.empty())
                    ZoomAuth::instance().authenticate(s.jwt_token);
            }
            ZoomMeeting::instance().join(c->meeting_id, c->passcode);
            return false;
        });

    obs_properties_add_button(props, "btn_leave",
        obs_module_text("ZoomSource.LeaveMeeting"),
        [](obs_properties_t *, obs_property_t *, void *) -> bool {
            ZoomMeeting::instance().leave();
            return false;
        });

    // Refresh participant list from current roster
    obs_properties_add_button(props, "btn_refresh",
        obs_module_text("ZoomSource.RefreshParticipants"),
        [](obs_properties_t *, obs_property_t *, void *) -> bool {
            return true; // returning true causes OBS to rebuild the properties panel
        });

    return props;
}

static void zoom_source_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_bool(settings, PROP_AUTO_JOIN,       false);
    obs_data_set_default_bool(settings, PROP_ACTIVE_SPEAKER,  false);
    obs_data_set_default_int( settings, PROP_PARTICIPANT_ID,  0);
    obs_data_set_default_int( settings, PROP_AUDIO_CHANNELS,  AUDIO_CH_MONO);
}

void zoom_source_register()
{
    struct obs_source_info info = {};
    info.id             = "zoom_participant_source";
    info.type           = OBS_SOURCE_TYPE_INPUT;
    info.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO |
                          OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name       = zoom_source_get_name;
    info.create         = zoom_source_create;
    info.destroy        = zoom_source_destroy;
    info.update         = zoom_source_update;
    info.get_width      = zoom_source_get_width;
    info.get_height     = zoom_source_get_height;
    info.get_properties = zoom_source_get_properties;
    info.get_defaults   = zoom_source_get_defaults;
    obs_register_source(&info);
}
