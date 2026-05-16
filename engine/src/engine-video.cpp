#include "engine-video.h"
#include "engine-writer.h"
#if __has_include(<rawdata/zoom_rawdata_api.h>)
#include <rawdata/zoom_rawdata_api.h>
#else
#include <zoom_rawdata_api.h>
#endif
#include <cstring>
#include <limits>
#include <tuple>
#include <vector>

static bool valid_i420_frame(YUVRawDataI420 *data, uint32_t w, uint32_t h, size_t &y_len)
{
    if (w == 0 || h == 0) return false;
    if (w > 8192 || h > 8192) return false;
    if ((w & 1) != 0 || (h & 1) != 0) return false;
    if (!data->GetYBuffer() || !data->GetUBuffer() || !data->GetVBuffer()) return false;

    const uint64_t pixels = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
    const uint64_t max_reasonable_i420 = 8192ull * 8192ull;
    constexpr uint64_t max_size_t_value = static_cast<uint64_t>(~size_t{0});
    if (pixels > max_reasonable_i420 || pixels > max_size_t_value) {
        return false;
    }

    y_len = static_cast<size_t>(pixels);
    return true;
}

ParticipantSubscription::ParticipantSubscription(uint32_t participant_id,
                                                 const std::string &initial_source_uuid,
                                                 IpcFd e2p_fd)
    : m_participant_id(participant_id)
{
    ZOOMSDK::SDKError err = ZOOMSDK::createRenderer(&m_renderer, this);
    if (err != ZOOMSDK::SDKERR_SUCCESS || !m_renderer) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"create_renderer_failed","source_uuid":")" +
            initial_source_uuid + R"(","participant_id":)" +
            std::to_string(m_participant_id) + R"(,"code":)" +
            std::to_string(static_cast<int>(err)) + "}");
        return;
    }

    m_renderer->setRawDataResolution(ZOOMSDK::ZoomSDKResolution_1080P);
    err = m_renderer->subscribe(participant_id, ZOOMSDK::RAW_DATA_TYPE_VIDEO);
    EngineIpc::write(
        R"({"cmd":"debug","stage":"video_subscribe","source_uuid":")" +
        initial_source_uuid + R"(","participant_id":)" +
        std::to_string(m_participant_id) + R"(,"code":)" +
        std::to_string(static_cast<int>(err)) + "}");
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        ZOOMSDK::destroyRenderer(m_renderer);
        m_renderer = nullptr;
        return;
    }
    add_source(initial_source_uuid, e2p_fd);
}

ParticipantSubscription::~ParticipantSubscription()
{
    if (m_renderer) {
        m_renderer->unSubscribe();
        ZOOMSDK::destroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    for (auto &entry : m_targets) {
        if (entry.second) shm_region_destroy(entry.second->shm);
    }
}

void ParticipantSubscription::add_source(const std::string &source_uuid, IpcFd e2p_fd)
{
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    if (m_targets.find(source_uuid) == m_targets.end())
        m_targets.emplace(source_uuid, std::make_unique<SourceTarget>(e2p_fd));
}

void ParticipantSubscription::remove_source(const std::string &source_uuid)
{
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    auto it = m_targets.find(source_uuid);
    if (it == m_targets.end()) return;
    if (it->second) shm_region_destroy(it->second->shm);
    m_targets.erase(it);
}

bool ParticipantSubscription::empty() const
{
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    return m_targets.empty();
}

std::vector<std::pair<std::string, IpcFd>> ParticipantSubscription::sources() const
{
    std::vector<std::pair<std::string, IpcFd>> result;
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    result.reserve(m_targets.size());
    for (const auto &entry : m_targets) {
        if (entry.second) result.emplace_back(entry.first, entry.second->e2p_fd);
    }
    return result;
}

bool ParticipantSubscription::ensure_shm(SourceTarget &target,
                                         const std::string &source_uuid,
                                         size_t y_len)
{
    const size_t total = sizeof(ShmFrameHeader) + y_len + y_len / 4 + y_len / 4;
    if (total < y_len) return false;
    if (target.shm.ptr && target.shm.size >= total) return true;

    const std::string region_name = IPC_SHM_PREFIX + source_uuid;
    return shm_region_create(target.shm, region_name, total);
}

void ParticipantSubscription::onRawDataFrameReceived(YUVRawDataI420 *data)
{
    if (!data) return;
    const uint32_t w     = data->GetStreamWidth();
    const uint32_t h     = data->GetStreamHeight();
    size_t y_len = 0;
    if (!valid_i420_frame(data, w, h, y_len)) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_frame_invalid","participant_id":)" +
            std::to_string(m_participant_id) + R"(,"w":)" +
            std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
        return;
    }

    std::lock_guard<std::mutex> lock(m_targets_mtx);
    for (auto &entry : m_targets) {
        const std::string &source_uuid = entry.first;
        SourceTarget &target = *entry.second;

        if (!ensure_shm(target, source_uuid, y_len) || !target.shm.ptr) {
            if (target.frame_count == 0) {
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"video_shm_create_failed","source_uuid":")" +
                    source_uuid + R"(","participant_id":)" +
                    std::to_string(m_participant_id) + R"(,"w":)" +
                    std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
            }
            continue;
        }

        auto *hdr    = static_cast<ShmFrameHeader *>(target.shm.ptr);
        auto *pixels = static_cast<char *>(target.shm.ptr) + sizeof(ShmFrameHeader);
        hdr->width = w;
        hdr->height = h;
        hdr->y_len = static_cast<uint32_t>(y_len);

        std::memcpy(pixels,                   data->GetYBuffer(), y_len);
        std::memcpy(pixels + y_len,           data->GetUBuffer(), y_len / 4);
        std::memcpy(pixels + y_len + y_len/4, data->GetVBuffer(), y_len / 4);

        ++target.frame_count;
        if (target.frame_count == 1 || target.frame_count % 120 == 0) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"video_frame_received","source_uuid":")" +
                source_uuid + R"(","participant_id":)" +
                std::to_string(m_participant_id) + R"(,"count":)" +
                std::to_string(target.frame_count) + R"(,"w":)" +
                std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
        }

        EngineIpc::write(
            R"({"cmd":"frame","source_uuid":")" + source_uuid +
            R"(","w":)" + std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
    }
}

void ParticipantSubscription::onRawDataStatusChanged(
    ZOOMSDK::IZoomSDKRendererDelegate::RawDataStatus status)
{
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    for (const auto &entry : m_targets) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_raw_status","source_uuid":")" +
            entry.first + R"(","participant_id":)" +
            std::to_string(m_participant_id) + R"(,"status":)" +
            std::to_string(static_cast<int>(status)) + "}");
    }
}

void ParticipantSubscription::onRendererBeDestroyed()
{
    m_renderer = nullptr;
}

void EngineVideo::subscribe(uint32_t participant_id,
                             const std::string &source_uuid,
                             IpcFd e2p_fd)
{
    unsubscribe_locked(source_uuid);

    auto it = m_subs.find(participant_id);
    if (it == m_subs.end()) {
        it = m_subs.emplace(
            participant_id,
            std::make_unique<ParticipantSubscription>(
                participant_id, source_uuid, e2p_fd)).first;
        if (!it->second || it->second->empty()) {
            m_subs.erase(it);
            return;
        }
    } else {
        it->second->add_source(source_uuid, e2p_fd);
    }
    m_source_participants[source_uuid] = participant_id;
}

void EngineVideo::unsubscribe(const std::string &source_uuid)
{
    unsubscribe_locked(source_uuid);
}

void EngineVideo::unsubscribe_locked(const std::string &source_uuid)
{
    auto source_it = m_source_participants.find(source_uuid);
    if (source_it == m_source_participants.end()) return;

    const uint32_t participant_id = source_it->second;
    m_source_participants.erase(source_it);

    auto sub_it = m_subs.find(participant_id);
    if (sub_it == m_subs.end() || !sub_it->second) return;

    sub_it->second->remove_source(source_uuid);
    if (sub_it->second->empty()) m_subs.erase(sub_it);
}

void EngineVideo::resubscribe_all()
{
    std::vector<std::tuple<std::string, uint32_t, IpcFd>> current;
    for (const auto &entry : m_subs) {
        if (entry.second) {
            for (const auto &source : entry.second->sources()) {
                current.emplace_back(source.first,
                                     entry.second->participant_id(),
                                     source.second);
            }
        }
    }

    m_subs.clear();
    m_source_participants.clear();
    for (const auto &entry : current) {
        const auto &[source_uuid, participant_id, e2p_fd] = entry;
        subscribe(participant_id, source_uuid, e2p_fd);
    }
}
