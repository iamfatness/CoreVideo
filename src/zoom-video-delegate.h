#pragma once
#include <obs-module.h>
#include <cstdint>
#include <atomic>
#ifndef ZOOM_SDK_NAMESPACE
namespace ZOOM_SDK_NAMESPACE {
struct YUVRawDataI420 {
    virtual char     *GetYBuffer()      = 0;
    virtual char     *GetUBuffer()      = 0;
    virtual char     *GetVBuffer()      = 0;
    virtual uint32_t  GetStreamWidth()  = 0;
    virtual uint32_t  GetStreamHeight() = 0;
    virtual uint32_t  GetBufferLen()    = 0;
    virtual ~YUVRawDataI420() = default;
};
enum RawDataStatus { RawData_On, RawData_Off };
struct IZoomSDKRendererDelegate {
    virtual void onRawDataFrameReceived(YUVRawDataI420 *)  = 0;
    virtual void onRawDataStatusChanged(RawDataStatus)     = 0;
    virtual void onRendererBeDestroyed()                   = 0;
    virtual ~IZoomSDKRendererDelegate() = default;
};
} // namespace ZOOM_SDK_NAMESPACE
#endif
class ZoomVideoDelegate : public ZOOM_SDK_NAMESPACE::IZoomSDKRendererDelegate {
public:
    explicit ZoomVideoDelegate(obs_source_t *source);
    ~ZoomVideoDelegate() override = default;
    void onRawDataFrameReceived(ZOOM_SDK_NAMESPACE::YUVRawDataI420 *data) override;
    void onRawDataStatusChanged(ZOOM_SDK_NAMESPACE::RawDataStatus status) override;
    void onRendererBeDestroyed() override;
    bool is_active() const { return m_active.load(std::memory_order_relaxed); }
private:
    obs_source_t     *m_source;
    std::atomic<bool> m_active{false};
};
