#include "engine-video.h"
#include "engine-writer.h"
#if __has_include(<rawdata/zoom_rawdata_api.h>)
#include <rawdata/zoom_rawdata_api.h>
#else
#include <zoom_rawdata_api.h>
#endif
#include <cstring>

ParticipantSubscription::ParticipantSubscription(uint32_t participant_id,
                                                 const std::string &source_uuid,
                                                 IpcFd e2p_fd)
    : m_participant_id(participant_id),
      m_source_uuid(source_uuid),
      m_e2p_fd(e2p_fd)
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
    shm_region_destroy(m_shm);
}

void ParticipantSubscription::ensure_shm(uint32_t y_len)
{
    const size_t total = sizeof(ShmFrameHeader) + y_len + y_len / 4 + y_len / 4;
    if (m_shm.ptr && m_shm.size >= total) return;

    const std::string region_name = IPC_SHM_PREFIX + m_source_uuid;
    shm_region_create(m_shm, region_name, total);
}

void ParticipantSubscription::onRawDataFrameReceived(YUVRawDataI420 *data)
{
    if (!data) return;
    const uint32_t w     = data->GetStreamWidth();
    const uint32_t h     = data->GetStreamHeight();
    const uint32_t y_len = w * h;
    if (w == 0 || h == 0) return;

    ensure_shm(y_len);
    if (!m_shm.ptr) return;

    auto *hdr    = static_cast<ShmFrameHeader *>(m_shm.ptr);
    auto *pixels = static_cast<char *>(m_shm.ptr) + sizeof(ShmFrameHeader);
    hdr->width = w; hdr->height = h; hdr->y_len = y_len;

    std::memcpy(pixels,                   data->GetYBuffer(), y_len);
    std::memcpy(pixels + y_len,           data->GetUBuffer(), y_len / 4);
    std::memcpy(pixels + y_len + y_len/4, data->GetVBuffer(), y_len / 4);

    EngineIpc::write(
        R"({"cmd":"frame","source_uuid":")" + m_source_uuid +
        R"(","w":)" + std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
}

void ParticipantSubscription::onRawDataStatusChanged(ZOOMSDK::IZoomSDKRendererDelegate::RawDataStatus) {}

void ParticipantSubscription::onRendererBeDestroyed()
{
    m_renderer = nullptr;
}

void EngineVideo::subscribe(uint32_t participant_id,
                             const std::string &source_uuid,
                             IpcFd e2p_fd)
{
    m_subs[source_uuid] = std::make_unique<ParticipantSubscription>(
        participant_id, source_uuid, e2p_fd);
}

void EngineVideo::unsubscribe(const std::string &source_uuid)
{
    m_subs.erase(source_uuid);
}
