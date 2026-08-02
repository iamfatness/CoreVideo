#include "zoom-participant-audio-source.h"

#include "engine-ipc.h"
#include "speaker-director.h"
#include "zoom-engine-client.h"
#include "zoom-settings.h"
#include "zoom-types.h"

#include <media-io/audio-io.h>
#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#define PROP_PARTICIPANT_ID "participant_id"
#define PROP_AUDIO_CHANNELS "audio_channels"

#define AUDIO_CH_MONO   0
#define AUDIO_CH_STEREO 1

static constexpr uint32_t kZoomBytesPerSample = sizeof(int16_t);

enum class CoreVideoAudioKind {
    Participant,
    ActiveSpeaker,
    Audience,
};

static std::string make_audio_source_uuid()
{
    static std::atomic<uint64_t> counter{1};
    return "aud_" + std::to_string(os_gettime_ns()) + "_" +
        std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

struct CoreVideoAudioSource {
    struct CallbackGate {
        std::mutex mtx;
        bool alive = true;
    };

    obs_source_t *source = nullptr;
    std::string source_uuid;
    CoreVideoAudioKind kind = CoreVideoAudioKind::Participant;
    std::atomic<uint32_t> participant_id{0};
    std::atomic<AudioChannelMode> audio_mode{AudioChannelMode::Mono};
    std::atomic<uint32_t> current_participant_id{0};
    std::atomic<bool> subscribed{false};
    std::atomic<bool> active{false};
    std::mutex mtx;
    ShmRegion audio_shm;
    std::vector<uint8_t> audio_buf;
    std::vector<int16_t> stereo_buf;
    uint64_t frame_count = 0;
    std::shared_ptr<CallbackGate> callback_gate =
        std::make_shared<CallbackGate>();
};

static uint32_t target_participant_id(const CoreVideoAudioSource *ctx)
{
    if (!ctx) return 0;
    if (ctx->kind == CoreVideoAudioKind::ActiveSpeaker) {
        const ZoomPluginSettings settings = ZoomPluginSettings::load();
        std::vector<uint32_t> excluded;
        if (settings.speaker_exclude_participant_1 != 0)
            excluded.push_back(settings.speaker_exclude_participant_1);
        if (settings.speaker_exclude_participant_2 != 0 &&
            settings.speaker_exclude_participant_2 !=
                settings.speaker_exclude_participant_1)
            excluded.push_back(settings.speaker_exclude_participant_2);
        SpeakerDirector::instance().configure(
            settings.speaker_sensitivity_ms, settings.speaker_hold_ms,
            settings.speaker_require_video, excluded);
        return ZoomEngineClient::instance().active_speaker_id();
    }
    if (ctx->kind == CoreVideoAudioKind::Participant)
        return ctx->participant_id.load(std::memory_order_acquire);
    return 0;
}

static void subscribe_audio(CoreVideoAudioSource *ctx)
{
    if (!ctx || ctx->source_uuid.empty()) return;

    if (ctx->kind == CoreVideoAudioKind::Audience) {
        ZoomEngineClient::instance().subscribe_audio(ctx->source_uuid, 0,
                                                     false, true);
        ctx->current_participant_id.store(0, std::memory_order_release);
        ctx->subscribed.store(true, std::memory_order_release);
        return;
    }

    const uint32_t target = target_participant_id(ctx);
    if (target == 0) return;

    ZoomEngineClient::instance().subscribe_audio(ctx->source_uuid, target,
                                                 true, false);
    ctx->current_participant_id.store(target, std::memory_order_release);
    ctx->subscribed.store(true, std::memory_order_release);
}

static void unsubscribe_audio(CoreVideoAudioSource *ctx)
{
    if (!ctx || !ctx->subscribed.load(std::memory_order_acquire)) return;
    ZoomEngineClient::instance().unsubscribe(ctx->source_uuid);
    ctx->subscribed.store(false, std::memory_order_release);
    ctx->current_participant_id.store(0, std::memory_order_release);
}

static void maybe_resubscribe_for_roster(CoreVideoAudioSource *ctx)
{
    if (!ctx || !ctx->active.load(std::memory_order_acquire)) return;
    if (ctx->kind == CoreVideoAudioKind::Audience) {
        if (!ctx->subscribed.load(std::memory_order_acquire))
            subscribe_audio(ctx);
        return;
    }

    const uint32_t target = target_participant_id(ctx);
    if (target == 0) return;
    if (!ctx->subscribed.load(std::memory_order_acquire) ||
        target != ctx->current_participant_id.load(std::memory_order_acquire)) {
        if (ctx->subscribed.load(std::memory_order_acquire))
            unsubscribe_audio(ctx);
        subscribe_audio(ctx);
    }
}

static void output_audio_frame(CoreVideoAudioSource *ctx,
                               uint32_t event_byte_len,
                               uint32_t resolved_participant_id)
{
    if (!ctx || event_byte_len == 0) return;

    std::lock_guard<std::mutex> lk(ctx->mtx);
    const std::string shm_name = IPC_SHM_PREFIX + ctx->source_uuid + "_audio";
    const size_t audio_bytes = sizeof(ShmAudioHeader) + event_byte_len;
    if ((!ctx->audio_shm.ptr || ctx->audio_shm.size < audio_bytes) &&
        !shm_region_open_read(ctx->audio_shm, shm_name, audio_bytes)) {
        if (ctx->frame_count == 0) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] Failed to open CoreVideo audio shared memory: source=%s uuid=%s bytes=%zu",
                 obs_source_get_name(ctx->source), ctx->source_uuid.c_str(),
                 audio_bytes);
        }
        return;
    }

    auto *hdr = static_cast<const ShmAudioHeader *>(ctx->audio_shm.ptr);
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
        if (!ctx->source || byte_len == 0) return;
        if (sizeof(ShmAudioHeader) + byte_len > ctx->audio_shm.size) return;
        const auto *pcm_src = static_cast<const uint8_t *>(ctx->audio_shm.ptr) +
            sizeof(ShmAudioHeader);
        if (ctx->audio_buf.size() < byte_len)
            ctx->audio_buf.resize(byte_len);
        std::memcpy(ctx->audio_buf.data(), pcm_src, byte_len);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t seq2 = hdr->sequence;
        if (seq1 == seq2 && (seq2 & 1u) == 0) {
            copied = true;
            break;
        }
    }
    if (!copied) return;

    const auto *pcm = reinterpret_cast<const int16_t *>(ctx->audio_buf.data());
    obs_source_audio audio = {};
    audio.samples_per_sec = sample_rate;
    audio.timestamp = os_gettime_ns();

    const AudioChannelMode audio_mode =
        ctx->audio_mode.load(std::memory_order_acquire);
    if (audio_mode == AudioChannelMode::Stereo && channels == 1) {
        const uint32_t mono_frames = byte_len / kZoomBytesPerSample;
        const uint32_t stereo_count = mono_frames * 2;
        if (ctx->stereo_buf.size() < stereo_count)
            ctx->stereo_buf.resize(stereo_count);
        for (uint32_t i = 0; i < mono_frames; ++i) {
            ctx->stereo_buf[i * 2] = pcm[i];
            ctx->stereo_buf[i * 2 + 1] = pcm[i];
        }
        audio.data[0] = reinterpret_cast<const uint8_t *>(ctx->stereo_buf.data());
        audio.frames = mono_frames;
        audio.format = AUDIO_FORMAT_16BIT;
        audio.speakers = SPEAKERS_STEREO;
    } else if (audio_mode == AudioChannelMode::Mono && channels == 2) {
        // True-stereo wire but the operator wants mono: average the pair
        // rather than dropping a side.
        const uint32_t frames = byte_len / (kZoomBytesPerSample * 2);
        if (ctx->stereo_buf.size() < frames)
            ctx->stereo_buf.resize(frames);
        for (uint32_t i = 0; i < frames; ++i) {
            ctx->stereo_buf[i] = static_cast<int16_t>(
                (static_cast<int32_t>(pcm[i * 2]) + pcm[i * 2 + 1]) / 2);
        }
        audio.data[0] = reinterpret_cast<const uint8_t *>(ctx->stereo_buf.data());
        audio.frames = frames;
        audio.format = AUDIO_FORMAT_16BIT;
        audio.speakers = SPEAKERS_MONO;
    } else {
        audio.data[0] = reinterpret_cast<const uint8_t *>(pcm);
        audio.frames = byte_len /
            (kZoomBytesPerSample * std::max<uint16_t>(channels, 1));
        audio.format = AUDIO_FORMAT_16BIT;
        audio.speakers = channels == 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
    }

    obs_source_output_audio(ctx->source, &audio);
    ++ctx->frame_count;
    if (ctx->frame_count == 1 || ctx->frame_count % 250 == 0) {
        blog(LOG_INFO,
             "[obs-zoom-plugin] Output CoreVideo audio frame: source=%s uuid=%s participant_id=%u count=%llu frames=%u sample_rate=%u channels=%u",
             obs_source_get_name(ctx->source), ctx->source_uuid.c_str(),
             resolved_participant_id,
             static_cast<unsigned long long>(ctx->frame_count),
             audio.frames, sample_rate, channels);
    }
}

static void apply_audio_settings(CoreVideoAudioSource *ctx, obs_data_t *settings)
{
    const uint32_t old_participant =
        ctx->participant_id.load(std::memory_order_acquire);
    const auto old_mode = ctx->audio_mode.load(std::memory_order_acquire);

    const uint32_t new_participant = static_cast<uint32_t>(
        obs_data_get_int(settings, PROP_PARTICIPANT_ID));
    const AudioChannelMode new_mode =
        obs_data_get_int(settings, PROP_AUDIO_CHANNELS) == AUDIO_CH_STEREO
        ? AudioChannelMode::Stereo : AudioChannelMode::Mono;

    ctx->participant_id.store(new_participant, std::memory_order_release);
    ctx->audio_mode.store(new_mode, std::memory_order_release);

    if (old_participant != new_participant || old_mode != new_mode)
        maybe_resubscribe_for_roster(ctx);
}

static void *audio_create_common(obs_data_t *settings, obs_source_t *source,
                                 CoreVideoAudioKind kind)
{
    auto *ctx = new CoreVideoAudioSource();
    ctx->source = source;
    ctx->source_uuid = make_audio_source_uuid();
    ctx->kind = kind;
    apply_audio_settings(ctx, settings);

    ZoomEngineClient::instance().register_source(ctx->source_uuid, {
        {},
        [ctx, gate = ctx->callback_gate](uint32_t byte_len,
                                         uint32_t participant_id) {
            std::lock_guard<std::mutex> callback_lock(gate->mtx);
            if (!gate->alive) return;
            output_audio_frame(ctx, byte_len, participant_id);
        }
    });
    ZoomEngineClient::instance().add_roster_callback(ctx,
        [ctx, gate = ctx->callback_gate]() {
            std::lock_guard<std::mutex> callback_lock(gate->mtx);
            if (!gate->alive) return;
            maybe_resubscribe_for_roster(ctx);
        });

    return ctx;
}

static void *participant_audio_create(obs_data_t *settings, obs_source_t *source)
{
    return audio_create_common(settings, source, CoreVideoAudioKind::Participant);
}

static void *active_speaker_audio_create(obs_data_t *settings, obs_source_t *source)
{
    return audio_create_common(settings, source, CoreVideoAudioKind::ActiveSpeaker);
}

static void *audience_audio_create(obs_data_t *settings, obs_source_t *source)
{
    return audio_create_common(settings, source, CoreVideoAudioKind::Audience);
}

static void audio_destroy(void *data)
{
    auto *ctx = static_cast<CoreVideoAudioSource *>(data);
    {
        std::lock_guard<std::mutex> callback_lock(ctx->callback_gate->mtx);
        ctx->callback_gate->alive = false;
    }
    ZoomEngineClient::instance().remove_roster_callback(ctx);
    unsubscribe_audio(ctx);
    ZoomEngineClient::instance().unregister_source(ctx->source_uuid);
    shm_region_destroy(ctx->audio_shm);
    delete ctx;
}

static void audio_update(void *data, obs_data_t *settings)
{
    apply_audio_settings(static_cast<CoreVideoAudioSource *>(data), settings);
}

static void audio_activate(void *data)
{
    auto *ctx = static_cast<CoreVideoAudioSource *>(data);
    ctx->active.store(true, std::memory_order_release);
    subscribe_audio(ctx);
}

static void audio_deactivate(void *data)
{
    auto *ctx = static_cast<CoreVideoAudioSource *>(data);
    ctx->active.store(false, std::memory_order_release);
    unsubscribe_audio(ctx);
}

static obs_properties_t *participant_audio_properties(void *)
{
    obs_properties_t *props = obs_properties_create();
    obs_property_t *plist = obs_properties_add_list(props, PROP_PARTICIPANT_ID,
        obs_module_text("ZoomParticipantAudio.ParticipantId"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(plist,
        obs_module_text("ZoomParticipantAudio.NoParticipant"), 0);
    for (const auto &p : ZoomEngineClient::instance().roster()) {
        std::string label = p.display_name.empty()
            ? "ID " + std::to_string(p.user_id)
            : p.display_name + " (" + std::to_string(p.user_id) + ")";
        if (p.is_talking) label += " [talking]";
        if (p.has_video) label += " [video]";
        obs_property_list_add_int(plist, label.c_str(),
                                  static_cast<long long>(p.user_id));
    }

    obs_property_t *ch = obs_properties_add_list(props, PROP_AUDIO_CHANNELS,
        obs_module_text("ZoomParticipantAudio.AudioChannels"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(ch, obs_module_text("ZoomParticipantAudio.AudioMono"),
                              AUDIO_CH_MONO);
    obs_property_list_add_int(ch, obs_module_text("ZoomParticipantAudio.AudioStereo"),
                              AUDIO_CH_STEREO);

    obs_properties_add_button(props, "btn_refresh",
        obs_module_text("ZoomParticipantAudio.RefreshParticipants"),
        [](obs_properties_t *, obs_property_t *, void *) -> bool { return true; });

    return props;
}

static obs_properties_t *auto_audio_properties(void *)
{
    obs_properties_t *props = obs_properties_create();
    obs_property_t *ch = obs_properties_add_list(props, PROP_AUDIO_CHANNELS,
        obs_module_text("ZoomParticipantAudio.AudioChannels"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(ch, obs_module_text("ZoomParticipantAudio.AudioMono"),
                              AUDIO_CH_MONO);
    obs_property_list_add_int(ch, obs_module_text("ZoomParticipantAudio.AudioStereo"),
                              AUDIO_CH_STEREO);
    return props;
}

static void audio_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, PROP_PARTICIPANT_ID, 0);
    obs_data_set_default_int(settings, PROP_AUDIO_CHANNELS, AUDIO_CH_MONO);
}

static const char *participant_audio_name(void *)
{
    return obs_module_text("ZoomParticipantAudio.Name");
}

static const char *active_speaker_audio_name(void *)
{
    return obs_module_text("CoreVideoActiveSpeakerAudio.Name");
}

static const char *audience_audio_name(void *)
{
    return obs_module_text("CoreVideoAudienceAudio.Name");
}

void zoom_participant_audio_source_register()
{
    struct obs_source_info participant = {};
    participant.id = "zoom_participant_audio_source";
    participant.type = OBS_SOURCE_TYPE_INPUT;
    participant.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    participant.get_name = participant_audio_name;
    participant.create = participant_audio_create;
    participant.destroy = audio_destroy;
    participant.update = audio_update;
    participant.activate = audio_activate;
    participant.deactivate = audio_deactivate;
    participant.get_properties = participant_audio_properties;
    participant.get_defaults = audio_defaults;
    obs_register_source(&participant);

    struct obs_source_info active = participant;
    active.id = "corevideo_active_speaker_audio_source";
    active.get_name = active_speaker_audio_name;
    active.create = active_speaker_audio_create;
    active.get_properties = auto_audio_properties;
    obs_register_source(&active);

    struct obs_source_info audience = participant;
    audience.id = "corevideo_audience_audio_source";
    audience.get_name = audience_audio_name;
    audience.create = audience_audio_create;
    audience.get_properties = auto_audio_properties;
    obs_register_source(&audience);
}
