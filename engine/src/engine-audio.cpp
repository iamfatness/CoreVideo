#include "engine-audio.h"
#include "engine-writer.h"
#include "tile-clock-log.h"
#if __has_include(<rawdata/zoom_rawdata_api.h>)
#include <rawdata/zoom_rawdata_api.h>
#else
#include <zoom_rawdata_api.h>
#endif
#include <atomic>
#include <cstring>

EngineAudio &EngineAudio::instance() { static EngineAudio inst; return inst; }

bool EngineAudio::init(IpcFd e2p_fd,
                       const std::string &source_uuid,
                       uint32_t participant_id,
                       bool isolate_audio,
                       bool audience_audio)
{
    // isolate wins if both are set — defensive, the plugin UI prevents this.
    if (isolate_audio) audience_audio = false;

    m_e2p_fd = e2p_fd;
    {
        std::lock_guard<std::mutex> lock(m_targets_mtx);
        auto it = m_targets.find(source_uuid);
        // Each audio target backs an SHM region — enforce the shared cap
        // (kMaxShmSources in engine-ipc.h). Re-registering is always allowed.
        if (shm_source_over_cap(m_targets.size(), it != m_targets.end())) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"audio_subscribe_rejected_capacity","source_uuid":")" +
                source_uuid + R"(","limit":)" +
                std::to_string(kMaxShmSources) + "}");
            EngineIpc::write(
                R"({"cmd":"error","msg":"subscribe_rejected","reason":"shm_capacity","source_uuid":")" +
                source_uuid + R"(","limit":)" +
                std::to_string(kMaxShmSources) + "}");
            return false;
        }
        if (it == m_targets.end()) {
            m_targets.emplace(source_uuid,
                std::make_unique<AudioTarget>(e2p_fd, participant_id,
                                              isolate_audio, audience_audio));
        } else if (it->second) {
            it->second->e2p_fd = e2p_fd;
            it->second->participant_id = participant_id;
            it->second->isolate_audio = isolate_audio;
            it->second->audience_audio = audience_audio;
        }
    }

    if (!m_raw_media_active) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"audio_subscribe_deferred","source_uuid":")" +
            source_uuid + R"(","reason":"raw_media_not_ready"})");
        return true;
    }

    return subscribe_if_needed(source_uuid, "audio_subscribe");
}

bool EngineAudio::subscribe_if_needed(const std::string &source_uuid,
                                      const std::string &stage)
{
    std::lock_guard<std::mutex> subscribe_lock(m_subscribe_mtx);
    if (m_subscribed) {
        // Already subscribed to the single mixed-audio stream; this call is
        // a no-op share. Emitting a debug line on every roster tick / source
        // added ~2300 lines to a 90-minute log for no signal — stay quiet.
        return true;
    }

    ZOOMSDK::IZoomSDKAudioRawDataHelper *helper = ZOOMSDK::GetAudioRawdataHelper();
    if (!helper) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"audio_helper_missing","source_uuid":")" +
            source_uuid + "\"}");
        return false;
    }

    ZOOMSDK::SDKError err = helper->subscribe(this);
    EngineIpc::write(
        R"({"cmd":"debug","stage":")" + stage +
        R"(","source_uuid":")" + source_uuid + R"(","code":)" +
        std::to_string(static_cast<int>(err)) + "}");
    if (err != ZOOMSDK::SDKERR_SUCCESS) return false;

    m_subscribed = true;
    return true;
}

bool EngineAudio::retry_subscribe(const std::string &reason)
{
    std::string source_uuid;
    {
        std::lock_guard<std::mutex> lock(m_targets_mtx);
        if (m_targets.empty()) return false;
        source_uuid = m_targets.begin()->first;
    }

    EngineIpc::write(
        R"({"cmd":"debug","stage":"audio_retry","source_uuid":")" +
        source_uuid + R"(","reason":")" + reason + "\"}");
    return subscribe_if_needed(source_uuid, "audio_resubscribe");
}

void EngineAudio::set_raw_media_active(bool active)
{
    std::lock_guard<std::mutex> subscribe_lock(m_subscribe_mtx);
    if (m_raw_media_active == active) return;
    m_raw_media_active = active;
    EngineIpc::write(
        R"({"cmd":"debug","stage":"audio_raw_media_state","active":)" +
        std::string(active ? "true" : "false") + "}");
}

void EngineAudio::reset_subscription(const std::string &reason)
{
    std::lock_guard<std::mutex> subscribe_lock(m_subscribe_mtx);
    if (!m_subscribed) return;

    ZOOMSDK::IZoomSDKAudioRawDataHelper *helper = ZOOMSDK::GetAudioRawdataHelper();
    if (helper) helper->unSubscribe();
    m_subscribed = false;
    EngineIpc::write(
        R"({"cmd":"debug","stage":"audio_subscription_reset","reason":")" +
        reason + "\"}");
}

void EngineAudio::shutdown()
{
    {
        std::lock_guard<std::mutex> subscribe_lock(m_subscribe_mtx);
        if (m_subscribed) {
            ZOOMSDK::IZoomSDKAudioRawDataHelper *helper =
                ZOOMSDK::GetAudioRawdataHelper();
            if (helper) helper->unSubscribe();
            m_subscribed = false;
        }
    }
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    for (auto &entry : m_targets) {
        if (entry.second) shm_region_destroy(entry.second->shm);
    }
    m_targets.clear();
}

void EngineAudio::remove(const std::string &source_uuid)
{
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    auto it = m_targets.find(source_uuid);
    if (it == m_targets.end()) return;
    if (it->second) shm_region_destroy(it->second->shm);
    m_targets.erase(it);
    EngineIpc::write(
        R"({"cmd":"debug","stage":"audio_target_removed","source_uuid":")" +
        source_uuid + "\"}");
}

bool EngineAudio::ensure_shm(AudioTarget &target,
                             const std::string &source_uuid,
                             uint32_t byte_len)
{
    const size_t total = shm_audio_region_bytes(byte_len);
    if (target.shm.ptr && target.shm.size >= total) return true;

    // Bump the generation BEFORE creating and bake it into the name: a
    // resize under the old name deadlocks while the plugin still maps the
    // old (smaller) section (see shm_region_name in engine-ipc.h).
    //
    // The counter is NOT a member of this target: EngineAudio::remove()
    // destroys it, and main.cpp calls that on every Unsubscribe and on every
    // video-only Subscribe, so a per-target counter restarted at 0 and put
    // every re-created region back on the legacy unsuffixed name. The "_audio"
    // base name gives audio its own counter without a special case — the table
    // is keyed by base name. See src/shm-generation.h.
    const ShmRegionAllocation region =
        shm_next_region(shm_generations(),
                        IPC_SHM_PREFIX + source_uuid + "_audio");
    if (!shm_region_create(target.shm, region.name, total)) return false;
    target.shm_gen = region.gen; // old plugin-side mappings are now stale

    // Initialise the header once, before any slot is published -- the reader
    // relies on slot_count and slot_bytes being correct from the first read.
    auto *ring = static_cast<ShmAudioHeader *>(target.shm.ptr);
    ring->write_index = 0;
    ring->slot_count  = kAudioRingSlots;
    ring->slot_bytes  = byte_len;
    ring->sample_rate = 0;
    ring->channels    = 0;
    ring->reserved    = 0;

    EngineIpc::write(
        R"({"cmd":"debug","stage":"audio_shm_created","source_uuid":")" +
        source_uuid + R"(","shm_gen":)" + std::to_string(region.gen) +
        R"(,"bytes":)" + std::to_string(total) + "}");
    return true;
}

void EngineAudio::output_audio_frame(AudioTarget &target,
                                     const std::string &source_uuid,
                                     AudioRawData *data,
                                     const char *stage)
{
    const uint32_t byte_len = data->GetBufferLen();
    if (byte_len == 0) return;

    if (!ensure_shm(target, source_uuid, byte_len) || !target.shm.ptr) {
        // Surface once per failure episode (not per callback) as a real error
        // so the plugin can show the operator that audio is being dropped.
        if (!target.shm_fail_reported) {
            target.shm_fail_reported = true;
            EngineIpc::write(
                R"({"cmd":"debug","stage":"audio_shm_create_failed","source_uuid":")" +
                source_uuid + R"(","byte_len":)" +
                std::to_string(byte_len) + R"(,"last_error":)" +
                std::to_string(target.shm.last_error) +
                // Which generation the failed create actually attempted.
                R"(,"attempted_gen":)" +
                std::to_string(shm_generations().issued(
                    IPC_SHM_PREFIX + source_uuid + "_audio")) + "}");
            EngineIpc::write(
                R"({"cmd":"error","msg":"shm_create_failed","source_uuid":")" +
                source_uuid + R"(","byte_len":)" +
                std::to_string(byte_len) + "}");
        }
        return;
    }
    if (target.shm_fail_reported) {
        target.shm_fail_reported = false;
        EngineIpc::write(
            R"({"cmd":"debug","stage":"audio_shm_recovered","source_uuid":")" +
            source_uuid + "\"}");
    }

    auto *ring = static_cast<ShmAudioHeader *>(target.shm.ptr);
    // A buffer larger than the slots we sized for cannot be published without
    // corrupting the neighbouring slot. ensure_shm() grows the region on the
    // next call; drop this one loudly rather than write out of bounds.
    if (byte_len > ring->slot_bytes) {
        EngineIpc::write(
            R"({"cmd":"error","msg":"audio_slot_too_small","source_uuid":")" +
            source_uuid + R"(","byte_len":)" + std::to_string(byte_len) +
            R"(,"slot_bytes":)" + std::to_string(ring->slot_bytes) + "}");
        return;
    }

    const uint32_t index = ring->write_index % ring->slot_count;
    auto *slot = reinterpret_cast<ShmAudioSlot *>(
        static_cast<char *>(target.shm.ptr) +
        shm_audio_slot_offset(*ring, index));

    uint32_t seq = slot->sequence + 1;
    if ((seq & 1u) == 0) ++seq;
    slot->sequence = seq;                       // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    // The engine has no libobs headers to call os_gettime_ns() with (it is a
    // standalone process); tile_clock_now_ns() is the same idea -- a
    // monotonic, QPC-backed clock (std::chrono::steady_clock on Windows) that
    // this process already uses for the tile clock probe. Both processes are
    // on one machine, so the plugin can still subtract this from its own
    // os_gettime_ns() reading to measure pipeline latency.
    slot->capture_ns = tile_clock_now_ns();
    slot->byte_len   = byte_len;
    slot->reserved   = 0;
    std::memcpy(reinterpret_cast<char *>(slot) + sizeof(ShmAudioSlot),
                data->GetBuffer(), byte_len);
    std::atomic_thread_fence(std::memory_order_release);
    slot->sequence = seq + 1;                   // even: readable

    ring->sample_rate = data->GetSampleRate();
    ring->channels    = static_cast<uint16_t>(data->GetChannelNum());
    std::atomic_thread_fence(std::memory_order_release);
    // Published last: the reader treats everything below write_index as
    // complete, so this store is what makes the slot visible.
    ring->write_index = (ring->write_index + 1) % ring->slot_count;

    ++target.frame_count;
    if (target.frame_count == 1 || target.frame_count % 250 == 0) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":")" + std::string(stage) +
            R"(","source_uuid":")" + source_uuid + R"(","count":)" +
            std::to_string(target.frame_count) + R"(,"sample_rate":)" +
            std::to_string(data->GetSampleRate()) + R"(,"channels":)" +
            std::to_string(data->GetChannelNum()) + R"(,"byte_len":)" +
            std::to_string(byte_len) + R"(,"participant_id":)" +
            std::to_string(target.participant_id) + "}");
    }

    EngineIpc::write(
        R"({"cmd":"audio","source_uuid":")" + source_uuid +
        R"(","participant_id":)" + std::to_string(target.participant_id) +
        R"(,"byte_len":)" + std::to_string(byte_len) +
        R"(,"shm_gen":)" + std::to_string(target.shm_gen) + "}");
}

void EngineAudio::onMixedAudioRawDataReceived(AudioRawData *data)
{
    if (!data || m_e2p_fd == kIpcInvalidFd || data->GetBufferLen() == 0) return;
    // Feed id 0: the mixed stream legitimately has no single participant.
    tile_clock_log(0, data->GetTimeStamp(), tile_clock_now_ns(), "a");

    std::lock_guard<std::mutex> lock(m_targets_mtx);
    for (auto &entry : m_targets) {
        if (!entry.second) continue;
        // Skip isolate AND audience targets — both receive only one-way audio.
        if (entry.second->isolate_audio || entry.second->audience_audio) continue;
        output_audio_frame(*entry.second, entry.first, data,
                           "audio_frame_received");
    }
}

void EngineAudio::onOneWayAudioRawDataReceived(AudioRawData *data, uint32_t user_id)
{
    if (!data || m_e2p_fd == kIpcInvalidFd || data->GetBufferLen() == 0) return;
    tile_clock_log(user_id, data->GetTimeStamp(), tile_clock_now_ns(), "a");

    std::lock_guard<std::mutex> lock(m_targets_mtx);

    // First pass: deliver to any isolate target bound to this user_id, and
    // determine whether this user is "claimed" by any isolate target.
    bool claimed_by_isolate = false;
    for (auto &entry : m_targets) {
        if (!entry.second || !entry.second->isolate_audio) continue;
        if (entry.second->participant_id != user_id) continue;
        claimed_by_isolate = true;
        output_audio_frame(*entry.second, entry.first, data,
                           "audio_one_way_frame_received");
    }

    // Second pass: audience targets get every non-isolated participant's
    // one-way audio. Because Zoom only fires this callback for active
    // talkers, an audience target naturally behaves as "active speaker
    // among the non-isolated set."
    if (claimed_by_isolate) return;
    for (auto &entry : m_targets) {
        if (!entry.second || !entry.second->audience_audio) continue;
        output_audio_frame(*entry.second, entry.first, data,
                           "audio_audience_frame_received");
    }
}
void EngineAudio::onShareAudioRawDataReceived(AudioRawData *, uint32_t) {}
void EngineAudio::onOneWayInterpreterAudioRawDataReceived(AudioRawData *,
                                                           const zchar_t *) {}
