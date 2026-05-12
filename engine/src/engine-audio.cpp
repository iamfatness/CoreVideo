#include "engine-audio.h"
#include "../../src/engine-ipc.h"
#include <cstring>
#include <string>
EngineAudio &EngineAudio::instance() { static EngineAudio inst; return inst; }
void EngineAudio::init(HANDLE e2p_pipe, const std::string &source_uuid)
{ m_e2p_pipe = e2p_pipe; m_source_uuid = source_uuid; }
void EngineAudio::ensure_shm(uint32_t byte_len)
{
    const size_t total = sizeof(ShmAudioHeader) + byte_len;
    if (m_shm_handle && m_shm_size >= total) return;
    if (m_shm_ptr)    { UnmapViewOfFile(m_shm_ptr);  m_shm_ptr    = nullptr; }
    if (m_shm_handle) { CloseHandle(m_shm_handle);   m_shm_handle = nullptr; }
    const std::string region_name = IPC_SHM_PREFIX + m_source_uuid + "_audio";
    m_shm_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(total), region_name.c_str());
    m_shm_ptr  = MapViewOfFile(m_shm_handle, FILE_MAP_WRITE, 0, 0, total);
    m_shm_size = total;
}
void EngineAudio::onMixedAudioRawDataReceived(ZOOM_SDK_NAMESPACE::AudioRawData *data)
{
    if (!data) return;
    const uint32_t byte_len = data->GetBufferLen();
    ensure_shm(byte_len);
    if (!m_shm_ptr) return;
    auto *hdr = static_cast<ShmAudioHeader *>(m_shm_ptr);
    hdr->sample_rate = data->GetSampleRate();
    hdr->channels    = static_cast<uint16_t>(data->GetChannelNum());
    hdr->byte_len    = byte_len;
    std::memcpy(static_cast<char *>(m_shm_ptr) + sizeof(ShmAudioHeader), data->GetBuffer(), byte_len);
    const std::string line = R"({"cmd":"audio","source_uuid":")" + m_source_uuid + "\"}\n";
    DWORD written;
    WriteFile(m_e2p_pipe, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
}
void EngineAudio::onOneWayAudioRawDataReceived(ZOOM_SDK_NAMESPACE::AudioRawData *, uint32_t) {}
