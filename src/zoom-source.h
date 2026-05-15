#pragma once

#include "engine-ipc.h"
#include "hw-video-pipeline.h"
#include "zoom-output-manager.h"
#include "zoom-types.h"
#include <obs-module.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "obs-zoom-version.h"

void zoom_source_register();

struct ZoomSource {
    obs_source_t *source = nullptr;
    std::string source_uuid;
    std::string output_display_name;
    // These scalars are written from the OBS UI thread (apply_settings,
    // configure_output) and read from the IPC reader thread
    // (on_engine_audio, on_roster_changed). Make them atomic so the
    // cross-thread reads are race-free without serializing the whole struct.
    std::atomic<uint32_t> participant_id{0};
    std::atomic<bool> active_speaker_mode{false};
    bool isolate_audio = false;
    std::atomic<AudioChannelMode> audio_mode{AudioChannelMode::Mono};
    VideoResolution resolution = VideoResolution::P1080;
    VideoLossMode video_loss_mode = VideoLossMode::LastFrame;
    uint32_t speaker_sensitivity_ms = 300;
    uint32_t speaker_hold_ms = 2000;
    // -1 = use global plugin setting; otherwise overrides per-source.
    int hw_accel_override = -1;
    // ZoomISO-style assignment options.
    std::atomic<AssignmentMode> assignment{AssignmentMode::Participant};
    std::atomic<uint32_t>       spotlight_slot{1};
    // Failover: if the primary participant leaves the meeting (and we're in
    // Participant mode), switch to this secondary participant. 0 = no failover.
    std::atomic<uint32_t>       failover_participant_id{0};

    void apply_settings(obs_data_t *settings);
    std::string output_name() const;
    ZoomOutputInfo output_info() const;
    void configure_output(uint32_t new_participant_id,
                          bool new_active_speaker_mode,
                          bool new_isolate_audio,
                          AudioChannelMode new_audio_mode);
    // Extended variant accepting full ZoomISO-style assignment information.
    void configure_output_ex(AssignmentMode mode,
                             uint32_t new_participant_id,
                             uint32_t new_spotlight_slot,
                             uint32_t new_failover_participant_id,
                             bool new_isolate_audio,
                             AudioChannelMode new_audio_mode);
    void subscribe();
    void unsubscribe();
    void on_roster_changed();
    void on_engine_frame(uint32_t width, uint32_t height);
    void on_engine_audio(uint32_t byte_len);

    uint32_t width() const { return m_width.load(std::memory_order_relaxed); }
    uint32_t height() const { return m_height.load(std::memory_order_relaxed); }
    bool is_subscribed() const { return m_subscribed; }
    void set_preview_cb(ZoomPreviewCallback cb);
    void clear_preview_cb();
    void release_shared_memory();

    HwVideoPipeline m_hw_pipeline;
    // Per-source OBS hotkey IDs.
    obs_hotkey_id m_hk_active_on_id  = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id m_hk_active_off_id = OBS_INVALID_HOTKEY_ID;

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
    std::atomic<bool> m_subscribed{false};
    std::atomic<uint32_t> m_current_subscription_id{0};
};
