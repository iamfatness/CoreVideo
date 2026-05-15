#include "engine-audio.h"
#include "engine-writer.h"
#if __has_include(<rawdata/zoom_rawdata_api.h>)
#include <rawdata/zoom_rawdata_api.h>
#else
#include <zoom_rawdata_api.h>
#endif
#include <cstring>

EngineAudio &EngineAudio::instance() { static EngineAudio inst; return inst; }

bool EngineAudio::init(IpcFd e2p_fd, const std::string &source_uuid)
{
    m_e2p_fd = e2p_fd;
    {
        std::lock_guard<std::mutex> lock(m_targets_mtx);
        if (m_targets.find(source_uuid) == m_targets.end())
            m_targets.emplace(source_uuid, std::make_unique<AudioTarget>(e2p_fd));
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

    const std::string region_name = IPC_SHM_PREFIX + source_uuid + "_audio";
    return shm_region_create(target.shm, region_name, total);
}

void EngineAudio::onMixedAudioRawDataReceived(AudioRawData *data)
{
    if (!data || m_e2p_fd == kIpcInvalidFd) return;
    const uint32_t byte_len = data->GetBufferLen();
    if (byte_len == 0) return;

    std::lock_guard<std::mutex> lock(m_targets_mtx);
    for (auto &entry : m_targets) {
        const std::string &source_uuid = entry.first;
        AudioTarget &target = *entry.second;
        if (!ensure_shm(target, source_uuid, byte_len) || !target.shm.ptr) {
            if (target.frame_count == 0) {
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"audio_shm_create_failed","source_uuid":")" +
                    source_uuid + R"(","byte_len":)" +
                    std::to_string(byte_len) + "}");
            }
            continue;
        }

        auto *hdr        = static_cast<ShmAudioHeader *>(target.shm.ptr);
        hdr->sample_rate = data->GetSampleRate();
        hdr->channels    = static_cast<uint16_t>(data->GetChannelNum());
        hdr->byte_len    = byte_len;
        std::memcpy(static_cast<char *>(target.shm.ptr) + sizeof(ShmAudioHeader),
                    data->GetBuffer(), byte_len);

        ++target.frame_count;
        if (target.frame_count == 1 || target.frame_count % 250 == 0) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"audio_frame_received","source_uuid":")" +
                source_uuid + R"(","count":)" +
                std::to_string(target.frame_count) + R"(,"sample_rate":)" +
                std::to_string(data->GetSampleRate()) + R"(,"channels":)" +
                std::to_string(data->GetChannelNum()) + R"(,"byte_len":)" +
                std::to_string(byte_len) + "}");
        }

        EngineIpc::write(
            R"({"cmd":"audio","source_uuid":")" + source_uuid +
            R"(","byte_len":)" + std::to_string(byte_len) + "}");
    }
}

void EngineAudio::onOneWayAudioRawDataReceived(AudioRawData *, uint32_t) {}
void EngineAudio::onShareAudioRawDataReceived(AudioRawData *, uint32_t) {}
void EngineAudio::onOneWayInterpreterAudioRawDataReceived(AudioRawData *,
                                                           const zchar_t *) {}
