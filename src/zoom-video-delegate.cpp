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

bool ZoomVideoDelegate::subscribe(uint32_t participant_id, VideoResolution res)
{
    unsubscribe();

    ZOOMSDK::IZoomSDKRenderer *renderer = nullptr;
    ZOOMSDK::SDKError err = ZOOMSDK::createRenderer(&renderer, this);
    if (err != ZOOMSDK::SDKERR_SUCCESS || !renderer) {
        blog(LOG_ERROR, "[obs-zoom-plugin] createRenderer failed: %d", static_cast<int>(err));
        return false;
    }

    ZOOMSDK::ZoomSDKResolution sdk_res;
    switch (res) {
    case VideoResolution::P360:  sdk_res = ZOOMSDK::ZoomSDKResolution_360P;  break;
    case VideoResolution::P720:  sdk_res = ZOOMSDK::ZoomSDKResolution_720P;  break;
    default:                     sdk_res = ZOOMSDK::ZoomSDKResolution_1080P; break;
    }
    renderer->setRawDataResolution(sdk_res);

    err = renderer->subscribe(participant_id, ZOOMSDK::RAW_DATA_TYPE_VIDEO);
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        blog(LOG_ERROR, "[obs-zoom-plugin] renderer->subscribe failed: %d", static_cast<int>(err));
        ZOOMSDK::destroyRenderer(renderer);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_renderer_mtx);
        m_renderer = renderer;
    }
    static const char *res_names[] = {"360P", "720P", "1080P"};
    blog(LOG_INFO, "[obs-zoom-plugin] Video subscribed for participant %u at %s",
         participant_id, res_names[static_cast<int>(res)]);
    return true;
}

void ZoomVideoDelegate::unsubscribe()
{
    ZOOMSDK::IZoomSDKRenderer *r = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_renderer_mtx);
        r = m_renderer;
        m_renderer = nullptr;
    }
    if (!r) return;
    m_active.store(false, std::memory_order_relaxed);
    m_width.store(0, std::memory_order_relaxed);
    m_height.store(0, std::memory_order_relaxed);
    r->unSubscribe();
    ZOOMSDK::destroyRenderer(r);
}

void ZoomVideoDelegate::onRawDataFrameReceived(YUVRawDataI420 *data)
{
    if (!data || !m_source) return;

    const uint32_t w = data->GetStreamWidth();
    const uint32_t h = data->GetStreamHeight();
    if (w == 0 || h == 0) return;
    m_width.store(w, std::memory_order_relaxed);
    m_height.store(h, std::memory_order_relaxed);

    obs_source_frame frame = {};
    frame.format      = VIDEO_FORMAT_I420;
    frame.width       = w;
    frame.height      = h;
    frame.timestamp   = os_gettime_ns();
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
    // The SDK is destroying the renderer; null our pointer without calling
    // destroyRenderer() ourselves (that would double-free).
    {
        std::lock_guard<std::mutex> lk(m_renderer_mtx);
        m_renderer = nullptr;
    }
    m_active.store(false, std::memory_order_relaxed);
    m_width.store(0, std::memory_order_relaxed);
    m_height.store(0, std::memory_order_relaxed);
    blog(LOG_INFO, "[obs-zoom-plugin] Video renderer destroyed by SDK");
}
