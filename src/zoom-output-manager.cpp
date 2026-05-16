#include "zoom-output-manager.h"
#include "zoom-source.h"
#include <algorithm>
#include <obs-module.h>

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
    std::vector<ZoomSource *> sources;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        sources = m_sources;
    }

    std::vector<ZoomOutputInfo> out;
    out.reserve(sources.size());
    for (auto *source : sources) {
        if (!source) continue;
        out.push_back(source->output_info());
    }
    return out;
}

void ZoomOutputManager::set_preview_cb(const std::string &source_name,
                                        ZoomPreviewCallback cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto *src : m_sources) {
        if (src && src->output_name() == source_name) {
            src->set_preview_cb(std::move(cb));
            return;
        }
    }
}

void ZoomOutputManager::clear_preview_cb(const std::string &source_name)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto *src : m_sources) {
        if (src && src->output_name() == source_name) {
            src->clear_preview_cb();
            return;
        }
    }
}

bool ZoomOutputManager::configure_output_ex(const std::string &source_name,
                                            AssignmentMode mode,
                                            uint32_t participant_id,
                                            uint32_t spotlight_slot,
                                            uint32_t failover_participant_id,
                                            bool isolate_audio,
                                            AudioChannelMode audio_mode,
                                            VideoResolution video_resolution,
                                            bool audience_audio)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto *source : m_sources) {
        if (!source) continue;
        if (source->output_name() != source_name) continue;
        source->configure_output_ex(mode, participant_id, spotlight_slot,
                                    failover_participant_id, isolate_audio,
                                    audio_mode, video_resolution,
                                    audience_audio);
        return true;
    }
    return false;
}

void ZoomOutputManager::resubscribe_all()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto *src : m_sources)
        if (src && src->is_subscribed()) src->subscribe();
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
                                         bool audience_audio)
{
    // Hold the mutex for the full operation so a source cannot be unregistered
    // (and freed) between the find and the configure call.
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto *source : m_sources) {
        if (!source) continue;
        if (source->output_name() != source_name) continue;
        source->configure_output(participant_id, active_speaker,
                                 isolate_audio, audio_mode, video_resolution,
                                 audience_audio);
        return true;
    }
    return false;
}
