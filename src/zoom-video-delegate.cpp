#include "zoom-video-delegate.h"
#include <obs-module.h>
#include <media-io/video-io.h>
#include <util/platform.h>
ZoomVideoDelegate::ZoomVideoDelegate(obs_source_t *source) : m_source(source) {}
void ZoomVideoDelegate::onRawDataFrameReceived(ZOOM_SDK_NAMESPACE::YUVRawDataI420 *data)
{
    if (!data || !m_source) return;
    const uint32_t w = data->GetStreamWidth(), h = data->GetStreamHeight();
    if (w == 0 || h == 0) return;
    obs_source_frame frame = {};
    frame.format     = VIDEO_FORMAT_I420;
    frame.width      = w;
    frame.height     = h;
    frame.timestamp  = os_gettime_ns();
    frame.data[0]    = reinterpret_cast<uint8_t *>(data->GetYBuffer());
    frame.data[1]    = reinterpret_cast<uint8_t *>(data->GetUBuffer());
    frame.data[2]    = reinterpret_cast<uint8_t *>(data->GetVBuffer());
    frame.linesize[0] = w; frame.linesize[1] = w/2; frame.linesize[2] = w/2;
    obs_source_output_video(m_source, &frame);
}
void ZoomVideoDelegate::onRawDataStatusChanged(ZOOM_SDK_NAMESPACE::RawDataStatus status)
{
    m_active.store(status == ZOOM_SDK_NAMESPACE::RawData_On, std::memory_order_relaxed);
    blog(LOG_INFO, "[obs-zoom-plugin] Video raw data %s", m_active ? "active" : "inactive");
}
void ZoomVideoDelegate::onRendererBeDestroyed()
{
    m_active.store(false, std::memory_order_relaxed);
    blog(LOG_INFO, "[obs-zoom-plugin] Video renderer destroyed");
}
