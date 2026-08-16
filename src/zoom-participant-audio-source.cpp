#include "zoom-participant-audio-source.h"

#include "audio-subscription-state.h"
#include "engine-ipc.h"
#include "shm-resubscribe.h"
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

// CoreVideoAudioKind, AudioSubscriptionState and the two decisions they drive
// live in src/audio-subscription-state.h so they can be tested without OBS.

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
    // The two fields of AudioSubscriptionState, held as atomics because they
    // are touched from the OBS UI thread (activate/deactivate/update), the
    // engine reader thread (roster callbacks) and whichever thread called
    // ZoomEngineClient::start() (the new-engine callback). The rules that read
    // them live in src/audio-subscription-state.h.
    std::atomic<uint32_t> current_participant_id{0};
    std::atomic<bool> subscribed{false};
    std::atomic<bool> active{false};
    std::mutex mtx;
    ShmRegion audio_shm;
    uint32_t audio_shm_gen = 0;
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
        // Read the director, do not reconfigure it. This is called from the
        // audio path, so loading settings here meant an obs_frontend call off
        // the UI thread several times a second, re-applying the whole director
        // config — timings and exclusions — on every audio tick. Configuration
        // belongs where it changes: apply_settings(), subscribe(), the dock,
        // and the control/OSC servers.
        return ZoomEngineClient::instance().active_speaker_id();
    }
    if (ctx->kind == CoreVideoAudioKind::Participant)
        return ctx->participant_id.load(std::memory_order_acquire);
    return 0;
}

static void subscribe_audio(CoreVideoAudioSource *ctx)
{
    if (!ctx || ctx->source_uuid.empty()) return;

    const bool audience = ctx->kind == CoreVideoAudioKind::Audience;
    uint32_t target = 0;
    if (!audience) {
        target = target_participant_id(ctx);
        if (target == 0) return;
    }

    // Only claim the subscription if the command was actually handed to an
    // engine. ZoomEngineClient::subscribe_audio() drops it silently when no
    // engine is running or the pipe has broken, and both happen on exactly the
    // path this file has to survive: audio_activate() can fire while a
    // replacement engine is still being launched, in the window that
    // release_source_mappings_for_new_engine() opens. Marking a dropped
    // command as subscribed makes maybe_resubscribe_for_roster() read it as
    // "already done" and the source never asks again.
    //
    // It is not only the restart window. The same claim was being made every
    // time OBS started with one of these sources already on screen: the engine
    // has not been requested yet, so the subscribe went nowhere, and the flag
    // it set meant the roster tick after the eventual join did nothing. A
    // Participant- or Audience-kind source in that position was silent for the
    // whole session.
    //
    // Same rule ZoomOutputManager::resubscribe_all() adopted for video after
    // the 2026-08-09 incident: re-subscribe by intent, never trust a flag that
    // may have outlived the engine it described.
    if (!ZoomEngineClient::instance().subscribe_audio(ctx->source_uuid, target,
                                                      !audience, audience)) {
        // "No engine yet" is ordinary (OBS starts long before anyone joins);
        // a drop while an engine IS running means the link broke, which is not.
        const bool engine_running = ZoomEngineClient::instance().is_running();
        blog(engine_running ? LOG_WARNING : LOG_INFO,
             "[obs-zoom-plugin] CoreVideo audio subscribe was not delivered (%s): source=%s uuid=%s participant_id=%u - will retry on the next roster update",
             engine_running ? "engine link down" : "no engine running",
             obs_source_get_name(ctx->source), ctx->source_uuid.c_str(), target);
        return;
    }
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

// Drops this source's SHM read mapping. Called when a NEW ZoomObsEngine process
// comes up, and only then.
//
// These sources are the most exposed holder of the whole defect class and the
// last one to be covered. They map "<uuid>_audio" and hold it for the whole life
// of the source: audio_destroy() unmaps it, but that is teardown, not recovery,
// and nothing else in this file lets go. There is no video path here to
// piggyback on, and unlike ZoomSource nothing here releases before a
// re-subscribe. Within one engine process that is safe,
// because the engine's generation counter survives the AudioTarget that
// maybe_resubscribe_for_roster() destroys on every active-speaker change
// (src/shm-generation.h), so the rebuilt region always lands on a fresh _gN
// name. A new engine process throws that away: its table is empty, its first
// create asks for generation 1 — the legacy unsuffixed name this mapping is
// sitting on — and if the new region has to be larger, Windows refuses. The
// engine then emits audio_shm_create_failed and publishes no audio event, so
// nothing ever asks us to reopen and this source is silent for the session.
//
// Caller must hold nothing: this takes ctx->mtx, which output_audio_frame()
// holds while reading the region.
static void release_audio_mapping(CoreVideoAudioSource *ctx)
{
    if (!ctx) return;
    std::lock_guard<std::mutex> lk(ctx->mtx);
    const uint32_t dropped_gen = ctx->audio_shm_gen;
    if (!shm_release_for_resubscribe(ctx->audio_shm, ctx->audio_shm_gen)) return;
    blog(LOG_INFO,
         "[obs-zoom-plugin] Released CoreVideo audio shared memory for a new engine process: source=%s uuid=%s dropped_gen=%u",
         obs_source_get_name(ctx->source), ctx->source_uuid.c_str(), dropped_gen);
}

// Forgets the subscription this source believed it had, because the engine that
// held it no longer exists. Called from the same new-engine callback that
// releases the mapping, and only from there.
//
// Releasing the mapping alone does not restore audio. It clears the way for the
// new engine's first region create, but nothing re-subscribes these sources:
// the reconnect path's ZoomOutputManager::resubscribe_all() iterates ZoomSource
// only, and maybe_resubscribe_for_roster() — the one path that does reach here —
// treats a set `subscribed` as "already done" and returns. Left set, the
// Participant and Audience kinds stayed silent for the rest of the session and
// ActiveSpeaker recovered only if the resolved speaker happened to change.
//
// Sends nothing. on_new_engine_process must not talk to the engine (see
// SourceCallbacks in zoom-engine-client.h), and there is nothing to cancel: the
// subscription died with the old process.
//
// The re-subscribe is deliberately left to the roster path rather than issued
// here, because here is too early — the new engine has not been launched, let
// alone authenticated or joined. The new engine drives it instead: on reaching
// MEETING_STATUS_INMEETING it attaches the participants controller and
// immediately publishes a roster (EngineParticipants::attach(),
// engine/src/main.cpp), which the plugin turns into a roster callback for every
// registered source. So every successful rejoin produces at least one tick, and
// that tick now finds `subscribed` false and subscribes.
//
// Touches only the two atomics, so it needs no lock; the caller already holds
// the CallbackGate that keeps ctx alive.
static void forget_subscription_for_new_engine(CoreVideoAudioSource *ctx)
{
    if (!ctx) return;
    const AudioSubscriptionState cleared = audio_state_for_new_engine_process();
    const bool was_subscribed =
        ctx->subscribed.exchange(cleared.subscribed, std::memory_order_acq_rel);
    ctx->current_participant_id.store(cleared.participant_id,
                                      std::memory_order_release);
    if (!was_subscribed) return;
    blog(LOG_INFO,
         "[obs-zoom-plugin] Dropped CoreVideo audio subscription held with the previous engine: source=%s uuid=%s - will re-subscribe on the new engine's first roster",
         obs_source_get_name(ctx->source), ctx->source_uuid.c_str());
}

static void maybe_resubscribe_for_roster(CoreVideoAudioSource *ctx)
{
    if (!ctx) return;
    const bool active = ctx->active.load(std::memory_order_acquire);
    if (!active) return;

    const AudioSubscriptionState state{
        ctx->subscribed.load(std::memory_order_acquire),
        ctx->current_participant_id.load(std::memory_order_acquire)};
    // Audience follows no participant, and resolving a target is not free —
    // target_participant_id() also reconfigures and ticks the SpeakerDirector
    // for the ActiveSpeaker kind — so it is only asked of the kinds that use it.
    uint32_t target = ctx->kind == CoreVideoAudioKind::Audience
        ? 0 : target_participant_id(ctx);

    // Whether the participant this source currently holds is still in the
    // meeting. Only meaningful when we hold one; a source that holds nothing
    // reports `true` so the departure rule cannot fire on it. Without this the
    // subscription — and the shared-memory region behind it, one of
    // kMaxShmSources — is held for the rest of the session after its person
    // leaves, and a long show with roster churn exhausts the budget.
    //
    // The same roster answers whether the TARGET is present, and it has to:
    // releasing a departed participant without also refusing to re-subscribe to
    // them just oscillates. A scene collection saved from an earlier meeting
    // points at ids that no longer exist, so tick one released and tick two
    // subscribed straight back — 19 cycles per source in 41 seconds, measured
    // live on 2026-08-16. An absent target resolves to 0, which is the
    // already-documented "nobody resolved yet, ask again next tick" case.
    bool held_participant_present = true;
    const bool needs_roster =
        (state.subscribed && state.participant_id != 0) || target != 0;
    if (needs_roster) {
        const auto roster = ZoomEngineClient::instance().roster();
        const auto present = [&](uint32_t id) {
            return std::any_of(roster.begin(), roster.end(),
                               [&](const ParticipantInfo &p) {
                                   return p.user_id == id;
                               });
        };
        if (state.subscribed && state.participant_id != 0)
            held_participant_present = present(state.participant_id);
        if (target != 0 && !present(target))
            target = 0;
    }

    switch (audio_resubscribe_action(ctx->kind, active, state, target,
                                     held_participant_present)) {
    case AudioResubscribeAction::None:
        return;
    case AudioResubscribeAction::UnsubscribeThenSubscribe:
        unsubscribe_audio(ctx);
        subscribe_audio(ctx);
        return;
    case AudioResubscribeAction::Subscribe:
        subscribe_audio(ctx);
        return;
    case AudioResubscribeAction::Unsubscribe:
        blog(LOG_INFO,
             "[obs-zoom-plugin] Releasing CoreVideo audio subscription for a departed participant: source=%s uuid=%s participant_id=%u",
             obs_source_get_name(ctx->source), ctx->source_uuid.c_str(),
             state.participant_id);
        unsubscribe_audio(ctx);
        return;
    }
}

static void output_audio_frame(CoreVideoAudioSource *ctx,
                               uint32_t event_byte_len,
                               uint32_t resolved_participant_id,
                               uint32_t event_shm_gen)
{
    if (!ctx || event_byte_len == 0) return;

    std::lock_guard<std::mutex> lk(ctx->mtx);
    const std::string audio_base = IPC_SHM_PREFIX + ctx->source_uuid + "_audio";
    const std::string shm_name = shm_region_name(audio_base, event_shm_gen);
    const size_t audio_bytes = sizeof(ShmAudioHeader) + event_byte_len;
    const bool gen_changed = event_shm_gen != 0 &&
                             event_shm_gen != ctx->audio_shm_gen;
    if (ctx->audio_shm.ptr && gen_changed)
        shm_region_destroy(ctx->audio_shm);
    if (!ctx->audio_shm.ptr || ctx->audio_shm.size < audio_bytes) {
        if (!shm_region_open_read(ctx->audio_shm, shm_name, audio_bytes) &&
            // Engines predating suffixed names recreate the legacy name for
            // every generation — fall back to it.
            (event_shm_gen <= 1 ||
             !shm_region_open_read(ctx->audio_shm, audio_base, audio_bytes))) {
            if (ctx->frame_count == 0) {
                blog(LOG_WARNING,
                     "[obs-zoom-plugin] Failed to open CoreVideo audio shared memory: source=%s uuid=%s bytes=%zu gen=%u",
                     obs_source_get_name(ctx->source), ctx->source_uuid.c_str(),
                     audio_bytes, event_shm_gen);
            }
            return;
        }
        ctx->audio_shm_gen = event_shm_gen;
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

    if (ctx->audio_mode.load(std::memory_order_acquire) == AudioChannelMode::Stereo &&
        channels == 1) {
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
                                         uint32_t participant_id,
                                         uint32_t shm_gen) {
            std::lock_guard<std::mutex> callback_lock(gate->mtx);
            if (!gate->alive) return;
            output_audio_frame(ctx, byte_len, participant_id, shm_gen);
        },
        // Two things, and both are needed. Releasing the mapping unblocks the
        // new engine's first create for this region; forgetting the
        // subscription is what lets the roster path ask it for audio again.
        // Release first, so there is no instant at which this source could
        // subscribe while still holding a name the new engine wants.
        [ctx, gate = ctx->callback_gate]() {
            std::lock_guard<std::mutex> callback_lock(gate->mtx);
            if (!gate->alive) return;
            release_audio_mapping(ctx);
            forget_subscription_for_new_engine(ctx);
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
