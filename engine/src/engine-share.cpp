#include "engine-share.h"
#include "engine-writer.h"

#include <algorithm>
#include <atomic>
#include <cstring>

static bool valid_i420_share_frame(YUVRawDataI420 *data,
                                   uint32_t w,
                                   uint32_t h,
                                   size_t &y_len)
{
    if (w == 0 || h == 0) return false;
    if (w > 8192 || h > 8192) return false;
    if ((w & 1) != 0 || (h & 1) != 0) return false;
    if (!data->GetYBuffer() || !data->GetUBuffer() || !data->GetVBuffer())
        return false;

    const uint64_t pixels = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
    constexpr uint64_t max_reasonable_i420 = 8192ull * 8192ull;
    constexpr uint64_t max_size_t_value = static_cast<uint64_t>(~size_t{0});
    if (pixels > max_reasonable_i420 || pixels > max_size_t_value)
        return false;

    y_len = static_cast<size_t>(pixels);
    return true;
}

EngineShare::EngineShare(EngineShareRosterSink *roster_sink)
    : m_roster_sink(roster_sink)
{
}

EngineShare::~EngineShare()
{
    detach();
}

void EngineShare::attach(ZOOMSDK::IMeetingShareController *share_ctrl)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_share_ctrl == share_ctrl)
        return;
    if (m_share_ctrl)
        m_share_ctrl->SetEvent(nullptr);
    m_share_ctrl = share_ctrl;
    if (m_share_ctrl)
        m_share_ctrl->SetEvent(this);
    subscribe_active_share_locked("attach");
}

void EngineShare::detach()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    unsubscribe_renderer_locked();
    if (m_share_ctrl) {
        m_share_ctrl->SetEvent(nullptr);
        m_share_ctrl = nullptr;
    }
    set_active_share_user(0);
}

void EngineShare::set_raw_media_active(bool active)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_raw_media_active = active;
    if (active) {
        subscribe_active_share_locked("raw_media_ready");
    } else {
        unsubscribe_renderer_locked();
    }
}

void EngineShare::subscribe(const std::string &source_uuid, IpcFd e2p_fd)
{
    if (source_uuid.empty())
        return;
    std::lock_guard<std::mutex> lock(m_mtx);
    // Each share target backs an SHM region — enforce the shared cap
    // (kMaxShmSources in engine-ipc.h). Re-registering is always allowed.
    if (shm_source_over_cap(m_targets.size(),
                            m_targets.find(source_uuid) != m_targets.end())) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"share_subscribe_rejected_capacity","source_uuid":")" +
            source_uuid + R"(","limit":)" +
            std::to_string(kMaxShmSources) + "}");
        EngineIpc::write(
            R"({"cmd":"error","msg":"subscribe_rejected","reason":"shm_capacity","source_uuid":")" +
            source_uuid + R"(","limit":)" +
            std::to_string(kMaxShmSources) + "}");
        return;
    }
    const auto [it, inserted] = m_targets.emplace(source_uuid, nullptr);
    if (inserted)
        it->second = std::make_unique<ShareTarget>(e2p_fd);
    else if (it->second)
        it->second->e2p_fd = e2p_fd;

    EngineIpc::write(
        R"({"cmd":"debug","stage":"share_source_registered","source_uuid":")" +
        source_uuid + R"(","active_share_source_id":)" +
        std::to_string(m_current_share_source_id) +
        R"(,"raw_media_active":)" +
        std::string(m_raw_media_active ? "true" : "false") + "}");
    subscribe_active_share_locked("source_registered");
}

void EngineShare::unsubscribe(const std::string &source_uuid)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    auto it = m_targets.find(source_uuid);
    if (it == m_targets.end())
        return;
    if (it->second)
        shm_region_destroy(it->second->shm);
    m_targets.erase(it);
    if (m_targets.empty())
        unsubscribe_renderer_locked();
}

void EngineShare::unsubscribe_all()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    unsubscribe_renderer_locked();
    clear_target_shm_locked();
    m_targets.clear();
}

void EngineShare::resubscribe_all()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    subscribe_active_share_locked("resubscribe_all");
}

uint32_t EngineShare::active_share_source_id(uint32_t *share_user_id) const
{
    if (share_user_id)
        *share_user_id = 0;
    if (!m_share_ctrl)
        return 0;
    auto *sharers = m_share_ctrl->GetViewableSharingUserList();
    if (!sharers)
        return 0;
    for (int i = 0; i < sharers->GetCount(); ++i) {
        const uint32_t user_id = sharers->GetItem(i);
        auto *sources = m_share_ctrl->GetSharingSourceInfoList(user_id);
        if (!sources)
            continue;
        for (int j = 0; j < sources->GetCount(); ++j) {
            const auto info = sources->GetItem(j);
            if (info.shareSourceID != 0) {
                if (share_user_id)
                    *share_user_id = user_id;
                return info.shareSourceID;
            }
        }
    }
    return 0;
}

void EngineShare::subscribe_active_share_locked(const char *reason)
{
    if (!m_raw_media_active || m_targets.empty()) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"share_subscribe_deferred","reason":")" +
            std::string(reason ? reason : "unknown") +
            R"(","raw_media_active":)" +
            std::string(m_raw_media_active ? "true" : "false") +
            R"(,"target_count":)" + std::to_string(m_targets.size()) + "}");
        return;
    }

    uint32_t share_user_id = 0;
    const uint32_t share_source_id = active_share_source_id(&share_user_id);
    if (share_source_id == 0) {
        unsubscribe_renderer_locked();
        set_active_share_user(0);
        EngineIpc::write(
            R"({"cmd":"debug","stage":"share_unavailable","reason":")" +
            std::string(reason ? reason : "unknown") + "\"}");
        return;
    }

    set_active_share_user(share_user_id);
    subscribe_to_locked(share_source_id, reason);
}

bool EngineShare::subscribe_to_locked(uint32_t share_source_id, const char *reason)
{
    if (share_source_id == 0)
        return false;
    if (m_renderer && m_current_share_source_id == share_source_id)
        return true;

    unsubscribe_renderer_locked();
    ZOOMSDK::IZoomSDKRenderer *renderer = nullptr;
    ZOOMSDK::SDKError err = ZOOMSDK::createRenderer(&renderer, this);
    if (err != ZOOMSDK::SDKERR_SUCCESS || !renderer) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"share_create_renderer_failed","code":)" +
            std::to_string(static_cast<int>(err)) +
            R"(,"share_source_id":)" + std::to_string(share_source_id) + "}");
        return false;
    }

    const ZOOMSDK::SDKError res_err =
        renderer->setRawDataResolution(ZOOMSDK::ZoomSDKResolution_1080P);
    EngineIpc::write(
        R"({"cmd":"debug","stage":"share_set_resolution","code":)" +
        std::to_string(static_cast<int>(res_err)) +
        R"(,"share_source_id":)" + std::to_string(share_source_id) + "}");

    err = renderer->subscribe(share_source_id, ZOOMSDK::RAW_DATA_TYPE_SHARE);
    EngineIpc::write(
        R"({"cmd":"debug","stage":"share_subscribe","code":)" +
        std::to_string(static_cast<int>(err)) +
        R"(,"share_source_id":)" + std::to_string(share_source_id) +
        R"(,"reason":")" + std::string(reason ? reason : "unknown") + "\"}");
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        ZOOMSDK::destroyRenderer(renderer);
        return false;
    }

    m_renderer = renderer;
    m_current_share_source_id = share_source_id;
    return true;
}

void EngineShare::unsubscribe_renderer_locked()
{
    ZOOMSDK::IZoomSDKRenderer *renderer = m_renderer;
    m_renderer = nullptr;
    m_current_share_source_id = 0;
    if (!renderer)
        return;
    renderer->unSubscribe();
    ZOOMSDK::destroyRenderer(renderer);
}

void EngineShare::clear_target_shm_locked()
{
    for (auto &entry : m_targets) {
        if (entry.second)
            shm_region_destroy(entry.second->shm);
    }
}

bool EngineShare::ensure_shm(ShareTarget &target,
                             const std::string &source_uuid,
                             size_t y_len)
{
    const size_t total =
        sizeof(ShmFrameHeader) + y_len + y_len / 4 + y_len / 4;
    if (total < y_len) return false;
    if (target.shm.ptr && target.shm.size >= total) return true;

    // Bump the generation BEFORE creating and bake it into the name: a
    // resize under the old name deadlocks while the plugin still maps the
    // old (smaller) section (see shm_region_name in engine-ipc.h).
    const uint32_t next_gen = target.shm_gen + 1;
    const std::string region_name =
        shm_region_name(IPC_SHM_PREFIX + source_uuid, next_gen);
    if (!shm_region_create(target.shm, region_name, total)) return false;
    target.shm_gen = next_gen; // old plugin-side mappings are now stale
    return true;
}

void EngineShare::set_active_share_user(uint32_t user_id)
{
    if (m_current_share_user_id == user_id)
        return;
    m_current_share_user_id = user_id;
    if (m_roster_sink)
        m_roster_sink->set_active_share_user(user_id);
}

void EngineShare::onRendererBeDestroyed()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_renderer = nullptr;
    m_current_share_source_id = 0;
}

void EngineShare::onRawDataFrameReceived(YUVRawDataI420 *data)
{
    if (!data)
        return;
    const uint32_t w = data->GetStreamWidth();
    const uint32_t h = data->GetStreamHeight();
    size_t y_len = 0;
    if (!valid_i420_share_frame(data, w, h, y_len)) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"share_frame_invalid","w":)" +
            std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
        return;
    }

    const uint32_t raw_share_source_id = data->GetSourceID();

    std::lock_guard<std::mutex> lock(m_mtx);
    const uint32_t share_user_id = m_current_share_user_id;
    for (auto &entry : m_targets) {
        const std::string &source_uuid = entry.first;
        ShareTarget &target = *entry.second;
        if (!ensure_shm(target, source_uuid, y_len) || !target.shm.ptr) {
            // Surface once per failure episode (not per frame) as a real
            // error so the plugin can show the operator that this source's
            // frames are being dropped.
            if (!target.shm_fail_reported) {
                target.shm_fail_reported = true;
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"share_shm_create_failed","last_error":)" +
                    std::to_string(target.shm.last_error) +
                    R"(,"source_uuid":")" +
                    source_uuid + R"(","w":)" + std::to_string(w) +
                    R"(,"h":)" + std::to_string(h) + "}");
                EngineIpc::write(
                    R"({"cmd":"error","msg":"shm_create_failed","source_uuid":")" +
                    source_uuid + R"(","w":)" + std::to_string(w) +
                    R"(,"h":)" + std::to_string(h) + "}");
            }
            continue;
        }
        if (target.shm_fail_reported) {
            target.shm_fail_reported = false;
            EngineIpc::write(
                R"({"cmd":"debug","stage":"share_shm_recovered","source_uuid":")" +
                source_uuid + "\"}");
        }

        auto *hdr = static_cast<ShmFrameHeader *>(target.shm.ptr);
        auto *pixels = static_cast<char *>(target.shm.ptr) + sizeof(ShmFrameHeader);
        uint32_t seq = hdr->sequence + 1;
        if ((seq & 1u) == 0) ++seq;
        hdr->sequence = seq;
        std::atomic_thread_fence(std::memory_order_release);
        hdr->width = w;
        hdr->height = h;
        hdr->y_len = static_cast<uint32_t>(y_len);

        std::memcpy(pixels, data->GetYBuffer(), y_len);
        std::memcpy(pixels + y_len, data->GetUBuffer(), y_len / 4);
        std::memcpy(pixels + y_len + y_len / 4, data->GetVBuffer(), y_len / 4);
        std::atomic_thread_fence(std::memory_order_release);
        hdr->sequence = seq + 1;

        ++target.frame_count;
        if (target.frame_count == 1 || target.frame_count % 120 == 0) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"share_frame_received","source_uuid":")" +
                source_uuid + R"(","share_source_id":)" +
                std::to_string(raw_share_source_id != 0
                    ? raw_share_source_id
                    : m_current_share_source_id) +
                R"(,"share_user_id":)" + std::to_string(share_user_id) +
                R"(,"count":)" + std::to_string(target.frame_count) +
                R"(,"w":)" + std::to_string(w) +
                R"(,"h":)" + std::to_string(h) + "}");
        }

        EngineIpc::write(
            R"({"cmd":"frame","source_uuid":")" + source_uuid +
            R"(","participant_id":)" + std::to_string(share_user_id) +
            R"(,"w":)" + std::to_string(w) +
            R"(,"h":)" + std::to_string(h) +
            R"(,"shm_gen":)" + std::to_string(target.shm_gen) + "}");
    }
}

void EngineShare::onRawDataStatusChanged(RawDataStatus status)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    for (const auto &entry : m_targets) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"share_raw_status","source_uuid":")" +
            entry.first + R"(","status":)" +
            std::to_string(static_cast<int>(status)) + "}");
    }
}

void EngineShare::onSharingStatus(ZOOMSDK::ZoomSDKSharingSourceInfo shareInfo)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    EngineIpc::write(
        R"({"cmd":"debug","stage":"share_status","user_id":)" +
        std::to_string(shareInfo.userid) +
        R"(,"share_source_id":)" + std::to_string(shareInfo.shareSourceID) +
        R"(,"status":)" + std::to_string(static_cast<int>(shareInfo.status)) +
        "}");

    switch (shareInfo.status) {
    case ZOOMSDK::Sharing_Other_Share_Begin:
    case ZOOMSDK::Sharing_View_Other_Sharing:
        if (shareInfo.userid != 0)
            set_active_share_user(shareInfo.userid);
        if (shareInfo.shareSourceID != 0)
            subscribe_to_locked(shareInfo.shareSourceID, "share_status");
        else
            subscribe_active_share_locked("share_status");
        break;
    case ZOOMSDK::Sharing_Other_Share_End:
        if (shareInfo.shareSourceID == 0 ||
            shareInfo.shareSourceID == m_current_share_source_id) {
            unsubscribe_renderer_locked();
            set_active_share_user(0);
            subscribe_active_share_locked("share_end");
        }
        break;
    default:
        break;
    }
}

void EngineShare::onShareContentNotification(
    ZOOMSDK::ZoomSDKSharingSourceInfo shareInfo)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (shareInfo.userid != 0)
        set_active_share_user(shareInfo.userid);
    if (shareInfo.shareSourceID != 0 &&
        shareInfo.shareSourceID != m_current_share_source_id) {
        subscribe_to_locked(shareInfo.shareSourceID, "share_content");
    }
}
