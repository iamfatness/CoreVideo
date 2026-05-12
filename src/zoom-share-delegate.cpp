#include "zoom-share-delegate.h"
#include <obs-module.h>
#include <media-io/video-io.h>
#include <util/platform.h>

ZoomShareDelegate &ZoomShareDelegate::instance()
{
    static ZoomShareDelegate inst;
    return inst;
}

void ZoomShareDelegate::attach(ZOOMSDK::IMeetingShareController *ctrl)
{
    m_share_ctrl = ctrl;
    if (!ctrl) return;
    ctrl->SetEvent(this);

    // Subscribe to any share that is already active when we join
    auto *sharers = ctrl->GetViewableSharingUserList();
    if (!sharers) return;
    for (int i = 0; i < sharers->GetCount(); ++i) {
        auto *src_list = ctrl->GetSharingSourceInfoList(sharers->GetItem(i));
        if (!src_list) continue;
        for (int j = 0; j < src_list->GetCount(); ++j) {
            auto info = src_list->GetItem(j);
            if (info.shareSourceID != 0) {
                subscribe_to(info.shareSourceID);
                return;
            }
        }
    }
}

void ZoomShareDelegate::detach()
{
    unsubscribe_renderer();
    if (m_share_ctrl) {
        m_share_ctrl->SetEvent(nullptr);
        m_share_ctrl = nullptr;
    }
}

void ZoomShareDelegate::subscribe_to(uint32_t share_source_id)
{
    if (m_current_source_id == share_source_id) return;
    unsubscribe_renderer();

    ZOOMSDK::SDKError err = ZOOMSDK::createRenderer(&m_renderer, this);
    if (err != ZOOMSDK::SDKERR_SUCCESS || !m_renderer) return;

    m_renderer->setRawDataResolution(ZOOMSDK::ZoomSDKResolution_1080P);
    err = m_renderer->subscribe(share_source_id, ZOOMSDK::RAW_DATA_TYPE_SHARE);
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        ZOOMSDK::destroyRenderer(m_renderer);
        m_renderer = nullptr;
        return;
    }
    m_current_source_id = share_source_id;
    blog(LOG_INFO, "[obs-zoom-plugin] Share subscribed (source_id=%u)", share_source_id);
}

void ZoomShareDelegate::unsubscribe_renderer()
{
    if (!m_renderer) return;
    m_renderer->unSubscribe();
    ZOOMSDK::destroyRenderer(m_renderer);
    m_renderer = nullptr;
    m_current_source_id = 0;
}

// ── IZoomSDKRendererDelegate ─────────────────────────────────────────────────

void ZoomShareDelegate::onRawDataFrameReceived(YUVRawDataI420 *data)
{
    if (!data || !m_source) return;
    const uint32_t w = data->GetStreamWidth();
    const uint32_t h = data->GetStreamHeight();
    if (w == 0 || h == 0) return;

    obs_source_frame frame = {};
    frame.format      = VIDEO_FORMAT_I420;
    frame.width       = w;
    frame.height      = h;
    frame.timestamp   = os_gettime_ns();
    frame.data[0]     = reinterpret_cast<uint8_t *>(data->GetYBuffer());
    frame.data[1]     = reinterpret_cast<uint8_t *>(data->GetUBuffer());
    frame.data[2]     = reinterpret_cast<uint8_t *>(data->GetVBuffer());
    frame.linesize[0] = w;
    frame.linesize[1] = w / 2;
    frame.linesize[2] = w / 2;
    obs_source_output_video(m_source, &frame);
}

void ZoomShareDelegate::onRawDataStatusChanged(RawDataStatus status)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Share raw data %s",
         status == RawData_On ? "active" : "inactive");
}

void ZoomShareDelegate::onRendererBeDestroyed()
{
    m_renderer = nullptr;
    m_current_source_id = 0;
}

// ── IMeetingShareCtrlEvent ───────────────────────────────────────────────────

void ZoomShareDelegate::onSharingStatus(ZOOMSDK::ZoomSDKSharingSourceInfo shareInfo)
{
    switch (shareInfo.status) {
    case ZOOMSDK::Sharing_Other_Share_Begin:
    case ZOOMSDK::Sharing_View_Other_Sharing:
        if (shareInfo.shareSourceID != 0)
            subscribe_to(shareInfo.shareSourceID);
        break;
    case ZOOMSDK::Sharing_Other_Share_End:
        if (shareInfo.shareSourceID == m_current_source_id)
            unsubscribe_renderer();
        break;
    default:
        break;
    }
}

void ZoomShareDelegate::onShareContentNotification(ZOOMSDK::ZoomSDKSharingSourceInfo shareInfo)
{
    // Pick up a new share that started while we were already subscribed to another
    if (shareInfo.shareSourceID == 0 || shareInfo.shareSourceID == m_current_source_id)
        return;
    if (shareInfo.status == ZOOMSDK::Sharing_Other_Share_Begin ||
        shareInfo.status == ZOOMSDK::Sharing_View_Other_Sharing)
        subscribe_to(shareInfo.shareSourceID);
}

void ZoomShareDelegate::onFailedToStartShare() {}
void ZoomShareDelegate::onLockShareStatus(bool) {}
void ZoomShareDelegate::onMultiShareSwitchToSingleShareNeedConfirm(
    ZOOMSDK::IShareSwitchMultiToSingleConfirmHandler *) {}
void ZoomShareDelegate::onShareSettingTypeChangedNotification(ZOOMSDK::ShareSettingType) {}
void ZoomShareDelegate::onSharedVideoEnded() {}
void ZoomShareDelegate::onVideoFileSharePlayError(ZOOMSDK::ZoomSDKVideoFileSharePlayError) {}
void ZoomShareDelegate::onOptimizingShareForVideoClipStatusChanged(
    ZOOMSDK::ZoomSDKSharingSourceInfo) {}

// ── OBS source registration ──────────────────────────────────────────────────

static const char *share_source_name(void *) { return "Zoom Screen Share"; }

static void *share_source_create(obs_data_t *, obs_source_t *source)
{
    ZoomShareDelegate::instance().set_obs_source(source);
    return source;
}

static void share_source_destroy(void *)
{
    ZoomShareDelegate::instance().set_obs_source(nullptr);
}

static uint32_t share_source_width(void *)  { return 0; }
static uint32_t share_source_height(void *) { return 0; }

void zoom_share_source_register()
{
    obs_source_info info = {};
    info.id           = "zoom_share_source";
    info.type         = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name     = share_source_name;
    info.create       = share_source_create;
    info.destroy      = share_source_destroy;
    info.get_width    = share_source_width;
    info.get_height   = share_source_height;
    obs_register_source(&info);
}
