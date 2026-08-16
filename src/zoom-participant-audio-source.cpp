#include "zoom-participant-audio-source.h"

#include "audio-subscription-state.h"
#include "audio-timeline.h"
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

static constexpr uint32_t kMaxAudioDelayMs = 500;

// The operator's global delay trim for the DEDICATED audio path, in ms.
//
// File-scope and read straight out of the publish expression in
// output_audio_frame(), NOT cached per source. That is the whole mechanism: a
// change takes effect on the very next buffer, on every live source, with no
// restart, no per-source push and no broadcast machinery -- and there is no
// broadcast machinery anywhere in this plugin to build it on. The previous
// per-source copy was only refreshed from apply_audio_settings(), i.e. when
// that ONE source's own properties dialog was OK'd, so a global settings
// change reached nothing that was already running.
//
// Seeded from ZoomPluginSettings::audio_delay_ms by apply_audio_settings()
// (create + update) and written by corevideo_set_global_audio_delay_ms() from
// the settings dialog. Always clamped to 0-kMaxAudioDelayMs at every write
// site: ZoomPluginSettings::save() does not clamp, so config_get_int() can
// hand back anything that is on disk.
static std::atomic<uint32_t> g_global_audio_delay_ms{0};

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
    // The master clock this source's audio is stamped from. Not
    // self-synchronizing (see src/audio-timeline.h), so every access is
    // guarded by ctx->mtx: advanced there by the engine reader thread in
    // output_audio_frame(), reset there by the OBS lifecycle callbacks
    // (unsubscribe_audio()) and the new-engine callback
    // (forget_subscription_for_new_engine()).
    AudioTimeline timeline;
    // Next ring slot this source will drain. Only the engine reader thread
    // touches it, the same thread that owns `timeline`.
    uint32_t read_index    = 0;
    bool     read_started  = false;
    // Slots the writer lapped before we drained them -- audio that was lost.
    // Counted so loss is visible; the old mailbox lost audio invisibly.
    uint64_t overrun_slots = 0;
    // How many times output_audio_frame() had to reopen the mapping because
    // the ring header described a region larger than our view. Rate-limits
    // that log; nonzero is diagnostic, not fatal.
    uint64_t remap_count = 0;
    // Engine capture to OBS publish, microseconds. 0 = not yet measured. Set
    // in output_audio_frame() against ShmAudioSlot::capture_ns. Deliberately
    // NOT in ZoomOutputInfo -- this source type is not a ZoomSource and is not
    // registered with ZoomOutputManager, so it can never appear in
    // list_outputs. It is surfaced through the registry below and the control
    // API's list_audio_sources instead.
    std::atomic<uint64_t> audio_latency_us{0};
};

// Live-instance registry.
//
// Mirrors ZoomOutputManager's register_source()/unregister_source() pattern for
// ZoomSource -- a mutex plus a vector of raw pointers, with registration in
// create and removal in destroy -- because nothing equivalent existed for these
// sources. Without it, overrun_slots and audio_latency_us were private fields
// reaching nothing but a rate-limited blog(), and the spec's requirement to
// surface loss and latency in the control API could not be met (nor its
// acceptance test run).
//
// LOCK ORDER: g_sources_mtx before any ctx->mtx, never the reverse. The only
// place both are taken is corevideo_audio_source_infos(); the audio reader
// thread takes ctx->mtx and never touches g_sources_mtx. audio_destroy()
// unregisters BEFORE `delete ctx`, so a pointer held under g_sources_mtx is
// always live.
static std::mutex g_sources_mtx;
static std::vector<CoreVideoAudioSource *> g_sources;

static const char *audio_kind_id(CoreVideoAudioKind kind)
{
    switch (kind) {
    case CoreVideoAudioKind::ActiveSpeaker: return "active_speaker";
    case CoreVideoAudioKind::Audience:      return "audience";
    case CoreVideoAudioKind::Participant:
    default:                                return "participant";
    }
}

uint32_t corevideo_set_global_audio_delay_ms(uint32_t delay_ms)
{
    const uint32_t clamped = std::min(delay_ms, kMaxAudioDelayMs);
    g_global_audio_delay_ms.store(clamped, std::memory_order_relaxed);
    return clamped;
}

std::vector<CoreVideoAudioSourceInfo> corevideo_audio_source_infos()
{
    const uint32_t delay_ms =
        g_global_audio_delay_ms.load(std::memory_order_relaxed);
    std::vector<CoreVideoAudioSourceInfo> out;
    std::lock_guard<std::mutex> lk(g_sources_mtx);
    out.reserve(g_sources.size());
    for (CoreVideoAudioSource *ctx : g_sources) {
        if (!ctx) continue;
        CoreVideoAudioSourceInfo info;
        const char *name = ctx->source ? obs_source_get_name(ctx->source)
                                       : nullptr;
        info.source_name = name ? name : std::string();
        info.source_uuid = ctx->source_uuid;
        info.kind = audio_kind_id(ctx->kind);
        info.participant_id =
            ctx->current_participant_id.load(std::memory_order_acquire);
        info.subscribed = ctx->subscribed.load(std::memory_order_acquire);
        info.audio_delay_ms = delay_ms;
        info.audio_latency_us =
            ctx->audio_latency_us.load(std::memory_order_relaxed);
        {
            // overrun_slots and frame_count are plain members owned by the
            // engine reader thread under ctx->mtx -- the same lock
            // output_audio_frame() holds for the whole drain. Read them the way
            // that path writes them.
            std::lock_guard<std::mutex> ctx_lk(ctx->mtx);
            info.overrun_slots = ctx->overrun_slots;
            info.frame_count   = ctx->frame_count;
        }
        out.push_back(std::move(info));
    }
    return out;
}

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
    // The next subscribe is a new timeline: a different participant, or the
    // same one after a gap of unknown length. Neither can be stamped from the
    // old sample count. Reset under ctx->mtx: output_audio_frame() holds the
    // same mutex while advancing this timeline on the engine reader thread,
    // and AudioTimeline has no synchronization of its own.
    {
        std::lock_guard<std::mutex> lk(ctx->mtx);
        audio_timeline_reset(ctx->timeline);
        ctx->read_started = false;
    }
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
// Touches the two atomics lock-free; the caller already holds the
// CallbackGate that keeps ctx alive. It also resets ctx->timeline, which is
// NOT atomic and is shared with the engine reader thread (output_audio_frame()
// advances it under ctx->mtx), so that reset takes ctx->mtx explicitly rather
// than relying on this function's own lock-free style.
static void forget_subscription_for_new_engine(CoreVideoAudioSource *ctx)
{
    if (!ctx) return;
    const AudioSubscriptionState cleared = audio_state_for_new_engine_process();
    const bool was_subscribed =
        ctx->subscribed.exchange(cleared.subscribed, std::memory_order_acq_rel);
    ctx->current_participant_id.store(cleared.participant_id,
                                      std::memory_order_release);
    // A new engine restarts its own generation counters; our accumulated
    // samples describe a process that no longer exists. Reset under ctx->mtx:
    // AudioTimeline has no synchronization of its own and the engine reader
    // thread advances the same timeline under the same mutex.
    {
        std::lock_guard<std::mutex> lk(ctx->mtx);
        audio_timeline_reset(ctx->timeline);
        ctx->read_started = false;
    }
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
    // engine-audio.cpp sizes and writes this region as an 8-slot ring
    // (src/engine-ipc.h) — the mapping must cover the whole ring, not just
    // header-plus-one-buffer.
    const size_t audio_bytes = shm_audio_region_bytes(event_byte_len);
    const bool gen_changed = event_shm_gen != 0 &&
                             event_shm_gen != ctx->audio_shm_gen;
    if (ctx->audio_shm.ptr && gen_changed) {
        shm_region_destroy(ctx->audio_shm);
        // A new region starts its own write_index at 0; our read_index
        // described the region we just let go of.
        ctx->read_started = false;
    }
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
        ctx->read_started = false;
    }

    if (!ctx->source) return;

    auto *ring = static_cast<const ShmAudioHeader *>(ctx->audio_shm.ptr);

    // A mismatched wire format must fail loudly, not walk off into whatever
    // offsets the old/new layout implies. slot_count != kAudioRingSlots means
    // either an engine speaking a different generation of this format, or a
    // region we opened before it was ever initialised -- neither is safe to
    // read as this ring.
    if (ring->slot_count != kAudioRingSlots) {
        if (ctx->frame_count == 0) {
            blog(LOG_ERROR,
                 "[obs-zoom-plugin] CoreVideo audio ring version mismatch: source=%s uuid=%s expected_slot_count=%u actual_slot_count=%u",
                 obs_source_get_name(ctx->source), ctx->source_uuid.c_str(),
                 kAudioRingSlots, ring->slot_count);
        }
        return;
    }

    // ensure_shm() only ever grows slot_bytes. The mapping this call opened
    // may have been sized against a SMALLER event_byte_len than the region
    // now actually holds (e.g. a target whose audio role flipped Mix (stereo,
    // 1920 B) to Isolated (mono, 960 B) without an unsubscribe: the region
    // keeps slot_bytes=1920 while a fresh mapping asks for only 7892 bytes of
    // a 15572-byte ring). Trusting ring->slot_bytes to derive a slot offset
    // without checking this reads past our own mapped VIEW, even when the
    // underlying region is large enough -- an access violation, not a logic
    // error.
    //
    // Simply returning here MUTED THE SOURCE FOREVER: the mapping is only
    // reopened when audio_shm.size < shm_audio_region_bytes(event_byte_len),
    // and event_byte_len is the CURRENT buffer size, which in this scenario is
    // the SMALL one -- so the condition that got us here could never untrip,
    // and after frame_count > 0 it stopped logging too. The header is inside
    // the view we already hold, so the correct size is known: reopen at it
    // now, in this same call.
    if (shm_audio_region_bytes(ring->slot_bytes) > ctx->audio_shm.size) {
        const size_t mapped_bytes = ctx->audio_shm.size;
        const uint32_t described_slot_bytes = ring->slot_bytes;
        const size_t needed_bytes = shm_audio_region_bytes(described_slot_bytes);
        ring = nullptr; // the view backing it is about to be unmapped
        shm_region_destroy(ctx->audio_shm);
        if (!shm_region_open_read(ctx->audio_shm, shm_name, needed_bytes) &&
            (event_shm_gen <= 1 ||
             !shm_region_open_read(ctx->audio_shm, audio_base, needed_bytes))) {
            ctx->read_started = false;
            ++ctx->remap_count;
            if (ctx->remap_count == 1 || ctx->remap_count % 250 == 0) {
                blog(LOG_WARNING,
                     "[obs-zoom-plugin] CoreVideo audio remap at the ring's own size FAILED: source=%s uuid=%s needed_bytes=%zu gen=%u",
                     obs_source_get_name(ctx->source), ctx->source_uuid.c_str(),
                     needed_bytes, event_shm_gen);
            }
            return;
        }
        ctx->audio_shm_gen = event_shm_gen;
        // Same named region, same writer, same free-running write_index -- the
        // view got bigger, nothing restarted -- so read_index stays valid and
        // read_started stays as it was.
        ring = static_cast<const ShmAudioHeader *>(ctx->audio_shm.ptr);
        ++ctx->remap_count;
        if (ctx->remap_count == 1 || ctx->remap_count % 250 == 0) {
            blog(LOG_INFO,
                 "[obs-zoom-plugin] CoreVideo audio mapping reopened at the ring's own size: source=%s uuid=%s was_bytes=%zu now_bytes=%zu slot_bytes=%u count=%llu",
                 obs_source_get_name(ctx->source), ctx->source_uuid.c_str(),
                 mapped_bytes, ctx->audio_shm.size, described_slot_bytes,
                 static_cast<unsigned long long>(ctx->remap_count));
        }
        // Re-validate against the NEW view: the writer may have moved on again
        // between the unmap and the remap, and the version guard above ran on
        // a header we no longer hold.
        if (ring->slot_count != kAudioRingSlots ||
            shm_audio_region_bytes(ring->slot_bytes) > ctx->audio_shm.size) {
            return;
        }
    }
    const uint32_t slot_count = ring->slot_count;

    // First event after a (re)subscribe: start level with the writer rather
    // than replaying whatever stale audio the region still holds.
    if (!ctx->read_started) {
        ctx->read_index   = ring->write_index;
        ctx->read_started = true;
        return;
    }

    // Nominal duration of one slot, from the ring header rather than any
    // individual slot's (possibly unread or unreadable) byte_len -- this is
    // what lets every discard path below keep the master clock honest about
    // audio we never got to look at.
    const uint32_t nominal_frames = ring->slot_bytes /
        (kZoomBytesPerSample * std::max<uint16_t>(ring->channels, 1));

    // write_index and read_index are free-running counters (never wrap at
    // slot_count -- see ShmAudioHeader::write_index), so "pending" is exact:
    // it cannot collapse a full lap into "caught up" the way a
    // slot_count-modulo difference would.
    const uint32_t write_index = ring->write_index;
    uint32_t pending = audio_ring_slots_behind(write_index, ctx->read_index,
                                               slot_count);
    // More has been written since our last drain than the ring can hold: the
    // oldest `pending - slot_count` generations were overwritten before we
    // ever read them. Skip to the oldest slot still intact and count -- and
    // clock-compensate -- what was lost. The point of the ring is that this
    // is now visible, not that it can never happen.
    if (pending > slot_count) {
        const uint32_t lost = pending - slot_count;
        const uint64_t overrun_before = ctx->overrun_slots;
        ctx->overrun_slots += lost;
        audio_timeline_skip(ctx->timeline, lost * nominal_frames);
        ctx->read_index = write_index - slot_count;
        pending = slot_count;
        // Rate-limited the same way every other high-frequency log in this
        // file is: first occurrence loud, then periodically (see the
        // frame_count == 1 || frame_count % 250 == 0 pattern below). Without
        // this, a sustained overrun at Zoom's ~100 callbacks/sec would put up
        // to 900 blog() calls/sec -- each taking libobs' log lock and writing
        // to disk, under ctx->mtx -- on the exact path that is already
        // CPU-starved, amplifying the stall being reported. overrun_slots is
        // already maintained cumulatively for Task 7 to surface; this only
        // decides when a change in it also goes to the log.
        if (overrun_before == 0 ||
            overrun_before / 250 != ctx->overrun_slots / 250) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] CoreVideo audio ring overrun: source=%s uuid=%s lost_slots=%llu",
                 obs_source_get_name(ctx->source), ctx->source_uuid.c_str(),
                 static_cast<unsigned long long>(ctx->overrun_slots));
        }
    }

    for (uint32_t n = 0; n < pending; ++n) {
        const uint32_t slot_index = ctx->read_index; // free-running generation
        const auto *slot = reinterpret_cast<const ShmAudioSlot *>(
            static_cast<const char *>(ctx->audio_shm.ptr) +
            shm_audio_slot_offset(*ring, slot_index % slot_count));

        uint32_t byte_len = 0;
        uint64_t capture_ns = 0;
        bool copied = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const uint32_t seq1 = slot->sequence;
            std::atomic_thread_fence(std::memory_order_acquire);
            if ((seq1 & 1u) != 0) continue;      // write in progress
            byte_len   = slot->byte_len;
            capture_ns = slot->capture_ns;
            // The payload can never exceed the slot the engine sized for it;
            // a larger value means we are reading a region the writer has
            // since resized, so drop it rather than read out of bounds.
            if (byte_len == 0 || byte_len > ring->slot_bytes) break;
            if (ctx->audio_buf.size() < byte_len)
                ctx->audio_buf.resize(byte_len);
            std::memcpy(ctx->audio_buf.data(),
                        reinterpret_cast<const char *>(slot) +
                            sizeof(ShmAudioSlot),
                        byte_len);
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint32_t seq2 = slot->sequence;
            if (seq1 == seq2 && (seq2 & 1u) == 0) { copied = true; break; }
        }

        // The seqlock only proves the payload was not TORN -- it does not
        // prove it is the buffer we MEANT to read. If the writer has since
        // advanced far enough past this generation, it has physically
        // overwritten this slot at least once more while we were copying it:
        // what we just read cleanly may be a newer buffer that happened to
        // land on an even/even pair, about to be published in this older
        // slot's timeline position.
        if (copied && ring->write_index - slot_index > slot_count) {
            copied = false;
            const uint64_t overrun_before = ctx->overrun_slots;
            ++ctx->overrun_slots;
            // Same rate-limit, and the same reason, as the overrun-skip log
            // above -- this one sits INSIDE the per-slot loop and could
            // otherwise fire up to slot_count times per callback. Sharing the
            // gate on the one overrun_slots counter (rather than a second
            // one) keeps "first occurrence, then periodically" meaningful
            // regardless of which of the two loss paths produced it. This
            // check can also false-positive if the reader is preempted
            // between the seqlock's seq2 read and this write_index re-read --
            // a still-valid buffer gets discarded and counted -- which is a
            // second reason not to treat every occurrence as log-worthy.
            if (overrun_before == 0 ||
                overrun_before / 250 != ctx->overrun_slots / 250) {
                blog(LOG_WARNING,
                     "[obs-zoom-plugin] CoreVideo audio slot overwritten mid-read: source=%s uuid=%s lost_slots=%llu",
                     obs_source_get_name(ctx->source), ctx->source_uuid.c_str(),
                     static_cast<unsigned long long>(ctx->overrun_slots));
            }
        }

        ctx->read_index = slot_index + 1;
        if (!copied) {
            // Lost this slot's audio, whichever way: unread, unreadably torn
            // after three attempts, or clobbered mid-copy above. Advance the
            // timeline anyway -- a lost slot is silence of a KNOWN duration,
            // and skipping the accounting shifts this source permanently
            // earlier relative to everything else on the timeline (see
            // src/audio-timeline.h's doctrine on gaps).
            audio_timeline_skip(ctx->timeline, nominal_frames);
            continue;
        }

        const uint32_t sample_rate = ring->sample_rate;
        const uint16_t channels    = ring->channels;
        const uint64_t now_ns = os_gettime_ns();

        // Both processes share a QPC-based monotonic clock, so this
        // subtraction is meaningful across the boundary -- see
        // ShmAudioSlot::capture_ns in engine-ipc.h.
        if (capture_ns != 0 && now_ns > capture_ns) {
            ctx->audio_latency_us.store((now_ns - capture_ns) / 1000,
                                        std::memory_order_relaxed);
        }

        const auto *pcm = reinterpret_cast<const int16_t *>(ctx->audio_buf.data());
        obs_source_audio audio = {};
        audio.samples_per_sec = sample_rate;
        // Sample-derived, not arrival-derived: IPC jitter must not reach OBS.
        // `frames` is what this buffer actually carries, so the timeline advances
        // by exactly the audio published. See src/audio-timeline.h.
        const uint32_t timeline_frames =
            byte_len / (kZoomBytesPerSample * std::max<uint16_t>(channels, 1));
        // Delay is arithmetic on the timeline, not a buffer: OBS's async path
        // holds timestamped audio until its time comes. Only ever pushes audio
        // LATER -- ITU-R BT.1359-1 detects audio leading at +45 ms but
        // tolerates lagging to -125 ms, so late is the safe direction to err.
        //
        // Read from the file-scope global here rather than a per-source cached
        // copy: that is what makes a change in Zoom Plugin Settings take effect
        // on the next buffer, on every live source, with no restart.
        audio.timestamp = audio_timeline_stamp(ctx->timeline, sample_rate,
                                               timeline_frames, now_ns) +
                          static_cast<uint64_t>(g_global_audio_delay_ms.load(
                              std::memory_order_relaxed)) * 1'000'000ULL;

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

    // The operator delay trim is deliberately GLOBAL to this source type, not
    // per source: it corrects the pipeline's audio-ahead-of-video bias, which
    // is a property of the pipeline, not of one participant. Re-seeding the
    // file-scope atomic here keeps a hand-edited global.ini working; the Zoom
    // Plugin Settings dialog writes the atomic directly, which is what makes
    // the change land on already-running sources. Clamp at every write site,
    // not just in load() -- ZoomPluginSettings::save() does not clamp and
    // config_get_int() returns whatever is on disk.
    corevideo_set_global_audio_delay_ms(
        ZoomPluginSettings::load().audio_delay_ms);

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

    {
        std::lock_guard<std::mutex> lk(g_sources_mtx);
        g_sources.push_back(ctx);
    }
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
        // Before anything else, and well before `delete ctx`: a reader holding
        // g_sources_mtx must never be handed a pointer that is about to die.
        std::lock_guard<std::mutex> lk(g_sources_mtx);
        g_sources.erase(std::remove(g_sources.begin(), g_sources.end(), ctx),
                        g_sources.end());
    }
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
