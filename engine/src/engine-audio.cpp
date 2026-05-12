#include "engine-audio.h"
#include "../../src/engine-ipc.h"
#include "../../third_party/zoom-sdk/h/zoom_rawdata_api.h"
#include <cstring>

EngineAudio &EngineAudio::instance() { static EngineAudio inst; return inst; }

bool EngineAudio::init(HANDLE e2p_pipe, const std::string &source_uuid)
{
    m_e2p_pipe    = e2p_pipe;
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
    if (m_shm_ptr)    { UnmapViewOfFile(m_shm_ptr);  m_shm_ptr    = nullptr; }
    if (m_shm_handle) { CloseHandle(m_shm_handle);   m_shm_handle = nullptr; }
}

void EngineAudio::ensure_shm(uint32_t byte_len)
{
    const size_t total = sizeof(ShmAudioHeader) + byte_len;
    if (m_shm_handle && m_shm_size >= total) return;

    if (m_shm_ptr)    { UnmapViewOfFile(m_shm_ptr);  m_shm_ptr    = nullptr; }
    if (m_shm_handle) { CloseHandle(m_shm_handle);   m_shm_handle = nullptr; }

    const std::string region_name = IPC_SHM_PREFIX + m_source_uuid + "_audio";
    m_shm_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                      PAGE_READWRITE, 0,
                                      static_cast<DWORD>(total),
                                      region_name.c_str());
    m_shm_ptr  = MapViewOfFile(m_shm_handle, FILE_MAP_WRITE, 0, 0, total);
    m_shm_size = total;
}

void EngineAudio::onMixedAudioRawDataReceived(AudioRawData *data)
{
    if (!data || !m_shm_handle) return;
    const uint32_t byte_len = data->GetBufferLen();
    ensure_shm(byte_len);
    if (!m_shm_ptr) return;

    auto *hdr        = static_cast<ShmAudioHeader *>(m_shm_ptr);
    hdr->sample_rate = data->GetSampleRate();
    hdr->channels    = static_cast<uint16_t>(data->GetChannelNum());
    hdr->byte_len    = byte_len;
    std::memcpy(static_cast<char *>(m_shm_ptr) + sizeof(ShmAudioHeader),
                data->GetBuffer(), byte_len);

    const std::string msg =
        R"({"cmd":"audio","source_uuid":")" + m_source_uuid + "\"}";
    DWORD written;
    WriteFile(m_e2p_pipe, (msg + "\n").c_str(),
              static_cast<DWORD>(msg.size() + 1), &written, nullptr);
}

void EngineAudio::onOneWayAudioRawDataReceived(AudioRawData *, uint32_t) {}
void EngineAudio::onShareAudioRawDataReceived(AudioRawData *, uint32_t) {}
void EngineAudio::onOneWayInterpreterAudioRawDataReceived(AudioRawData *,
                                                           const zchar_t *) {}
