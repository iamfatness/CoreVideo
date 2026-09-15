#include "iso-audio-tap.h"
#include "zoom-engine-client.h"
#include <util/platform.h>
#include <algorithm>

IsoAudioTap::~IsoAudioTap() { stop(); }
bool IsoAudioTap::start()
{
    const std::weak_ptr<IsoAudioTap> weak = shared_from_this();
    ZoomEngineClient::instance().register_source(m_uuid, {
        {}, [weak](uint32_t bytes, uint32_t, uint32_t gen) {
            if (auto tap = weak.lock()) tap->read(bytes, gen);
        }, [weak] {
            if (auto tap = weak.lock()) {
                std::lock_guard<std::mutex> lock(tap->m_mutex);
                shm_region_destroy(tap->m_region);
                tap->m_started = false;
                tap->m_error = "ISO audio engine restarted; stop and restart ISO recording";
            }
        }});
    if (ZoomEngineClient::instance().subscribe_audio(m_uuid, m_participant, true, false, true))
        return true;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_error = "ISO isolated audio subscription could not be sent";
    return false;
}
void IsoAudioTap::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) return;
        m_closed = true;
        shm_region_destroy(m_region);
    }
    ZoomEngineClient::instance().unregister_source(m_uuid);
    ZoomEngineClient::instance().unsubscribe(m_uuid);
}
