#pragma once

#include "zoom-audio-delegate.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct ZoomSource;

struct ZoomOutputInfo {
    std::string source_name;
    uint32_t participant_id = 0;
    bool active_speaker = false;
    bool isolate_audio = false;
    AudioChannelMode audio_mode = AudioChannelMode::Mono;
};

class ZoomOutputManager {
public:
    static ZoomOutputManager &instance();

    void register_source(ZoomSource *source);
    void unregister_source(ZoomSource *source);

    std::vector<ZoomOutputInfo> outputs() const;
    bool configure_output(const std::string &source_name,
                          uint32_t participant_id,
                          bool active_speaker,
                          bool isolate_audio,
                          AudioChannelMode audio_mode);

private:
    ZoomOutputManager() = default;

    mutable std::mutex m_mtx;
    std::vector<ZoomSource *> m_sources;
};
