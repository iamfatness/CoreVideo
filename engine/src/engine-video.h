#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>
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
                            const std::string &initial_source_uuid,
                            IpcFd e2p_fd);
    ~ParticipantSubscription();

    uint32_t participant_id() const { return m_participant_id; }
    void add_source(const std::string &source_uuid, IpcFd e2p_fd);
    void remove_source(const std::string &source_uuid);
    bool empty() const;
    std::vector<std::pair<std::string, IpcFd>> sources() const;

    void onRawDataFrameReceived(YUVRawDataI420 *data) override;
    void onRawDataStatusChanged(ZOOMSDK::IZoomSDKRendererDelegate::RawDataStatus status) override;
    void onRendererBeDestroyed() override;

private:
    struct SourceTarget {
        explicit SourceTarget(IpcFd e2p) : e2p_fd(e2p) {}
        IpcFd e2p_fd;
        ShmRegion shm;
        uint64_t frame_count = 0;
    };

    bool ensure_shm(SourceTarget &target,
                    const std::string &source_uuid,
                    uint32_t y_len);

    uint32_t    m_participant_id;
    ZOOMSDK::IZoomSDKRenderer *m_renderer = nullptr;
    mutable std::mutex m_targets_mtx;
    std::unordered_map<std::string, std::unique_ptr<SourceTarget>> m_targets;
};

class EngineVideo {
public:
    void subscribe(uint32_t participant_id,
                   const std::string &source_uuid,
                   IpcFd e2p_fd);
    void unsubscribe(const std::string &source_uuid);
    void resubscribe_all();

private:
    void unsubscribe_locked(const std::string &source_uuid);

    std::unordered_map<uint32_t,
                       std::unique_ptr<ParticipantSubscription>> m_subs;
    std::unordered_map<std::string, uint32_t> m_source_participants;
};
