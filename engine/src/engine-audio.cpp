#include "engine-audio.h"
#include "../../src/engine-ipc.h"
#include "../../third_party/zoom-sdk/h/zoom_rawdata_api.h"
#include <cstring>

EngineAudio &EngineAudio::instance() { static EngineAudio inst; return inst; }

bool EngineAudio::init(IpcFd e2p_fd, const std::string &source_uuid)
{
    m_e2p_fd      = e2p_fd;
    m_source_uuid = source_uuid;

    if (m_subscribed) return true;

    ZOOMSDK::IZoomSDKAudioRawDataHelper *helper = ZOOMSDK::GetAudioRawdataHelper();
    if (!helper) return false;

    ZOOMSDK::SDKError err = helper->subscribe(this);
    if (err != ZOOMSDK::SDKERR_SUCCESS) return false;

    m_subscribed = true;
    return true;
}

void EngineAudio::shutdown()
{
    if (!m_subscribed) return;
    ZOOMSDK::IZoomSDKAudioRawDataHelper *helper = ZOOMSDK::GetAudioRawdataHelper();
    if (helper) helper->unSubscribe();
    m_subscribed = false;
    shm_region_destroy(m_shm);
}

void EngineAudio::ensure_shm(uint32_t byte_len)
{
    const size_t total = sizeof(ShmAudioHeader) + byte_len;
    if (m_shm.ptr && m_shm.size >= total) return;

    const std::string region_name = IPC_SHM_PREFIX + m_source_uuid + "_audio";
    shm_region_create(m_shm, region_name, total);
}

void EngineAudio::onMixedAudioRawDataReceived(AudioRawData *data)
{
    if (!data || m_e2p_fd == kIpcInvalidFd) return;
    const uint32_t byte_len = data->GetBufferLen();
    ensure_shm(byte_len);
    if (!m_shm.ptr) return;

    auto *hdr        = static_cast<ShmAudioHeader *>(m_shm.ptr);
    hdr->sample_rate = data->GetSampleRate();
    hdr->channels    = static_cast<uint16_t>(data->GetChannelNum());
    hdr->byte_len    = byte_len;
    std::memcpy(static_cast<char *>(m_shm.ptr) + sizeof(ShmAudioHeader),
                data->GetBuffer(), byte_len);

    ipc_write_line(m_e2p_fd,
        R"({"cmd":"audio","source_uuid":")" + m_source_uuid + "\"}");
}

void EngineAudio::onOneWayAudioRawDataReceived(AudioRawData *, uint32_t) {}
void EngineAudio::onShareAudioRawDataReceived(AudioRawData *, uint32_t) {}
void EngineAudio::onOneWayInterpreterAudioRawDataReceived(AudioRawData *,
                                                           const zchar_t *) {}
