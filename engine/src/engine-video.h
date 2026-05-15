#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
#include "../../src/engine-ipc.h"
#if __has_include(<zoom_sdk_raw_data_def.h>)
#include <zoom_sdk_raw_data_def.h>
#else
#include <rawdata_def.h>
#endif
#if __has_include(<rawdata/rawdata_renderer_interface.h>)
#include <rawdata/rawdata_renderer_interface.h>
#else
#include <rawdata_renderer_interface.h>
#endif

class ParticipantSubscription : public ZOOMSDK::IZoomSDKRendererDelegate {
public:
    ParticipantSubscription(uint32_t participant_id,
                            const std::string &source_uuid,
                            IpcFd e2p_fd);
    ~ParticipantSubscription();

    uint32_t participant_id() const { return m_participant_id; }
    IpcFd ipc_fd() const { return m_e2p_fd; }

    void onRawDataFrameReceived(YUVRawDataI420 *data) override;
    void onRawDataStatusChanged(ZOOMSDK::IZoomSDKRendererDelegate::RawDataStatus status) override;
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
    void resubscribe_all();

private:
    std::unordered_map<std::string,
                       std::unique_ptr<ParticipantSubscription>> m_subs;
};
