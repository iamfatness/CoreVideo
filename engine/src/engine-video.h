#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>
#include "../../src/engine-ipc.h"
#include "../../src/shm-generation.h"
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
                            IpcFd e2p_fd,
                            uint32_t resolution);
    ~ParticipantSubscription();

    uint32_t participant_id() const { return m_participant_id; }
    uint32_t resolution() const { return m_resolution; }
    bool active() const { return m_renderer != nullptr; }
    // Change the raw-data resolution on the LIVE renderer, in place. Avoids
    // destroy+recreate of the renderer (which races the SDK's async release
    // of the participant and returns SDKERR_WRONG_USAGE on the recreate —
    // the "switching to Active Speaker kills a mapped source" failure).
    bool set_resolution(uint32_t resolution);
    size_t target_count() const;
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
        // The generation the CURRENT region was created under; sent with every
        // frame event so the plugin can detect an orphaned mapping. This is a
        // record of what was published, NOT the counter — the counter must
        // survive this struct being destroyed on a re-subscribe and lives in
        // the process-wide table (src/shm-generation.h).
        uint32_t shm_gen = 0;
        // True after an ensure_shm() failure has been surfaced as an error —
        // avoids re-emitting once per frame while the failure persists.
        bool shm_fail_reported = false;
    };

    bool ensure_shm(SourceTarget &target,
                    const std::string &source_uuid,
                    size_t y_len);

    uint32_t    m_participant_id;
    uint32_t    m_resolution = 1;
    ZOOMSDK::IZoomSDKRenderer *m_renderer = nullptr;
    mutable std::mutex m_targets_mtx;
    std::unordered_map<std::string, std::unique_ptr<SourceTarget>> m_targets;
};

class EngineVideo {
public:
    void subscribe(uint32_t participant_id,
                   const std::string &source_uuid,
                   IpcFd e2p_fd,
                   uint32_t resolution);
    void set_raw_media_active(bool active);
    void unsubscribe(const std::string &source_uuid);
    void resubscribe_all();
    void unsubscribe_all();

private:
    void unsubscribe_locked(const std::string &source_uuid);

    std::unordered_map<uint32_t,
                       std::unique_ptr<ParticipantSubscription>> m_subs;
    struct SourceBinding {
        uint32_t participant_id = 0;
        uint32_t resolution = 1;
        IpcFd e2p_fd = kIpcInvalidFd;
    };
    std::unordered_map<std::string, SourceBinding> m_source_participants;
    bool m_raw_media_active = false;
};
