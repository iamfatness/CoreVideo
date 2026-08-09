#include "engine-audio.h"
#include "engine-writer.h"
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
        EngineIpc::write(
            R"({"cmd":"debug","stage":"audio_target_added","source_uuid":")" +
            source_uuid + "\"}");
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
    const size_t total = sizeof(ShmAudioHeader) + byte_len;
    if (target.shm.ptr && target.shm.size >= total) return true;

    // Bump the generation BEFORE creating and bake it into the name: a
    // resize under the old name deadlocks while the plugin still maps the
    // old (smaller) section (see shm_region_name in engine-ipc.h).
    const uint32_t next_gen = target.shm_gen + 1;
    const std::string region_name =
        shm_region_name(IPC_SHM_PREFIX + source_uuid + "_audio", next_gen);
    if (!shm_region_create(target.shm, region_name, total)) return false;
    target.shm_gen = next_gen;
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
                std::to_string(target.shm.last_error) + "}");
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

    auto *hdr        = static_cast<ShmAudioHeader *>(target.shm.ptr);
    uint32_t seq = hdr->sequence + 1;
    if ((seq & 1u) == 0) ++seq;
    hdr->sequence = seq;
    std::atomic_thread_fence(std::memory_order_release);
    hdr->sample_rate = data->GetSampleRate();
    hdr->channels    = static_cast<uint16_t>(data->GetChannelNum());
    hdr->reserved    = 0;
    hdr->byte_len    = byte_len;
    std::memcpy(static_cast<char *>(target.shm.ptr) + sizeof(ShmAudioHeader),
                data->GetBuffer(), byte_len);
    std::atomic_thread_fence(std::memory_order_release);
    hdr->sequence = seq + 1;

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
