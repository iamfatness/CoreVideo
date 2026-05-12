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
    bool video_active = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frame_count = 0;
    uint64_t last_frame_ns = 0;
};

struct ZoomOutputPreset {
    std::string name;
    std::vector<ZoomOutputInfo> outputs;
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
    bool save_preset(const std::string &name);
    bool apply_preset(const std::string &name);
    bool delete_preset(const std::string &name);
    std::vector<std::string> preset_names() const;

private:
    ZoomOutputManager() = default;
    std::vector<ZoomOutputPreset> load_presets() const;
    void save_presets(const std::vector<ZoomOutputPreset> &presets) const;

    mutable std::mutex m_mtx;
    std::vector<ZoomSource *> m_sources;
};
