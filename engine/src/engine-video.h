#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
#include "../../src/engine-ipc.h"
#include "../../third_party/zoom-sdk/h/rawdata_def.h"
#include "../../third_party/zoom-sdk/h/rawdata_renderer_interface.h"

#pragma pack(push, 1)
struct ShmFrameHeader { uint32_t width, height, y_len; };
#pragma pack(pop)

class ParticipantSubscription : public ZOOMSDK::IZoomSDKRendererDelegate {
public:
    ParticipantSubscription(uint32_t participant_id,
                            const std::string &source_uuid,
                            IpcFd e2p_fd);
    ~ParticipantSubscription();

    void onRawDataFrameReceived(YUVRawDataI420 *data) override;
    void onRawDataStatusChanged(RawDataStatus status) override;
    void onRendererBeDestroyed() override;

private:
    void ensure_shm(uint32_t y_len);

    uint32_t    m_participant_id;
    std::string m_source_uuid;
    IpcFd       m_e2p_fd;
    ZOOMSDK::IZoomSDKRenderer *m_renderer = nullptr;
    ShmRegion   m_shm;
};

class EngineVideo {
public:
    void subscribe(uint32_t participant_id,
                   const std::string &source_uuid,
                   IpcFd e2p_fd);
    void unsubscribe(const std::string &source_uuid);

private:
    std::unordered_map<std::string,
                       std::unique_ptr<ParticipantSubscription>> m_subs;
};
