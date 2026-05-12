#include "zoom-audio-router.h"
#include <rawdata/zoom_rawdata_api.h>
#include <obs-module.h>

ZoomAudioRouter &ZoomAudioRouter::instance()
{
    static ZoomAudioRouter inst;
    return inst;
}

bool ZoomAudioRouter::subscribe()
{
    if (m_subscribed) return true;
    ZOOMSDK::IZoomSDKAudioRawDataHelper *helper = ZOOMSDK::GetAudioRawdataHelper();
    if (!helper) {
        blog(LOG_ERROR, "[obs-zoom-plugin] GetAudioRawdataHelper returned null");
        return false;
    }
    ZOOMSDK::SDKError err = helper->subscribe(this);
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Audio router subscribe failed: %d",
             static_cast<int>(err));
        return false;
    }
    m_subscribed = true;
    blog(LOG_INFO, "[obs-zoom-plugin] Audio router subscribed");
    return true;
}

void ZoomAudioRouter::unsubscribe()
{
    if (!m_subscribed) return;
    ZOOMSDK::IZoomSDKAudioRawDataHelper *helper = ZOOMSDK::GetAudioRawdataHelper();
    if (helper) helper->unSubscribe();
    m_subscribed = false;
    blog(LOG_INFO, "[obs-zoom-plugin] Audio router unsubscribed");
}

void ZoomAudioRouter::add_mixed_sink(void *key, MixedSink cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (cb)
        m_mixed_sinks[key] = std::move(cb);
    else
        m_mixed_sinks.erase(key);
}

void ZoomAudioRouter::remove_mixed_sink(void *key)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_mixed_sinks.erase(key);
}

void ZoomAudioRouter::add_one_way_sink(void *key, OneWaySink cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (cb)
        m_one_way_sinks[key] = std::move(cb);
    else
        m_one_way_sinks.erase(key);
}

void ZoomAudioRouter::remove_one_way_sink(void *key)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_one_way_sinks.erase(key);
}

void ZoomAudioRouter::add_participant_sink(uint32_t user_id, void *key,
                                           ParticipantSink cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (cb) {
        m_participant_sinks[user_id][key] = std::move(cb);
        return;
    }
    auto it = m_participant_sinks.find(user_id);
    if (it == m_participant_sinks.end()) return;
    it->second.erase(key);
    if (it->second.empty()) m_participant_sinks.erase(it);
}

void ZoomAudioRouter::remove_participant_sink(uint32_t user_id, void *key)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_participant_sinks.find(user_id);
    if (it == m_participant_sinks.end()) return;
    it->second.erase(key);
    if (it->second.empty()) m_participant_sinks.erase(it);
}

// ── IZoomSDKAudioRawDataDelegate ─────────────────────────────────────────────

void ZoomAudioRouter::onMixedAudioRawDataReceived(AudioRawData *data)
{
    std::vector<MixedSink> sinks;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto &[key, cb] : m_mixed_sinks)
            if (cb) sinks.push_back(cb);
    }
    for (auto &cb : sinks)
        cb(data);
}

void ZoomAudioRouter::onOneWayAudioRawDataReceived(AudioRawData *data, uint32_t user_id)
{
    std::vector<OneWaySink> one_way_sinks;
    std::vector<ParticipantSink> participant_sinks;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto &[key, cb] : m_one_way_sinks)
            if (cb) one_way_sinks.push_back(cb);
        auto it = m_participant_sinks.find(user_id);
        if (it != m_participant_sinks.end()) {
            for (auto &[key, cb] : it->second)
                if (cb) participant_sinks.push_back(cb);
        }
    }
    for (auto &cb : one_way_sinks)
        cb(data, user_id);
    for (auto &cb : participant_sinks)
        cb(data);
}
