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

bool ZoomOutputManager::configure_output(const std::string &source_name,
                                         uint32_t participant_id,
                                         bool active_speaker,
                                         bool isolate_audio,
                                         AudioChannelMode audio_mode)
{
    std::vector<ZoomSource *> sources;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        sources = m_sources;
    }

    for (auto *source : sources) {
        if (!source) continue;
        if (source->output_name() != source_name) continue;
        source->configure_output(participant_id, active_speaker,
                                 isolate_audio, audio_mode);
        return true;
    }
    return false;
}
