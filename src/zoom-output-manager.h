#pragma once

#include "zoom-types.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct ZoomSource;

struct ZoomOutputInfo {
    std::string source_uuid;
    std::string source_name;
    std::string display_name; // user-editable label; falls back to source_name if empty
    uint32_t participant_id = 0;
    bool active_speaker = false;
    bool isolate_audio = false;
    bool audience_audio = false;
    AudioChannelMode audio_mode = AudioChannelMode::Mono;
    VideoResolution video_resolution = VideoResolution::P720;
    uint32_t observed_width = 0;
    uint32_t observed_height = 0;
    double observed_fps = 0.0;
    AssignmentMode   assignment = AssignmentMode::Participant;
    uint32_t         spotlight_slot = 1;     // used when assignment == SpotlightIndex
    uint32_t         failover_participant_id = 0; // 0 = none
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
                          AudioChannelMode audio_mode,
                          VideoResolution video_resolution = VideoResolution::P720,
                          bool audience_audio = false);
    // Extended variant supporting ZoomISO-style assignment modes (spotlight,
    // screen share) plus failover. Returns true if the output was found.
    bool configure_output_ex(const std::string &source_name,
                             AssignmentMode mode,
                             uint32_t participant_id,
                             uint32_t spotlight_slot,
                             uint32_t failover_participant_id,
                             bool isolate_audio,
                             AudioChannelMode audio_mode,
                             VideoResolution video_resolution = VideoResolution::P720,
                             bool audience_audio = false);

    // Re-send subscribe commands for all active sources after engine recovery.
    void resubscribe_all();

    // Preview callbacks — call from the UI thread only.
    void set_preview_cb(const std::string &source_name,
                        ZoomPreviewCallback cb);
    void clear_preview_cb(const std::string &source_name);
    void clear_all_preview_cbs();

private:
    ZoomOutputManager() = default;

    mutable std::mutex m_mtx;
    std::vector<ZoomSource *> m_sources;
};
