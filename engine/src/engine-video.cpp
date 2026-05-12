#include "engine-video.h"
#include "../../src/engine-ipc.h"
#include "../../third_party/zoom-sdk/h/zoom_rawdata_api.h"
#include <cstring>

ParticipantSubscription::ParticipantSubscription(uint32_t participant_id,
                                                 const std::string &source_uuid,
                                                 HANDLE e2p_pipe)
    : m_participant_id(participant_id),
      m_source_uuid(source_uuid),
      m_e2p_pipe(e2p_pipe)
{
    ZOOMSDK::SDKError err = ZOOMSDK::createRenderer(&m_renderer, this);
    if (err != ZOOMSDK::SDKERR_SUCCESS || !m_renderer) return;

    m_renderer->setRawDataResolution(ZOOMSDK::ZoomSDKResolution_1080P);
    m_renderer->subscribe(participant_id, ZOOMSDK::RAW_DATA_TYPE_VIDEO);
}

ParticipantSubscription::~ParticipantSubscription()
{
    if (m_renderer) {
        m_renderer->unSubscribe();
        ZOOMSDK::destroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
    if (m_shm_ptr)    UnmapViewOfFile(m_shm_ptr);
    if (m_shm_handle) CloseHandle(m_shm_handle);
}

void ParticipantSubscription::ensure_shm(uint32_t y_len)
{
    const size_t total = sizeof(ShmFrameHeader) + y_len + y_len / 4 + y_len / 4;
    if (m_shm_handle && m_shm_size >= total) return;

    if (m_shm_ptr)    { UnmapViewOfFile(m_shm_ptr);  m_shm_ptr    = nullptr; }
    if (m_shm_handle) { CloseHandle(m_shm_handle);   m_shm_handle = nullptr; }

    const std::string region_name = IPC_SHM_PREFIX + m_source_uuid;
    m_shm_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                      PAGE_READWRITE, 0,
                                      static_cast<DWORD>(total),
                                      region_name.c_str());
    m_shm_ptr  = MapViewOfFile(m_shm_handle, FILE_MAP_WRITE, 0, 0, total);
    m_shm_size = total;
}

void ParticipantSubscription::onRawDataFrameReceived(YUVRawDataI420 *data)
{
    if (!data) return;
    const uint32_t w     = data->GetStreamWidth();
    const uint32_t h     = data->GetStreamHeight();
    const uint32_t y_len = w * h;
    if (w == 0 || h == 0) return;

    ensure_shm(y_len);
    if (!m_shm_ptr) return;

    auto *hdr    = static_cast<ShmFrameHeader *>(m_shm_ptr);
    auto *pixels = static_cast<char *>(m_shm_ptr) + sizeof(ShmFrameHeader);
    hdr->width = w; hdr->height = h; hdr->y_len = y_len;

    std::memcpy(pixels,                   data->GetYBuffer(), y_len);
    std::memcpy(pixels + y_len,           data->GetUBuffer(), y_len / 4);
    std::memcpy(pixels + y_len + y_len/4, data->GetVBuffer(), y_len / 4);

    const std::string msg = R"({"cmd":"frame","source_uuid":")" + m_source_uuid +
        R"(","w":)" + std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}";
    DWORD written;
    WriteFile(m_e2p_pipe, (msg + "\n").c_str(),
              static_cast<DWORD>(msg.size() + 1), &written, nullptr);
}

void ParticipantSubscription::onRawDataStatusChanged(RawDataStatus) {}

void ParticipantSubscription::onRendererBeDestroyed()
{
    m_renderer = nullptr;
}

void EngineVideo::subscribe(uint32_t participant_id,
                             const std::string &source_uuid,
                             HANDLE e2p_pipe)
{
    m_subs[source_uuid] = std::make_unique<ParticipantSubscription>(
        participant_id, source_uuid, e2p_pipe);
}

void EngineVideo::unsubscribe(const std::string &source_uuid)
{
    m_subs.erase(source_uuid);
}
