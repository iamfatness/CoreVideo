#pragma once
#include <obs-module.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <zoom_sdk_raw_data_def.h>
#include <rawdata/rawdata_renderer_interface.h>

enum class VideoResolution { P360 = 0, P720 = 1, P1080 = 2 };

class ZoomVideoDelegate : public ZOOMSDK::IZoomSDKRendererDelegate {
public:
    explicit ZoomVideoDelegate(obs_source_t *source);
    ~ZoomVideoDelegate() override;

    bool subscribe(uint32_t participant_id, VideoResolution res = VideoResolution::P1080);
    void unsubscribe();
    bool     is_active() const { return m_active.load(std::memory_order_relaxed); }
    uint32_t width()     const { return m_width.load(std::memory_order_relaxed); }
    uint32_t height()    const { return m_height.load(std::memory_order_relaxed); }

    // IZoomSDKRendererDelegate
    void onRawDataFrameReceived(YUVRawDataI420 *data) override;
    void onRawDataStatusChanged(RawDataStatus status) override;
    void onRendererBeDestroyed() override;

private:
    obs_source_t              *m_source;
    // m_renderer_mtx guards m_renderer against concurrent unsubscribe() and
    // SDK-initiated onRendererBeDestroyed() firing from different threads.
    std::mutex                 m_renderer_mtx;
    ZOOMSDK::IZoomSDKRenderer *m_renderer = nullptr;
    std::atomic<bool>          m_active{false};
    std::atomic<uint32_t>      m_width{0};
    std::atomic<uint32_t>      m_height{0};
};
