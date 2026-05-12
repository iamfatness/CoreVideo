#pragma once
#include <cstdint>
#include <obs-module.h>
#include <zoom_sdk_raw_data_def.h>
#include <rawdata/rawdata_renderer_interface.h>
#include <meeting_service_components/meeting_sharing_interface.h>
#include <rawdata/zoom_rawdata_api.h>

class ZoomShareDelegate : public ZOOMSDK::IZoomSDKRendererDelegate,
                          public ZOOMSDK::IMeetingShareCtrlEvent {
public:
    static ZoomShareDelegate &instance();

    void attach(ZOOMSDK::IMeetingShareController *ctrl);
    void detach();

    void set_obs_source(obs_source_t *source) { m_source = source; }
    obs_source_t *obs_source() const          { return m_source; }

    // IZoomSDKRendererDelegate
    void onRawDataFrameReceived(YUVRawDataI420 *data) override;
    void onRawDataStatusChanged(RawDataStatus status) override;
    void onRendererBeDestroyed() override;

    // IMeetingShareCtrlEvent
    void onSharingStatus(ZOOMSDK::ZoomSDKSharingSourceInfo shareInfo) override;
    void onFailedToStartShare() override;
    void onLockShareStatus(bool bLocked) override;
    void onShareContentNotification(ZOOMSDK::ZoomSDKSharingSourceInfo shareInfo) override;
    void onMultiShareSwitchToSingleShareNeedConfirm(
        ZOOMSDK::IShareSwitchMultiToSingleConfirmHandler *handler_) override;
    void onShareSettingTypeChangedNotification(ZOOMSDK::ShareSettingType type) override;
    void onSharedVideoEnded() override;
    void onVideoFileSharePlayError(ZOOMSDK::ZoomSDKVideoFileSharePlayError error) override;
    void onOptimizingShareForVideoClipStatusChanged(
        ZOOMSDK::ZoomSDKSharingSourceInfo shareInfo) override;

private:
    ZoomShareDelegate() = default;
    void subscribe_to(uint32_t share_source_id);
    void unsubscribe_renderer();

    obs_source_t                    *m_source     = nullptr;
    ZOOMSDK::IZoomSDKRenderer       *m_renderer   = nullptr;
    ZOOMSDK::IMeetingShareController *m_share_ctrl = nullptr;
    uint32_t                         m_current_source_id = 0;
};

void zoom_share_source_register();
