#include "zoom-supersource.h"
#include "zoom-output-manager.h"
#include "zoom-engine-client.h"
#include "zoom-output-health.h"
#include "zoom-iso-recorder.h"
#include "zoom-source.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <obs-module.h>
#include <util/platform.h>

namespace {

bool json_int(const QJsonObject &obj, const char *key, int &value)
{
    const QJsonValue json_value = obj.value(QLatin1String(key));
    if (!json_value.isDouble())
        return false;
    value = json_value.toInt();
    return true;
}

int json_int_or(const QJsonObject &obj, const char *key, int fallback)
{
    int value = fallback;
    json_int(obj, key, value);
    return value;
}

void remember_quality_event(ZoomOutputInfo &info,
                            const ZoomEngineClient::DebugEvent &event,
                            const QJsonObject &obj,
                            uint64_t now_ms)
{
    info.last_quality_stage = event.stage;
    info.last_quality_event_age_ms =
        now_ms > event.timestamp_ms ? now_ms - event.timestamp_ms : 0;

    if (event.stage == "set_resolution") {
        info.last_set_resolution_code =
            json_int_or(obj, "code", info.last_set_resolution_code);
        const int resolution = json_int_or(obj, "resolution", -1);
        if (resolution >= 0)
            info.negotiated_resolution = resolution;
        return;
    }

    if (event.stage == "video_subscribe") {
        info.last_video_subscribe_code =
            json_int_or(obj, "code", info.last_video_subscribe_code);
        const int resolution = json_int_or(obj, "resolution", -1);
        if (resolution >= 0)
            info.negotiated_resolution = resolution;
        return;
    }

    if (event.stage == "video_raw_status") {
        info.last_raw_status = json_int_or(obj, "status", info.last_raw_status);
        return;
    }

    if (event.stage == "video_resolution_downgraded" ||
        event.stage == "video_source_bound" ||
        event.stage == "video_subscription_upgraded") {
        const int actual = json_int_or(obj, "actual", -1);
        const int requested = json_int_or(obj, "requested",
                                          static_cast<int>(info.video_resolution));
        if (actual >= 0)
            info.negotiated_resolution = actual;
        if (actual >= 0 && requested >= 0 && actual < requested)
            info.subscription_downgraded = true;
        return;
    }

    if (event.stage == "video_source_attached_existing_subscription") {
        const int resolution = json_int_or(obj, "resolution", -1);
        if (resolution >= 0)
            info.negotiated_resolution = resolution;
        return;
    }

    if (event.stage == "video_subscribe_noop_existing") {
        const int active = json_int_or(obj, "active", -1);
        const int requested = json_int_or(obj, "requested",
                                          static_cast<int>(info.video_resolution));
        if (active >= 0)
            info.negotiated_resolution = active;
        if (active >= 0 && requested >= 0 && active < requested)
            info.subscription_downgraded = true;
        return;
    }
}

bool quality_stage_is_interesting(const std::string &stage)
{
    return stage == "set_resolution" ||
           stage == "video_subscribe" ||
           stage == "video_raw_status" ||
           stage == "video_resolution_downgraded" ||
           stage == "video_source_bound" ||
           stage == "video_subscription_upgraded" ||
           stage == "video_source_attached_existing_subscription" ||
           stage == "video_subscribe_noop_existing" ||
           stage == "video_subscribe_failed_all" ||
           stage == "video_subscribe_deferred" ||
           stage == "video_subscribe_skipped";
}

void attach_quality_events(std::vector<ZoomOutputInfo> &outputs)
{
    if (outputs.empty())
        return;

    const auto events = ZoomEngineClient::instance().recent_debug_events();
    const uint64_t now_ms = os_gettime_ns() / 1000000ULL;
    for (auto &output : outputs) {
        if (output.source_uuid.empty())
            continue;

        for (const auto &event : events) {
            if (event.source_uuid != output.source_uuid ||
                !quality_stage_is_interesting(event.stage)) {
                continue;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(
                QByteArray::fromStdString(event.message));
            if (!doc.isObject())
                continue;
            remember_quality_event(output, event, doc.object(), now_ms);
        }

        if (output.negotiated_resolution >= 0 &&
            output.negotiated_resolution < static_cast<int>(output.video_resolution)) {
            output.subscription_downgraded = true;
        }
    }
}

} // namespace

ZoomOutputManager &ZoomOutputManager::instance()
{
    static ZoomOutputManager inst;
    return inst;
}

void ZoomOutputManager::register_source(ZoomSource *source)
{
    if (!source) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    if (std::find(m_sources.begin(), m_sources.end(), source) == m_sources.end())
        m_sources.push_back(source);
}

void ZoomOutputManager::unregister_source(ZoomSource *source)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sources.erase(std::remove(m_sources.begin(), m_sources.end(), source),
                    m_sources.end());
}

std::vector<ZoomOutputInfo> ZoomOutputManager::outputs() const
{
    std::vector<ZoomOutputInfo> out;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        out.reserve(m_sources.size());
        for (const auto *source : m_sources) {
            if (!source) continue;
            ZoomOutputInfo info = source->output_info();
            out.push_back(info);
        }
    }
    apply_output_health(out, ZoomEngineClient::instance().roster(),
                        ZoomEngineClient::instance().is_media_active());
    attach_quality_events(out);
    return out;
}

void ZoomOutputManager::set_preview_cb(const std::string &source_name,
                                        ZoomPreviewCallback cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    const auto it = std::find_if(m_sources.begin(), m_sources.end(),
        [&source_name](const ZoomSource *src) {
            return src && src->output_name() == source_name;
        });
    if (it != m_sources.end())
        (*it)->set_preview_cb(std::move(cb));
}

void ZoomOutputManager::clear_preview_cb(const std::string &source_name)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    const auto it = std::find_if(m_sources.begin(), m_sources.end(),
        [&source_name](const ZoomSource *src) {
            return src && src->output_name() == source_name;
        });
    if (it != m_sources.end())
        (*it)->clear_preview_cb();
}

bool ZoomOutputManager::configure_output_ex(const std::string &source_name,
                                            AssignmentMode mode,
                                            uint32_t participant_id,
                                            uint32_t spotlight_slot,
                                            uint32_t failover_participant_id,
                                            bool isolate_audio,
                                            AudioChannelMode audio_mode,
                                            VideoResolution video_resolution,
                                            bool audience_audio,
                                            uint32_t audio_delay_ms)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    const auto it = std::find_if(m_sources.begin(), m_sources.end(),
        [&source_name](const ZoomSource *source) {
            return source && source->output_name() == source_name;
        });
    if (it == m_sources.end())
        return false;

    ZoomSource *source = *it;
    source->configure_output_ex(mode, participant_id, spotlight_slot,
                                failover_participant_id, isolate_audio,
                                audio_mode, video_resolution,
                                audience_audio, audio_delay_ms);
    ZoomIsoRecorder::instance().on_output_updated(source->output_info());
    return true;
}

void ZoomOutputManager::retry_media()
{
    ZoomEngineClient::instance().start_media();
    resubscribe_all();
    zoom_supersource_retry_media();
}

void ZoomOutputManager::resubscribe_all()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    // Re-subscribe by INTENT, not by the current flag. After an engine
    // crash/reconnect the engine is brand new and knows nothing, yet a
    // source's stale m_subscribed flag may be either true or false; gating
    // on is_subscribed() left feeds dark until the operator hit Apply/Recover
    // by hand (2026-08-09). Every source that wants a feed re-subscribes.
    for (auto *src : m_sources)
        if (src && src->wants_subscription()) src->subscribe();
}

uint32_t ZoomOutputManager::recover_stale_sources(bool force)
{
    const uint64_t now_ns = os_gettime_ns();
    std::lock_guard<std::mutex> lk(m_mtx);
    return static_cast<uint32_t>(std::count_if(
        m_sources.begin(), m_sources.end(),
        [now_ns, force](ZoomSource *src) {
            return src && src->recover_stale_video(now_ns, force);
        }));
}

uint32_t ZoomOutputManager::upgrade_low_quality_sources(bool force)
{
    const uint64_t now_ns = os_gettime_ns();
    std::lock_guard<std::mutex> lk(m_mtx);
    return static_cast<uint32_t>(std::count_if(
        m_sources.begin(), m_sources.end(),
        [now_ns, force](ZoomSource *src) {
            return src && src->upgrade_low_quality_video(now_ns, force);
        }));
}

void ZoomOutputManager::clear_all_preview_cbs()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto *src : m_sources)
        if (src) src->clear_preview_cb();
}

bool ZoomOutputManager::configure_output(const std::string &source_name,
                                         uint32_t participant_id,
                                         bool active_speaker,
                                         bool isolate_audio,
                                         AudioChannelMode audio_mode,
                                         VideoResolution video_resolution,
                                         bool audience_audio,
                                         uint32_t audio_delay_ms)
{
    // Hold the mutex for the full operation so a source cannot be unregistered
    // (and freed) between the find and the configure call.
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto *source : m_sources) {
        if (!source) continue;
        if (source->output_name() != source_name) continue;
        source->configure_output(participant_id, active_speaker,
                                 isolate_audio, audio_mode, video_resolution,
                                 audience_audio, audio_delay_ms);
        ZoomIsoRecorder::instance().on_output_updated(source->output_info());
        return true;
    }
    return false;
}
