#pragma once
#include <obs-module.h>
#include <cstdint>
#include <atomic>
#include <zoom_sdk_raw_data_def.h>
#include <rawdata/rawdata_renderer_interface.h>

class ZoomVideoDelegate : public ZOOMSDK::IZoomSDKRendererDelegate {
public:
    explicit ZoomVideoDelegate(obs_source_t *source);
    ~ZoomVideoDelegate() override;

    bool subscribe(uint32_t participant_id);
    void unsubscribe();
    bool is_active() const { return m_active.load(std::memory_order_relaxed); }
    uint64_t frame_count() const { return m_frame_count.load(std::memory_order_relaxed); }
    uint64_t last_frame_ns() const { return m_last_frame_ns.load(std::memory_order_relaxed); }
    uint32_t width() const { return m_width.load(std::memory_order_relaxed); }
    uint32_t height() const { return m_height.load(std::memory_order_relaxed); }

    // IZoomSDKRendererDelegate
    void onRawDataFrameReceived(YUVRawDataI420 *data) override;
    void onRawDataStatusChanged(RawDataStatus status) override;
    void onRendererBeDestroyed() override;

private:
    obs_source_t              *m_source;
    ZOOMSDK::IZoomSDKRenderer *m_renderer = nullptr;
    std::atomic<bool>          m_active{false};
    std::atomic<uint64_t>      m_frame_count{0};
    std::atomic<uint64_t>      m_last_frame_ns{0};
    std::atomic<uint32_t>      m_width{0};
    std::atomic<uint32_t>      m_height{0};
};
