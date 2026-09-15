#include "iso-audio-reader.h"
#include <algorithm>
void IsoAudioReader::read(uint32_t bytes, uint32_t generation)
{
    // Copy and release the tap lock before calling the recorder: Stop acquires
    // the recorder lock first, and a callback must never reverse that order.
    struct Packet { std::vector<uint8_t> pcm; uint32_t rate; uint16_t channels; uint64_t ns; };
    std::vector<Packet> packets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed || !m_error.empty() || !bytes || bytes > 1024*1024) return;
        const auto base = IPC_SHM_PREFIX + m_uuid + "_audio";
        const auto name = shm_region_name(base, generation);
        if (m_generation != generation || m_region.size < shm_audio_region_bytes(bytes)) {
            shm_region_destroy(m_region); m_started = false;
        }
        if (!m_region.ptr) {
            if (!shm_region_open_readwrite(m_region, name, shm_audio_region_bytes(bytes))) return;
            m_generation = generation;
        }
        auto *ring = static_cast<ShmAudioHeader *>(m_region.ptr);
        if (ring->slot_count != kAudioRingSlots || !ring->slot_bytes || ring->slot_bytes > 1024*1024) {
            m_error = "ISO audio ring format is invalid"; return;
        }
        const auto needed = shm_audio_region_bytes(ring->slot_bytes);
        if (needed > m_region.size) {
            shm_region_destroy(m_region);
            if (!shm_region_open_readwrite(m_region, name, needed)) return;
            ring = static_cast<ShmAudioHeader *>(m_region.ptr);
        }
        if (ring->slot_count != kAudioRingSlots || ring->slot_bytes > 1024*1024 ||
            shm_audio_region_bytes(ring->slot_bytes) > m_region.size) {
            m_error = "ISO audio ring resized beyond mapped bounds"; return;
        }
        if (!m_started) {
            // This UUID is unique to the recording. Preserve its first buffers.
            m_index = ring->write_index > kAudioRingSlots ? ring->write_index-kAudioRingSlots : 0;
            m_started = true;
        }
        for (int pass=0; pass<16; ++pass) {
            const auto end = ring->write_index;
            if (end-m_index > kAudioRingSlots) {
                m_error = "ISO isolated audio ring overrun"; audio_ring_reader_abandon(ring); break;
            }
            while (m_index != end) {
                const auto index = m_index++;
                const auto *slot = reinterpret_cast<const ShmAudioSlot *>(
                    static_cast<const char *>(m_region.ptr)+shm_audio_slot_offset(*ring,index%kAudioRingSlots));
                for (int attempt=0; attempt<3; ++attempt) {
                    const auto seq = slot->sequence;
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if (seq&1) continue;
                    const auto len=slot->byte_len, participant=slot->participant_id;
                    const auto rate=ring->sample_rate; const auto channels=ring->channels;
                    if (!len || len>ring->slot_bytes) break;
                    m_pcm.resize(len); std::memcpy(m_pcm.data(),slot+1,len);
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if (slot->sequence!=seq || ring->write_index-index>kAudioRingSlots) continue;
                    if (participant != m_participant) {
                        m_error="ISO audio participant attribution mismatch"; break;
                    }
                    packets.push_back({m_pcm,rate,channels,m_now()});
                    break;
                }
            }
            if (audio_ring_reader_done(ring,m_index)) break;
            if (pass==15) audio_ring_reader_abandon(ring);
        }
    }
    for (const auto &packet:packets)
        m_deliver(packet.pcm.data(),uint32_t(packet.pcm.size()),packet.rate,packet.channels,packet.ns);
}
