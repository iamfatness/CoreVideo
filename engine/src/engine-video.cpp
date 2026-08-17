#include "engine-video.h"
#include "engine-writer.h"
#include "tile-clock-log.h"
#if __has_include(<rawdata/zoom_rawdata_api.h>)
#include <rawdata/zoom_rawdata_api.h>
#else
#include <zoom_rawdata_api.h>
#endif
#include <cstring>
#include <limits>
#include <atomic>
#include <algorithm>
#include <iterator>
#include <tuple>
#include <vector>

static ZOOMSDK::ZoomSDKResolution sdk_resolution(uint32_t resolution)
{
    switch (resolution) {
    case 0: return ZOOMSDK::ZoomSDKResolution_360P;
    case 2: return ZOOMSDK::ZoomSDKResolution_1080P;
    case 1:
    default: return ZOOMSDK::ZoomSDKResolution_720P;
    }
}

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
                                                 IpcFd e2p_fd,
                                                 uint32_t resolution)
    : m_participant_id(participant_id)
{
    if (resolution > 2) resolution = 1;

    std::vector<uint32_t> attempts;
    for (int candidate = static_cast<int>(resolution); candidate >= 0; --candidate)
        attempts.push_back(static_cast<uint32_t>(candidate));

    for (const uint32_t candidate_resolution : attempts) {
        ZOOMSDK::SDKError err = ZOOMSDK::createRenderer(&m_renderer, this);
        if (err != ZOOMSDK::SDKERR_SUCCESS || !m_renderer) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"create_renderer_failed","source_uuid":")" +
                initial_source_uuid + R"(","participant_id":)" +
                std::to_string(m_participant_id) + R"(,"code":)" +
                std::to_string(static_cast<int>(err)) + R"(,"resolution":)" +
                std::to_string(candidate_resolution) + "}");
            continue;
        }

        m_resolution = candidate_resolution;
        const ZOOMSDK::SDKError res_err =
            m_renderer->setRawDataResolution(sdk_resolution(m_resolution));
        EngineIpc::write(
            R"({"cmd":"debug","stage":"set_resolution","source_uuid":")" +
            initial_source_uuid + R"(","participant_id":)" +
            std::to_string(m_participant_id) + R"(,"code":)" +
            std::to_string(static_cast<int>(res_err)) + R"(,"resolution":)" +
            std::to_string(m_resolution) + "}");

        err = m_renderer->subscribe(participant_id, ZOOMSDK::RAW_DATA_TYPE_VIDEO);
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_subscribe","source_uuid":")" +
            initial_source_uuid + R"(","participant_id":)" +
            std::to_string(m_participant_id) + R"(,"code":)" +
            std::to_string(static_cast<int>(err)) + R"(,"resolution":)" +
            std::to_string(m_resolution) + "}");
        if (err == ZOOMSDK::SDKERR_SUCCESS) {
            add_source(initial_source_uuid, e2p_fd);
            if (m_resolution != resolution) {
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"video_resolution_downgraded","source_uuid":")" +
                    initial_source_uuid + R"(","participant_id":)" +
                    std::to_string(m_participant_id) + R"(,"requested":)" +
                    std::to_string(resolution) + R"(,"actual":)" +
                    std::to_string(m_resolution) + "}");
            }
            return;
        }

        ZOOMSDK::destroyRenderer(m_renderer);
        m_renderer = nullptr;
    }

    EngineIpc::write(
        R"({"cmd":"debug","stage":"video_subscribe_failed_all","source_uuid":")" +
        initial_source_uuid + R"(","participant_id":)" +
        std::to_string(m_participant_id) + R"(,"requested":)" +
        std::to_string(resolution) + "}");
}

bool ParticipantSubscription::set_resolution(uint32_t resolution)
{
    if (resolution > 2) resolution = 1;
    if (!m_renderer)
        return false;
    if (resolution == m_resolution)
        return true;
    const ZOOMSDK::SDKError err =
        m_renderer->setRawDataResolution(sdk_resolution(resolution));
    EngineIpc::write(
        R"({"cmd":"debug","stage":"set_resolution_inplace","participant_id":)" +
        std::to_string(m_participant_id) + R"(,"code":)" +
        std::to_string(static_cast<int>(err)) + R"(,"from":)" +
        std::to_string(m_resolution) + R"(,"to":)" +
        std::to_string(resolution) + "}");
    if (err != ZOOMSDK::SDKERR_SUCCESS)
        return false;
    m_resolution = resolution;
    return true;
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
    const auto [it, inserted] = m_targets.emplace(source_uuid, nullptr);
    if (inserted)
        it->second = std::make_unique<SourceTarget>(e2p_fd);
}

size_t ParticipantSubscription::target_count() const
{
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    return m_targets.size();
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
    const size_t total =
        sizeof(ShmFrameHeader) + y_len + y_len / 4 + y_len / 4;
    if (total < y_len) return false;
    if (target.shm.ptr && target.shm.size >= total) return true;

    // Bump the generation BEFORE creating and bake it into the name: a
    // resize under the old name deadlocks while the plugin still maps the
    // old (smaller) section (see shm_region_name in engine-ipc.h).
    //
    // The counter is NOT a member of this target. A re-subscribe destroys the
    // target (EngineVideo::subscribe -> unsubscribe_locked -> remove_source),
    // so a per-target counter restarted at 0 and put every re-created region
    // back on the legacy unsuffixed name — which is how the resize race kept
    // reaching production. See src/shm-generation.h.
    const ShmRegionAllocation region =
        shm_next_region(shm_generations(), IPC_SHM_PREFIX + source_uuid);
    if (!shm_region_create(target.shm, region.name, total)) return false;
    target.shm_gen = region.gen; // old plugin-side mappings are now stale
    EngineIpc::write(
        R"({"cmd":"debug","stage":"video_shm_created","source_uuid":")" +
        source_uuid + R"(","participant_id":)" +
        std::to_string(m_participant_id) + R"(,"shm_gen":)" +
        std::to_string(region.gen) + R"(,"bytes":)" +
        std::to_string(total) + "}");
    return true;
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
    tile_clock_log(m_participant_id, data->GetTimeStamp(), tile_clock_now_ns(), "v");

    std::lock_guard<std::mutex> lock(m_targets_mtx);
    for (auto &entry : m_targets) {
        const std::string &source_uuid = entry.first;
        SourceTarget &target = *entry.second;

        if (!ensure_shm(target, source_uuid, y_len) || !target.shm.ptr) {
            // Surface once per failure episode (not per frame) as a real
            // error so the plugin can show the operator that this source's
            // frames are being dropped.
            if (!target.shm_fail_reported) {
                target.shm_fail_reported = true;
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"video_shm_create_failed","source_uuid":")" +
                    source_uuid + R"(","participant_id":)" +
                    std::to_string(m_participant_id) + R"(,"w":)" +
                    std::to_string(w) + R"(,"h":)" + std::to_string(h) +
                    R"(,"last_error":)" +
                    std::to_string(target.shm.last_error) +
                    // Which generation the failed create actually attempted. A
                    // failure on a _gN name is a different bug from a failure
                    // on the legacy name — do not make the next incident guess.
                    R"(,"attempted_gen":)" +
                    std::to_string(shm_generations().issued(
                        IPC_SHM_PREFIX + source_uuid)) + "}");
                EngineIpc::write(
                    R"({"cmd":"error","msg":"shm_create_failed","source_uuid":")" +
                    source_uuid + R"(","participant_id":)" +
                    std::to_string(m_participant_id) + R"(,"w":)" +
                    std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
            }
            continue;
        }
        if (target.shm_fail_reported) {
            target.shm_fail_reported = false;
            EngineIpc::write(
                R"({"cmd":"debug","stage":"video_shm_recovered","source_uuid":")" +
                source_uuid + R"(","participant_id":)" +
                std::to_string(m_participant_id) + "}");
        }

        auto *hdr    = static_cast<ShmFrameHeader *>(target.shm.ptr);
        auto *pixels = static_cast<char *>(target.shm.ptr) + sizeof(ShmFrameHeader);
        uint32_t seq = hdr->sequence + 1;
        if ((seq & 1u) == 0) ++seq;
        hdr->sequence = seq;
        std::atomic_thread_fence(std::memory_order_release);
        hdr->width = w;
        hdr->height = h;
        hdr->y_len = static_cast<uint32_t>(y_len);
        // See ShmAudioSlot::capture_ns in engine-ipc.h for why tile_clock_now_ns()
        // (not os_gettime_ns(), which this standalone process cannot call) is
        // the right clock here and what it lets the plugin measure.
        hdr->capture_ns = tile_clock_now_ns();

        std::memcpy(pixels,                   data->GetYBuffer(), y_len);
        std::memcpy(pixels + y_len,           data->GetUBuffer(), y_len / 4);
        std::memcpy(pixels + y_len + y_len/4, data->GetVBuffer(), y_len / 4);
        std::atomic_thread_fence(std::memory_order_release);
        hdr->sequence = seq + 1;

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
            R"(","participant_id":)" + std::to_string(m_participant_id) +
            R"(,"w":)" + std::to_string(w) + R"(,"h":)" + std::to_string(h) +
            R"(,"shm_gen":)" + std::to_string(target.shm_gen) + "}");
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
                             IpcFd e2p_fd,
                             uint32_t resolution)
{
    if (participant_id == 0) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_subscribe_skipped","source_uuid":")" +
            source_uuid + R"(","participant_id":0,"reason":"missing_participant"})");
        unsubscribe_locked(source_uuid);
        return;
    }

    // Bound the number of distinct video source UUIDs. Each source backs a
    // shared-memory region, so without a cap a misbehaving or runaway plugin
    // could create unbounded SHM regions and exhaust memory (policy and
    // rationale live with kMaxShmSources in engine-ipc.h). Re-subscribing an
    // existing source is always allowed.
    if (shm_source_over_cap(m_source_participants.size(),
                            m_source_participants.find(source_uuid) !=
                                m_source_participants.end())) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_subscribe_rejected_capacity","source_uuid":")" +
            source_uuid + R"(","participant_id":)" +
            std::to_string(participant_id) + R"(,"limit":)" +
            std::to_string(kMaxShmSources) + "}");
        EngineIpc::write(
            R"({"cmd":"error","msg":"subscribe_rejected","reason":"shm_capacity","source_uuid":")" +
            source_uuid + R"(","limit":)" +
            std::to_string(kMaxShmSources) + "}");
        return;
    }

    auto existing_source = m_source_participants.find(source_uuid);
    if (existing_source != m_source_participants.end()) {
        if (existing_source->second.participant_id == participant_id) {
            auto existing_sub = m_subs.find(participant_id);
            if (existing_sub != m_subs.end() && existing_sub->second &&
                existing_sub->second->active()) {
                const uint32_t current_resolution = existing_sub->second->resolution();
                if (resolution <= current_resolution) {
                    EngineIpc::write(
                        R"({"cmd":"debug","stage":"video_subscribe_noop_existing","source_uuid":")" +
                        source_uuid + R"(","participant_id":)" +
                        std::to_string(participant_id) + R"(,"requested":)" +
                        std::to_string(resolution) + R"(,"active":)" +
                        std::to_string(current_resolution) + "}");
                    return;
                }
            } else {
                unsubscribe_locked(source_uuid);
            }
        } else {
            unsubscribe_locked(source_uuid);
        }
    }

    if (!m_raw_media_active) {
        m_source_participants[source_uuid] = {
            participant_id,
            resolution,
            e2p_fd
        };
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_subscribe_deferred","source_uuid":")" +
            source_uuid + R"(","participant_id":)" +
            std::to_string(participant_id) + R"(,"requested":)" +
            std::to_string(resolution) + R"(,"reason":"raw_media_not_ready"})");
        return;
    }

    auto it = m_subs.find(participant_id);
    if (it != m_subs.end() && it->second) {
        if (it->second->active()) {
            if (resolution > it->second->resolution()) {
                // Raise the resolution on the LIVE renderer. The old path
                // destroyed and recreated the renderer, which races the
                // SDK's async participant release and returns WRONG_USAGE on
                // the recreate — killing every source sharing this
                // participant (the "switch to Active Speaker kills a mapped
                // source, Apply restores it" report). Change it in place and
                // just attach the new source as another target.
                const uint32_t previous = it->second->resolution();
                const bool raised = it->second->set_resolution(resolution);
                it->second->add_source(source_uuid, e2p_fd);
                for (const auto &target : it->second->sources()) {
                    m_source_participants[target.first] = {
                        participant_id,
                        it->second->resolution(),
                        target.second
                    };
                }
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"video_subscription_upgraded","source_uuid":")" +
                    source_uuid + R"(","participant_id":)" +
                    std::to_string(participant_id) + R"(,"requested":)" +
                    std::to_string(resolution) + R"(,"previous":)" +
                    std::to_string(previous) + R"(,"actual":)" +
                    std::to_string(it->second->resolution()) +
                    R"(,"in_place":)" + (raised ? "true" : "false") +
                    R"(,"active_targets":)" +
                    std::to_string(it->second->target_count()) + "}");
                return;
            }
            it->second->add_source(source_uuid, e2p_fd);
            m_source_participants[source_uuid] = {
                participant_id,
                it->second->resolution(),
                e2p_fd
            };
            EngineIpc::write(
                R"({"cmd":"debug","stage":"video_source_attached_existing_subscription","source_uuid":")" +
                source_uuid + R"(","participant_id":)" +
                std::to_string(participant_id) + R"(,"resolution":)" +
                std::to_string(it->second->resolution()) + R"(,"active_targets":)" +
                std::to_string(it->second->target_count()) + "}");
            return;
        }
        m_subs.erase(it);
        it = m_subs.end();
    }

    if (it == m_subs.end()) {
        it = m_subs.emplace(
            participant_id,
            std::make_unique<ParticipantSubscription>(
                participant_id, source_uuid, e2p_fd, resolution)).first;
        if (!it->second || it->second->empty()) {
            m_subs.erase(it);
            return;
        }
    } else {
        it->second->add_source(source_uuid, e2p_fd);
    }
    m_source_participants[source_uuid] = {
        participant_id,
        it->second->resolution(),
        e2p_fd
    };
    EngineIpc::write(
        R"({"cmd":"debug","stage":"video_source_bound","source_uuid":")" +
        source_uuid + R"(","participant_id":)" +
        std::to_string(participant_id) + R"(,"requested":)" +
        std::to_string(resolution) + R"(,"actual":)" +
        std::to_string(it->second->resolution()) + R"(,"participant_subscriptions":)" +
        std::to_string(m_subs.size()) + R"(,"source_bindings":)" +
        std::to_string(m_source_participants.size()) + "}");
}

void EngineVideo::set_raw_media_active(bool active)
{
    if (m_raw_media_active == active) return;
    m_raw_media_active = active;
    EngineIpc::write(
        R"({"cmd":"debug","stage":"video_raw_media_state","active":)" +
        std::string(active ? "true" : "false") +
        R"(,"pending_sources":)" +
        std::to_string(m_source_participants.size()) + "}");
}

void EngineVideo::unsubscribe(const std::string &source_uuid)
{
    unsubscribe_locked(source_uuid);
}

void EngineVideo::unsubscribe_locked(const std::string &source_uuid)
{
    auto source_it = m_source_participants.find(source_uuid);
    if (source_it == m_source_participants.end()) return;

    const uint32_t participant_id = source_it->second.participant_id;
    m_source_participants.erase(source_it);

    auto sub_it = m_subs.find(participant_id);
    if (sub_it == m_subs.end() || !sub_it->second) return;

    sub_it->second->remove_source(source_uuid);
    if (sub_it->second->empty()) m_subs.erase(sub_it);
}

void EngineVideo::resubscribe_all()
{
    std::vector<std::tuple<std::string, uint32_t, IpcFd, uint32_t>> current;
    for (const auto &entry : m_source_participants) {
        if (entry.second.e2p_fd != kIpcInvalidFd) {
            current.emplace_back(entry.first,
                                 entry.second.participant_id,
                                 entry.second.e2p_fd,
                                 entry.second.resolution);
        }
    }
    if (current.empty()) {
        for (const auto &entry : m_subs) {
            if (entry.second) {
                const uint32_t participant_id = entry.second->participant_id();
                const uint32_t resolution = entry.second->resolution();
                const auto sources = entry.second->sources();
                std::transform(sources.begin(), sources.end(),
                               std::back_inserter(current),
                               [participant_id, resolution](const auto &source) {
                                   return std::make_tuple(source.first,
                                                          participant_id,
                                                          source.second,
                                                          resolution);
                               });
            }
        }
    }

    m_subs.clear();
    m_source_participants.clear();
    if (!m_raw_media_active) {
        std::for_each(current.begin(), current.end(), [this](const auto &entry) {
            const auto &[source_uuid, participant_id, e2p_fd, resolution] = entry;
            m_source_participants[source_uuid] = {
                participant_id,
                resolution,
                e2p_fd
            };
        });
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_resubscribe_deferred","pending_sources":)" +
            std::to_string(m_source_participants.size()) +
            R"(,"reason":"raw_media_not_ready"})");
        return;
    }
    std::for_each(current.begin(), current.end(), [this](const auto &entry) {
        const auto &[source_uuid, participant_id, e2p_fd, resolution] = entry;
        subscribe(participant_id, source_uuid, e2p_fd, resolution);
    });
}

void EngineVideo::suspend_all()
{
    m_subs.clear();
    EngineIpc::write(
        R"({"cmd":"debug","stage":"video_suspended","pending_sources":)" +
        std::to_string(m_source_participants.size()) + "}");
}

void EngineVideo::unsubscribe_all()
{
    m_subs.clear();
    m_source_participants.clear();
}
