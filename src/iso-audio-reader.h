#pragma once
#include "engine-ipc.h"
#include <functional>
#include <mutex>
#include <vector>

class IsoAudioReader {
public:
    using Deliver = std::function<void(const uint8_t *, uint32_t, uint32_t, uint16_t, uint64_t)>;
    using Now = std::function<uint64_t()>;
    IsoAudioReader(std::string uuid, uint32_t participant, Deliver deliver, Now now)
        : m_uuid(std::move(uuid)), m_participant(participant), m_deliver(std::move(deliver)), m_now(std::move(now)) {}
    ~IsoAudioReader() { shm_region_destroy(m_region); }
    void read(uint32_t bytes, uint32_t generation);
    std::string error() const { std::lock_guard<std::mutex> lock(m_mutex); return m_error; }
protected:
    std::string m_uuid;
    uint32_t m_participant;
    Deliver m_deliver;
    mutable std::mutex m_mutex;
    ShmRegion m_region{};
    uint32_t m_generation = 0, m_index = 0;
    bool m_started = false, m_closed = false;
    std::vector<uint8_t> m_pcm;
    std::string m_error;
    Now m_now;
};
