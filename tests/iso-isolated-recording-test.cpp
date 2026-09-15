#include "iso-audio-reader.h"
#include "iso-audio-routing.h"
#include "iso-track-writer.h"
#include <util/platform.h>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

int main(int argc,char **argv)
{
    if(argc!=2) return 2;
    const auto epoch=os_gettime_ns();
    std::array<ShmRegion,2> regions{};
    std::array<std::unique_ptr<IsoTrackWriter>,2> writers;
    std::array<std::unique_ptr<IsoAudioReader>,2> readers;
    for(int i=0;i<2;++i) {
        IsoTrackWriter::Config config;
        config.ffmpeg=argv[1]; config.encoder="libx264";
        config.path="isolated-"+std::to_string(i+1)+".mp4"; config.log_path=config.path+".log";
        config.epoch_ns=epoch; config.now=[] {return os_gettime_ns();};
        writers[i]=std::make_unique<IsoTrackWriter>(config);
        const auto uuid="isolated_test_"+std::to_string(epoch)+"_"+std::to_string(i);
        if(!shm_region_create(regions[i],shm_region_name(IPC_SHM_PREFIX+uuid+"_audio",1),shm_audio_region_bytes(960))) return 3;
        std::memset(regions[i].ptr,0,regions[i].size);
        auto *ring=static_cast<ShmAudioHeader *>(regions[i].ptr);
        ring->slot_count=kAudioRingSlots; ring->slot_bytes=960; ring->sample_rate=48000; ring->channels=1;
        readers[i]=std::make_unique<IsoAudioReader>(uuid,i+1,[&,i](auto pcm,auto bytes,auto rate,auto channels,auto ns) {
            writers[i]->audio(pcm,bytes,rate,channels,ns);
        }, [] {return os_gettime_ns();});
    }
    const auto begin=std::chrono::steady_clock::now();
    std::vector<uint8_t> image(1920*1080*3/2,128);
    std::array<int16_t,480> audio{};
    for(int packet=0;packet<400;++packet) {
        std::this_thread::sleep_until(begin+std::chrono::milliseconds(packet*10));
        const uint32_t speaker=uint32_t((packet/100)%2+1);
        const double frequency=speaker==1?500:1500;
        for(int j=0;j<480;++j) audio[j]=int16_t(12000*std::sin(2*3.141592653589793*frequency*j/48000));
        for(int i=0;i<2;++i) {
            // The production engine policy must never pass the mixed callback.
            if(audio_receives_mix(true,false,true)) return 4;
            if(audio_receives_participant(i+1,speaker,true,true)) {
                auto *ring=static_cast<ShmAudioHeader *>(regions[i].ptr);
                auto *slot=reinterpret_cast<ShmAudioSlot *>(static_cast<char *>(regions[i].ptr)+
                    shm_audio_slot_offset(*ring,ring->write_index%kAudioRingSlots));
                slot->sequence=1; slot->participant_id=speaker; slot->byte_len=960;
                std::memcpy(slot+1,audio.data(),960); slot->sequence=2; ++ring->write_index; ring->notify=1;
                readers[i]->read(960,1);
            }
            if(packet%3==0) writers[i]->video(1920,1080,image.data(),image.data()+1920*1080,
                image.data()+1920*1080*5/4,1920,960,epoch+uint64_t(packet)*10000000);
        }
    }
    for(auto &writer:writers) writer->close(epoch+4000000000ULL);
    for(int i=0;i<2;++i) {
        if(!writers[i]->wait_finished(15000) || !writers[i]->status().error.empty() || !readers[i]->error().empty()) return 5;
        readers[i].reset(); shm_region_destroy(regions[i]);
    }
}
