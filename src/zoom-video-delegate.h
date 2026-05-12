#pragma once
#include <obs-module.h>
#include <cstdint>
#include <atomic>
#include "../third_party/zoom-sdk/h/rawdata_def.h"
#include "../third_party/zoom-sdk/h/rawdata_renderer_interface.h"

class ZoomVideoDelegate : public ZOOMSDK::IZoomSDKRendererDelegate {
public:
    explicit ZoomVideoDelegate(obs_source_t *source);
    ~ZoomVideoDelegate() override;

    bool subscribe(uint32_t participant_id);
    void unsubscribe();
    bool is_active() const { return m_active.load(std::memory_order_relaxed); }

    // IZoomSDKRendererDelegate
    void onRawDataFrameReceived(YUVRawDataI420 *data) override;
    void onRawDataStatusChanged(RawDataStatus status) override;
    void onRendererBeDestroyed() override;

private:
    obs_source_t              *m_source;
    ZOOMSDK::IZoomSDKRenderer *m_renderer = nullptr;
    std::atomic<bool>          m_active{false};
};
