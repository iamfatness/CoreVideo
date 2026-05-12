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

void ZoomAudioRouter::set_mixed_sink(MixedSink cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_mixed_sink = std::move(cb);
}

void ZoomAudioRouter::clear_mixed_sink()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_mixed_sink = nullptr;
}

void ZoomAudioRouter::set_one_way_sink(OneWaySink cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_one_way_sink = std::move(cb);
}

void ZoomAudioRouter::clear_one_way_sink()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_one_way_sink = nullptr;
}

void ZoomAudioRouter::add_participant_sink(uint32_t user_id, ParticipantSink cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_participant_sinks[user_id] = std::move(cb);
}

void ZoomAudioRouter::remove_participant_sink(uint32_t user_id)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_participant_sinks.erase(user_id);
}

// ── IZoomSDKAudioRawDataDelegate ─────────────────────────────────────────────

void ZoomAudioRouter::onMixedAudioRawDataReceived(AudioRawData *data)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_mixed_sink) m_mixed_sink(data);
}

void ZoomAudioRouter::onOneWayAudioRawDataReceived(AudioRawData *data, uint32_t user_id)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    // Primary delegate receives all one-way frames; it applies m_isolated_user internally.
    if (m_one_way_sink) m_one_way_sink(data, user_id);
    // Per-participant sources receive only their own user's frames.
    auto it = m_participant_sinks.find(user_id);
    if (it != m_participant_sinks.end()) it->second(data);
}
