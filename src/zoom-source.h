#pragma once

#include "engine-ipc.h"
#include "zoom-output-manager.h"
#include "zoom-types.h"
#include <obs-module.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define OBS_ZOOM_PLUGIN_VERSION "0.1.0"

void zoom_source_register();

struct ZoomSource {
    obs_source_t *source = nullptr;
    std::string source_uuid;
    std::string meeting_id;
    std::string passcode;
    std::string display_name;
    std::string output_display_name;
    uint32_t participant_id = 0;
    bool auto_join = false;
    bool active_speaker_mode = false;
    bool isolate_audio = false;
    AudioChannelMode audio_mode = AudioChannelMode::Mono;
    VideoResolution resolution = VideoResolution::P1080;
    VideoLossMode video_loss_mode = VideoLossMode::LastFrame;
    uint32_t speaker_sensitivity_ms = 300;
    uint32_t speaker_hold_ms = 2000;

    void apply_settings(obs_data_t *settings);
    std::string output_name() const;
    ZoomOutputInfo output_info() const;
    void configure_output(uint32_t new_participant_id,
                          bool new_active_speaker_mode,
                          bool new_isolate_audio,
                          AudioChannelMode new_audio_mode);
    void subscribe();
    void unsubscribe();
    void on_roster_changed();
    void on_engine_frame(uint32_t width, uint32_t height);
    void on_engine_audio(uint32_t byte_len);

    uint32_t width() const { return m_width.load(std::memory_order_relaxed); }
    uint32_t height() const { return m_height.load(std::memory_order_relaxed); }
    void set_preview_cb(ZoomPreviewCallback cb);
    void clear_preview_cb();
    void release_shared_memory();

private:
    void output_black_frame();

    mutable std::mutex m_mtx;
    ShmRegion m_video_shm;
    ShmRegion m_audio_shm;
    std::atomic<uint32_t> m_width{0};
    std::atomic<uint32_t> m_height{0};
    std::vector<int16_t> m_stereo_buf;
    ZoomPreviewCallback m_preview_cb;
    uint64_t m_preview_last_ns = 0;
    bool m_subscribed = false;
    uint32_t m_current_subscription_id = 0;
};
