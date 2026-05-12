#pragma once
#include <obs-module.h>
#include <cstdint>
#include <atomic>
#include <functional>
#include <mutex>
#include <zoom_sdk_raw_data_def.h>
#include <rawdata/rawdata_renderer_interface.h>

enum class VideoResolution { P360 = 0, P720 = 1, P1080 = 2 };
// What to show when the participant's video feed goes inactive.
// LastFrame: OBS holds the last decoded frame (default OBS behaviour).
// Black:     push a black I420 frame immediately on feed loss.
enum class VideoLossMode  { LastFrame = 0, Black = 1 };

class ZoomVideoDelegate : public ZOOMSDK::IZoomSDKRendererDelegate {
public:
    explicit ZoomVideoDelegate(obs_source_t *source);
    ~ZoomVideoDelegate() override;

    bool subscribe(uint32_t participant_id, VideoResolution res = VideoResolution::P1080);
    void unsubscribe();
    bool     is_active() const { return m_active.load(std::memory_order_relaxed); }
    uint32_t width()     const { return m_width.load(std::memory_order_relaxed); }
    uint32_t height()    const { return m_height.load(std::memory_order_relaxed); }

    void set_loss_mode(VideoLossMode mode) {
        m_loss_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
    }

    // Preview callback — called at most ~5 fps with a raw I420 frame.
    // Invoked on the SDK raw-data thread; the callback must be fast (post to UI thread).
    using PreviewCallback = std::function<void(uint32_t w, uint32_t h,
        const uint8_t *y, const uint8_t *u, const uint8_t *v,
        uint32_t stride_y, uint32_t stride_uv)>;
    void set_preview_cb(PreviewCallback cb);
    void clear_preview_cb();

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
    std::atomic<int>           m_loss_mode{static_cast<int>(VideoLossMode::LastFrame)};

    std::mutex                 m_preview_mtx;
    PreviewCallback            m_preview_cb;
    uint64_t                   m_preview_last_ns = 0;
    static constexpr uint64_t  kPreviewIntervalNs = 200'000'000ULL; // max 5 fps
};
