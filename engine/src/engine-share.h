#pragma once

#include "../../src/engine-ipc.h"
#include "../../src/shm-generation.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <meeting_service_interface.h>
#if __has_include(<meeting_service_components/meeting_sharing_interface.h>)
#include <meeting_service_components/meeting_sharing_interface.h>
#else
#include <meeting_sharing_interface.h>
#endif
#if __has_include(<rawdata/zoom_rawdata_api.h>)
#include <rawdata/zoom_rawdata_api.h>
#else
#include <zoom_rawdata_api.h>
#endif
#if __has_include(<rawdata/rawdata_renderer_interface.h>)
#include <rawdata/rawdata_renderer_interface.h>
#else
#include <rawdata_renderer_interface.h>
#endif
#if __has_include(<zoom_sdk_raw_data_def.h>)
#include <zoom_sdk_raw_data_def.h>
#else
#include <rawdata_def.h>
#endif

class EngineShareRosterSink {
public:
    virtual ~EngineShareRosterSink() = default;
    virtual void set_active_share_user(uint32_t user_id) = 0;
};

class EngineShare : public ZOOMSDK::IMeetingShareCtrlEvent,
                    public ZOOMSDK::IZoomSDKRendererDelegate {
public:
    explicit EngineShare(EngineShareRosterSink *roster_sink = nullptr);
    ~EngineShare();

    void attach(ZOOMSDK::IMeetingShareController *share_ctrl);
    void detach();
    void set_raw_media_active(bool active);
    void subscribe(const std::string &source_uuid, IpcFd e2p_fd);
    void unsubscribe(const std::string &source_uuid);
    void unsubscribe_all();
    void resubscribe_all();

    void onRendererBeDestroyed() override;
    void onRawDataFrameReceived(YUVRawDataI420 *data) override;
    void onRawDataStatusChanged(RawDataStatus status) override;

    void onSharingStatus(ZOOMSDK::ZoomSDKSharingSourceInfo shareInfo) override;
    void onFailedToStartShare() override {}
    void onLockShareStatus(bool) override {}
    void onShareContentNotification(ZOOMSDK::ZoomSDKSharingSourceInfo shareInfo) override;
    void onMultiShareSwitchToSingleShareNeedConfirm(
        ZOOMSDK::IShareSwitchMultiToSingleConfirmHandler *) override {}
    void onShareSettingTypeChangedNotification(ZOOMSDK::ShareSettingType) override {}
    void onSharedVideoEnded() override {}
    void onVideoFileSharePlayError(ZOOMSDK::ZoomSDKVideoFileSharePlayError) override {}
    void onOptimizingShareForVideoClipStatusChanged(
        ZOOMSDK::ZoomSDKSharingSourceInfo) override {}

private:
    struct ShareTarget {
        explicit ShareTarget(IpcFd e2p) : e2p_fd(e2p) {}
        IpcFd e2p_fd;
        ShmRegion shm;
        uint64_t frame_count = 0;
        // The generation the CURRENT region was created under; sent with every
        // frame event so the plugin can detect an orphaned mapping. A record of
        // what was published, NOT the counter — the counter survives this
        // struct in the process-wide table (src/shm-generation.h).
        uint32_t shm_gen = 0;
        // True after an ensure_shm() failure has been surfaced as an error —
        // avoids re-emitting once per frame while the failure persists.
        bool shm_fail_reported = false;
    };

    void deliver_frame(YUVRawDataI420 *data);
    // `_locked` = caller holds m_lifecycle_mtx.
    uint32_t active_share_source_id(uint32_t *user_id) const;
    void subscribe_active_share_locked(const char *reason);
    bool subscribe_to_locked(uint32_t share_source_id, const char *reason);
    void unsubscribe_renderer_locked();
    size_t target_count() const;
    bool ensure_shm(ShareTarget &target,
                    const std::string &source_uuid,
                    size_t y_len);
    void set_active_share_user(uint32_t user_id);

    // Two locks, and the split is the point. The Zoom SDK calls the renderer
    // delegate (onRawDataStatusChanged, onRendererBeDestroyed) synchronously
    // from inside subscribe()/unSubscribe()/destroyRenderer(), on the calling
    // thread. When one mutex guarded both the SDK calls and the callbacks,
    // that callback relocked a mutex its own thread held; MSVC's std::mutex
    // threw resource_deadlock_would_occur, nothing caught it, and the engine
    // aborted mid-share (field crash 2026-10-02, six local dumps Aug 9-25).
    //
    // m_lifecycle_mtx: renderer/controller state and every SDK call that
    //   changes it. Recursive because SDK share events can arrive re-entrantly
    //   while one of those calls is in progress. Renderer callbacks never take it.
    // m_targets_mtx: the per-source SHM targets. Taken by the frame and status
    //   callbacks; NEVER held across an SDK call. Order: lifecycle, then targets.
    EngineShareRosterSink *m_roster_sink = nullptr;
    ZOOMSDK::IMeetingShareController *m_share_ctrl = nullptr;
    std::atomic<ZOOMSDK::IZoomSDKRenderer *> m_renderer{nullptr};
    mutable std::recursive_mutex m_lifecycle_mtx;
    mutable std::mutex m_targets_mtx;
    std::unordered_map<std::string, std::unique_ptr<ShareTarget>> m_targets;
    std::atomic<uint32_t> m_current_share_source_id{0};
    std::atomic<uint32_t> m_current_share_user_id{0};
    bool m_raw_media_active = false;
};
