#include "zoom-source.h"
#include "zoom-meeting.h"
#include "zoom-participants.h"
#include "zoom-auth.h"
#include "zoom-settings.h"
#include "zoom-output-manager.h"
#include <obs-module.h>

#define PROP_MEETING_ID        "meeting_id"
#define PROP_PASSCODE          "passcode"
#define PROP_DISPLAY_NAME      "display_name"
#define PROP_PARTICIPANT_ID    "participant_id"
#define PROP_ACTIVE_SPEAKER    "active_speaker_mode"
#define PROP_ISOLATE_AUDIO     "isolate_audio"
#define PROP_AUTO_JOIN         "auto_join"
#define PROP_AUDIO_CHANNELS    "audio_channels"
#define PROP_RESOLUTION        "resolution"
#define PROP_STATUS            "status_label"
#define AUDIO_CH_MONO   0
#define AUDIO_CH_STEREO 1
#define RES_360P  0
#define RES_720P  1
#define RES_1080P 2

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

void ZoomSource::apply_settings(obs_data_t *settings)
{
    const uint32_t old_participant_id = participant_id;
    const bool old_active_speaker_mode = active_speaker_mode;
    const bool old_isolate_audio = isolate_audio;
    const AudioChannelMode old_audio_mode = audio_mode;
    const VideoResolution old_resolution = resolution;

    meeting_id          = obs_data_get_string(settings, PROP_MEETING_ID);
    passcode            = obs_data_get_string(settings, PROP_PASSCODE);
    display_name        = obs_data_get_string(settings, PROP_DISPLAY_NAME);
    participant_id      = static_cast<uint32_t>(
                            obs_data_get_int(settings, PROP_PARTICIPANT_ID));
    auto_join           = obs_data_get_bool(settings, PROP_AUTO_JOIN);
    active_speaker_mode = obs_data_get_bool(settings, PROP_ACTIVE_SPEAKER);
    isolate_audio       = obs_data_get_bool(settings, PROP_ISOLATE_AUDIO);
    audio_mode          = audio_mode_from_data(settings);
    resolution          = resolution_from_data(settings);
    if (audio_delegate) audio_delegate->set_channel_mode(audio_mode);

    if (video_delegate && audio_delegate &&
        ZoomMeeting::instance().state() == MeetingState::InMeeting &&
        (old_participant_id != participant_id ||
         old_active_speaker_mode != active_speaker_mode ||
         old_isolate_audio != isolate_audio ||
         old_audio_mode != audio_mode ||
         old_resolution != resolution)) {
        resubscribe();
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
    if (audio_delegate) audio_delegate->set_channel_mode(audio_mode);
    if (ZoomMeeting::instance().state() == MeetingState::InMeeting)
        resubscribe();
}

void ZoomSource::resubscribe()
{
    if (!video_delegate || !audio_delegate) return;
    video_delegate->unsubscribe();
    audio_delegate->unsubscribe();
    audio_delegate->set_isolated_user(0);
    on_meeting_state(ZoomMeeting::instance().state());
}

void ZoomSource::on_meeting_state(MeetingState state)
{
    switch (state) {
    case MeetingState::InMeeting:
        if (!active_speaker_mode)
            video_delegate->subscribe(participant_id, resolution);
        audio_delegate->subscribe();
        if (isolate_audio && !active_speaker_mode && participant_id != 0)
            audio_delegate->set_isolated_user(participant_id);
        break;
    case MeetingState::Idle:
    case MeetingState::Failed:
        video_delegate->unsubscribe();
        audio_delegate->unsubscribe();
        audio_delegate->set_isolated_user(0);
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
    video_delegate->subscribe(spk, resolution);
    if (isolate_audio)
        audio_delegate->set_isolated_user(spk);
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

    ZoomMeeting::instance().on_state_change(ctx,
        [ctx](MeetingState s) { ctx->on_meeting_state(s); });

    ZoomParticipants::instance().add_roster_callback(ctx,
        [ctx]() { ctx->on_active_speaker_changed(); });

    ZoomOutputManager::instance().register_source(ctx);

    if (ctx->auto_join && !ctx->meeting_id.empty()) {
        if (ZoomAuth::instance().state() == ZoomAuthState::Authenticated) {
            ZoomMeeting::instance().join(ctx->meeting_id, ctx->passcode,
                                         ctx->display_name.empty() ? "OBS"
                                                                    : ctx->display_name);
        } else {
            // Not yet authenticated — init SDK if needed and register a
            // one-shot callback to join as soon as authentication completes.
            auto s = ZoomPluginSettings::load();
            if (ZoomAuth::instance().state() == ZoomAuthState::Unauthenticated) {
                if (!s.sdk_key.empty())
                    ZoomAuth::instance().init(s.sdk_key, s.sdk_secret);
                if (!s.jwt_token.empty())
                    ZoomAuth::instance().authenticate(s.jwt_token);
            }
            ZoomAuth::instance().add_state_callback(ctx,
                [ctx](ZoomAuthState auth_state) {
                    if (auth_state == ZoomAuthState::Authenticated) {
                        ZoomAuth::instance().remove_state_callback(ctx);
                        ZoomMeeting::instance().join(
                            ctx->meeting_id, ctx->passcode,
                            ctx->display_name.empty() ? "OBS" : ctx->display_name);
                    } else if (auth_state == ZoomAuthState::Failed) {
                        ZoomAuth::instance().remove_state_callback(ctx);
                    }
                });
        }
    }

    if (ZoomMeeting::instance().state() == MeetingState::InMeeting)
        ctx->on_meeting_state(MeetingState::InMeeting);

    return ctx;
}

static void zoom_source_destroy(void *data)
{
    auto *ctx = static_cast<ZoomSource *>(data);
    ZoomOutputManager::instance().unregister_source(ctx);
    ZoomAuth::instance().remove_state_callback(ctx);
    ZoomMeeting::instance().remove_state_change(ctx);
    ZoomParticipants::instance().remove_roster_callback(ctx);
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

    // Read-only status label
    {
        const MeetingState cur_state = ZoomMeeting::instance().state();
        const char *status_keys[] = {
            "ZoomStatus.Idle", "ZoomStatus.Joining", "ZoomStatus.InMeeting",
            "ZoomStatus.Leaving", "ZoomStatus.Failed"
        };
        const char *status_str = obs_module_text(
            status_keys[static_cast<int>(cur_state)]);
        obs_property_t *status_prop = obs_properties_add_text(
            props, PROP_STATUS, obs_module_text("ZoomSource.Status"),
            OBS_TEXT_DEFAULT);
        obs_data_set_string(obs_source_get_settings(ctx->source),
                            PROP_STATUS, status_str);
        obs_property_set_enabled(status_prop, false);
    }

    obs_properties_add_text(props, PROP_MEETING_ID,
        obs_module_text("ZoomSource.MeetingId"),   OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, PROP_PASSCODE,
        obs_module_text("ZoomSource.Passcode"),    OBS_TEXT_PASSWORD);
    obs_properties_add_text(props, PROP_DISPLAY_NAME,
        obs_module_text("ZoomSource.DisplayName"), OBS_TEXT_DEFAULT);

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

    obs_properties_add_bool(props, PROP_ISOLATE_AUDIO,
        obs_module_text("ZoomSource.IsolateAudio"));

    obs_properties_add_bool(props, PROP_AUTO_JOIN,
        obs_module_text("ZoomSource.AutoJoin"));

    obs_property_t *ch = obs_properties_add_list(props, PROP_AUDIO_CHANNELS,
        obs_module_text("ZoomSource.AudioChannels"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(ch,
        obs_module_text("ZoomSource.AudioMono"),   AUDIO_CH_MONO);
    obs_property_list_add_int(ch,
        obs_module_text("ZoomSource.AudioStereo"), AUDIO_CH_STEREO);

    obs_property_t *res = obs_properties_add_list(props, PROP_RESOLUTION,
        obs_module_text("ZoomSource.Resolution"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(res,
        obs_module_text("ZoomSource.Resolution360P"),  RES_360P);
    obs_property_list_add_int(res,
        obs_module_text("ZoomSource.Resolution720P"),  RES_720P);
    obs_property_list_add_int(res,
        obs_module_text("ZoomSource.Resolution1080P"), RES_1080P);

    obs_properties_add_button(props, "btn_join",
        obs_module_text("ZoomSource.JoinMeeting"),
        [](obs_properties_t *, obs_property_t *, void *d) -> bool {
            auto *c = static_cast<ZoomSource *>(d);
            if (ZoomAuth::instance().state() == ZoomAuthState::Authenticated) {
                ZoomMeeting::instance().join(c->meeting_id, c->passcode,
                    c->display_name.empty() ? "OBS" : c->display_name);
            } else {
                auto s = ZoomPluginSettings::load();
                if (ZoomAuth::instance().state() == ZoomAuthState::Unauthenticated) {
                    if (!s.sdk_key.empty())
                        ZoomAuth::instance().init(s.sdk_key, s.sdk_secret);
                    if (!s.jwt_token.empty())
                        ZoomAuth::instance().authenticate(s.jwt_token);
                }
                ZoomAuth::instance().add_state_callback(c,
                    [c](ZoomAuthState auth_state) {
                        if (auth_state == ZoomAuthState::Authenticated ||
                            auth_state == ZoomAuthState::Failed) {
                            ZoomAuth::instance().remove_state_callback(c);
                            if (auth_state == ZoomAuthState::Authenticated)
                                ZoomMeeting::instance().join(
                                    c->meeting_id, c->passcode,
                                    c->display_name.empty() ? "OBS" : c->display_name);
                        }
                    });
            }
            return true; // return true so OBS refreshes the properties panel
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
    obs_data_set_default_string(settings, PROP_DISPLAY_NAME,  "OBS");
    obs_data_set_default_bool(  settings, PROP_AUTO_JOIN,     false);
    obs_data_set_default_bool(  settings, PROP_ACTIVE_SPEAKER,false);
    obs_data_set_default_bool(  settings, PROP_ISOLATE_AUDIO, false);
    obs_data_set_default_int(   settings, PROP_PARTICIPANT_ID,0);
    obs_data_set_default_int(   settings, PROP_AUDIO_CHANNELS,AUDIO_CH_MONO);
    obs_data_set_default_int(   settings, PROP_RESOLUTION,    RES_1080P);
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
