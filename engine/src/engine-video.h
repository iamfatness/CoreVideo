#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
#include <windows.h>
#include "../../third_party/zoom-sdk/h/rawdata_def.h"
#include "../../third_party/zoom-sdk/h/rawdata_renderer_interface.h"

#pragma pack(push, 1)
struct ShmFrameHeader { uint32_t width, height, y_len; };
#pragma pack(pop)

class ParticipantSubscription : public ZOOMSDK::IZoomSDKRendererDelegate {
public:
    ParticipantSubscription(uint32_t participant_id,
                            const std::string &source_uuid,
                            HANDLE e2p_pipe);
    ~ParticipantSubscription();

    void onRawDataFrameReceived(YUVRawDataI420 *data) override;
    void onRawDataStatusChanged(RawDataStatus status) override;
    void onRendererBeDestroyed() override;

private:
    void ensure_shm(uint32_t y_len);

    uint32_t    m_participant_id;
    std::string m_source_uuid;
    HANDLE      m_e2p_pipe;
    ZOOMSDK::IZoomSDKRenderer *m_renderer = nullptr;
    HANDLE      m_shm_handle = nullptr;
    void       *m_shm_ptr    = nullptr;
    size_t      m_shm_size   = 0;
};

class EngineVideo {
public:
    void subscribe(uint32_t participant_id,
                   const std::string &source_uuid,
                   HANDLE e2p_pipe);
    void unsubscribe(const std::string &source_uuid);

private:
    std::unordered_map<std::string,
                       std::unique_ptr<ParticipantSubscription>> m_subs;
};
