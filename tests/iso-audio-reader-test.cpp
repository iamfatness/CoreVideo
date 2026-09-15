#include "iso-audio-reader.h"
#include <chrono>
#include <iostream>

int main()
{
    const auto uuid="iso_reader_test_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    ShmRegion writer{};
    const auto base=IPC_SHM_PREFIX+uuid+"_audio";
    if (!shm_region_create(writer,shm_region_name(base,1),shm_audio_region_bytes(8))) return 1;
    auto *ring=static_cast<ShmAudioHeader *>(writer.ptr);
    std::memset(writer.ptr,0,writer.size);
    ring->slot_count=kAudioRingSlots; ring->slot_bytes=8; ring->sample_rate=48000; ring->channels=1;
    int delivered=0; bool correct=true;
    {
        IsoAudioReader reader(uuid,42,[&](const uint8_t *pcm,uint32_t bytes,uint32_t rate,uint16_t ch,uint64_t ns) {
            ++delivered;
            correct &= bytes==8 && rate==48000 && ch==1 && ns==123 && pcm[0]==uint8_t(delivered);
        }, [] {return 123ULL;});
        auto publish=[&](uint32_t id,uint8_t value) {
            auto *slot=reinterpret_cast<ShmAudioSlot *>(static_cast<char *>(writer.ptr)+
                shm_audio_slot_offset(*ring,ring->write_index%kAudioRingSlots));
            slot->sequence=1; slot->participant_id=id; slot->byte_len=8;
            std::memset(slot+1,value,8); slot->sequence=2; ++ring->write_index; ring->notify=1;
        };
        // Coalesced first wakeup must retain all initial buffers, not skip them.
        publish(42,1); publish(42,2); reader.read(8,1);
        if(delivered!=2 || !correct || ring->notify!=0 || !reader.error().empty()) return 2;
        reader.read(8,1); if(delivered!=2) return 3;
        publish(42,3); reader.read(8,1);
        if(delivered!=3 || !correct) return 4;
        // Even valid PCM with another attribution is never handed to the writer.
        publish(43,4); reader.read(8,1);
        if(delivered!=3 || reader.error().empty()) return 5;
    }
    shm_region_destroy(writer);
    std::cout << "Isolated reader preserves first/batched audio and rejects other participants\n";
}
