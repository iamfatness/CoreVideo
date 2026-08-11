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
    struct CallbackGate {
        std::mutex mtx;
        bool alive = true;
    };

    obs_source_t *source = nullptr;
    std::string source_uuid;
    std::string m_director_preview_uuid;
    std::string output_display_name;
    bool dedicated_active_speaker_source = false;
    // These scalars are written from the OBS UI thread (apply_settings,
    // configure_output) and read from the IPC reader thread
    // (on_engine_audio, on_roster_changed). Make them atomic so the
    // cross-thread reads are race-free without serializing the whole struct.
    std::atomic<uint32_t> participant_id{0};
    std::atomic<bool> active_speaker_mode{false};
    bool isolate_audio = false;
    // When true, this source receives one-way audio for every participant
    // NOT bound to any isolate-audio target — i.e. the "residual active
    // speaker." Mutually exclusive with isolate_audio (isolate wins if both
    // somehow get set).
    bool audience_audio = false;
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
                          AudioChannelMode new_audio_mode,
                          VideoResolution new_resolution = VideoResolution::P720,
                          bool new_audience_audio = false);
    // Extended variant accepting full ZoomISO-style assignment information.
    void configure_output_ex(AssignmentMode mode,
                             uint32_t new_participant_id,
                             uint32_t new_spotlight_slot,
                             uint32_t new_failover_participant_id,
                             bool new_isolate_audio,
                             AudioChannelMode new_audio_mode,
                             VideoResolution new_resolution = VideoResolution::P720,
                             bool new_audience_audio = false);
    void subscribe();
    void unsubscribe();
    // Sends the engine unsubscribe and resets latched signal state without
    // consulting m_subscribed — used when an assignment is cleared, where the
    // subscribed flag may already be false but an engine feed still exists.
    void clear_subscription_state();
    bool recover_stale_video(uint64_t now_ns, bool force = false);
    bool upgrade_low_quality_video(uint64_t now_ns, bool force = false);
    void activate();
    void deactivate();
    void on_roster_changed();
    void on_engine_frame(uint32_t width, uint32_t height,
                         uint32_t resolved_participant_id,
                         uint32_t shm_generation);
    void on_director_preview_frame(uint32_t width, uint32_t height,
                                   uint32_t resolved_participant_id,
                                   uint32_t shm_generation);
    void on_engine_audio(uint32_t byte_len,
                         uint32_t resolved_participant_id,
                         uint32_t shm_generation);

    uint32_t width() const;
    uint32_t height() const;
    bool is_subscribed() const { return m_subscribed; }
    // True when the source's assignment means it SHOULD have a live feed
    // (regardless of whether it currently does). Used by reconnect/recovery
    // to re-establish feeds by intent instead of by a possibly-stale flag.
    bool wants_subscription() const;
    void set_preview_cb(ZoomPreviewCallback cb);
    void clear_preview_cb();
    void release_shared_memory();

    HwVideoPipeline m_hw_pipeline;
    // Per-source OBS hotkey IDs.
    obs_hotkey_id m_hk_active_on_id  = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id m_hk_active_off_id = OBS_INVALID_HOTKEY_ID;
    std::shared_ptr<CallbackGate> m_callback_gate =
        std::make_shared<CallbackGate>();

private:
    void output_placeholder_frame(bool color_bars);
    void maybe_update_director_subscription();
    // Drop one feed's SHM read mapping so the engine can recreate that region.
    // MUST run before the subscribe that re-points the uuid reaches the engine
    // — see shm-resubscribe.h for why this is a precondition and not cleanup.
    // The plain forms take m_mtx (and must therefore not be called with it
    // held); _locked is for callers already inside it, such as the frame
    // callbacks. Neither talks to the engine, so neither blocks on IPC.
    void release_video_shm();
    void release_video_shm_locked();
    void release_director_preview_shm();
    bool output_video_from_shared_memory(const std::string &uuid,
                                         ShmRegion &video_shm,
                                         uint32_t &video_shm_gen,
                                         std::vector<uint8_t> &video_buf,
                                         std::vector<uint8_t> &scaled_video_buf,
                                         uint32_t event_width,
                                         uint32_t event_height,
                                         uint32_t resolved_participant_id,
                                         uint32_t event_shm_gen,
                                         bool commit_director_cut);

    mutable std::mutex m_mtx;
    ShmRegion m_video_shm;
    ShmRegion m_director_preview_shm;
    ShmRegion m_audio_shm;
    uint32_t m_audio_shm_gen = 0;
    // Engine-reported SHM generation each mapping was opened against. Used to
    // detect a recreated (orphaned) region so we re-open instead of reading a
    // frozen frame forever. 0 = opened without a generation (older engine).
    uint32_t m_video_shm_gen = 0;
    uint32_t m_director_preview_shm_gen = 0;
    std::vector<uint8_t> m_placeholder_buf;
    std::vector<uint8_t> m_video_buf;
    std::vector<uint8_t> m_scaled_video_buf;
    std::vector<uint8_t> m_director_preview_buf;
    std::vector<uint8_t> m_director_preview_scaled_buf;
    std::vector<uint8_t> m_audio_buf;
    std::atomic<uint32_t> m_width{0};
    std::atomic<uint32_t> m_height{0};
    std::atomic<uint32_t> m_observed_fps_x100{0};
    std::atomic<uint64_t> m_last_frame_ns{0};
    std::atomic<uint64_t> m_last_subscribe_ns{0};
    std::atomic<uint64_t> m_last_stale_recover_ns{0};
    std::atomic<uint32_t> m_stale_recover_attempts{0};
    std::atomic<uint64_t> m_last_quality_upgrade_ns{0};
    std::atomic<uint32_t> m_quality_upgrade_attempts{0};
    std::atomic<int> m_last_roster_video_state{-1};
    std::vector<int16_t> m_stereo_buf;
    ZoomPreviewCallback m_preview_cb;
    uint64_t m_preview_last_ns = 0;
    uint64_t m_frame_count = 0;
    uint64_t m_fps_window_start_ns = 0;
    uint32_t m_fps_window_frames = 0;
    uint64_t m_audio_frame_count = 0;
    std::atomic<bool> m_subscribed{false};
    std::atomic<bool> m_active{false};
    std::atomic<uint32_t> m_current_subscription_id{0};
    std::atomic<uint32_t> m_director_preview_subscription_id{0};
};
