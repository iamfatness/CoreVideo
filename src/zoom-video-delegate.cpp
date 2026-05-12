#include "zoom-video-delegate.h"
#include <rawdata/zoom_rawdata_api.h>
#include <obs-module.h>
#include <media-io/video-io.h>
#include <util/platform.h>

ZoomVideoDelegate::ZoomVideoDelegate(obs_source_t *source) : m_source(source) {}

ZoomVideoDelegate::~ZoomVideoDelegate()
{
    unsubscribe();
}

bool ZoomVideoDelegate::subscribe(uint32_t participant_id)
{
    if (m_renderer) unsubscribe();

    ZOOMSDK::SDKError err = ZOOMSDK::createRenderer(&m_renderer, this);
    if (err != ZOOMSDK::SDKERR_SUCCESS || !m_renderer) {
        blog(LOG_ERROR, "[obs-zoom-plugin] createRenderer failed: %d", static_cast<int>(err));
        return false;
    }

    // 1080P for broadcast quality
    m_renderer->setRawDataResolution(ZOOMSDK::ZoomSDKResolution_1080P);

    err = m_renderer->subscribe(participant_id, ZOOMSDK::RAW_DATA_TYPE_VIDEO);
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        blog(LOG_ERROR, "[obs-zoom-plugin] renderer->subscribe failed: %d", static_cast<int>(err));
        ZOOMSDK::destroyRenderer(m_renderer);
        m_renderer = nullptr;
        return false;
    }

    blog(LOG_INFO, "[obs-zoom-plugin] Video subscribed for participant %u at 1080P", participant_id);
    return true;
}

void ZoomVideoDelegate::unsubscribe()
{
    if (!m_renderer) return;
    m_renderer->unSubscribe();
    ZOOMSDK::destroyRenderer(m_renderer);
    m_renderer = nullptr;
    m_active.store(false, std::memory_order_relaxed);
    m_width.store(0, std::memory_order_relaxed);
    m_height.store(0, std::memory_order_relaxed);
}

void ZoomVideoDelegate::onRawDataFrameReceived(YUVRawDataI420 *data)
{
    if (!data || !m_source) return;

    const uint32_t w = data->GetStreamWidth();
    const uint32_t h = data->GetStreamHeight();
    if (w == 0 || h == 0) return;
    const uint64_t now = os_gettime_ns();
    m_width.store(w, std::memory_order_relaxed);
    m_height.store(h, std::memory_order_relaxed);
    m_last_frame_ns.store(now, std::memory_order_relaxed);
    m_frame_count.fetch_add(1, std::memory_order_relaxed);

    obs_source_frame frame = {};
    frame.format      = VIDEO_FORMAT_I420;
    frame.width       = w;
    frame.height      = h;
    frame.timestamp   = now;
    frame.data[0]     = reinterpret_cast<uint8_t *>(data->GetYBuffer());
    frame.data[1]     = reinterpret_cast<uint8_t *>(data->GetUBuffer());
    frame.data[2]     = reinterpret_cast<uint8_t *>(data->GetVBuffer());
    frame.linesize[0] = w;
    frame.linesize[1] = w / 2;
    frame.linesize[2] = w / 2;

    obs_source_output_video(m_source, &frame);
}

void ZoomVideoDelegate::onRawDataStatusChanged(RawDataStatus status)
{
    m_active.store(status == RawData_On, std::memory_order_relaxed);
    blog(LOG_INFO, "[obs-zoom-plugin] Video raw data %s",
         status == RawData_On ? "active" : "inactive");
}

void ZoomVideoDelegate::onRendererBeDestroyed()
{
    m_active.store(false, std::memory_order_relaxed);
    m_renderer = nullptr;
    blog(LOG_INFO, "[obs-zoom-plugin] Video renderer destroyed by SDK");
}
