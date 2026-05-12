#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
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
#pragma pack(push, 1)
struct ShmFrameHeader { uint32_t width, height, y_len; };
#pragma pack(pop)
class ParticipantSubscription : public ZOOM_SDK_NAMESPACE::IZoomSDKRendererDelegate {
public:
    ParticipantSubscription(uint32_t participant_id, const std::string &source_uuid, HANDLE e2p_pipe);
    ~ParticipantSubscription();
    void onRawDataFrameReceived(ZOOM_SDK_NAMESPACE::YUVRawDataI420 *data) override;
    void onRawDataStatusChanged(ZOOM_SDK_NAMESPACE::RawDataStatus status) override;
    void onRendererBeDestroyed() override;
private:
    void ensure_shm(uint32_t y_len);
    uint32_t    m_participant_id;
    std::string m_source_uuid;
    HANDLE      m_e2p_pipe;
    HANDLE      m_shm_handle = nullptr;
    void       *m_shm_ptr    = nullptr;
    size_t      m_shm_size   = 0;
};
class EngineVideo {
public:
    void subscribe(uint32_t participant_id, const std::string &source_uuid, HANDLE e2p_pipe);
    void unsubscribe(const std::string &source_uuid);
private:
    std::unordered_map<std::string, std::unique_ptr<ParticipantSubscription>> m_subs;
};
