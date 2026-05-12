#pragma once
#include <obs-module.h>
#include <string>
#include <memory>
#include <atomic>
#include <chrono>
#include "zoom-video-delegate.h"
#include "zoom-audio-delegate.h"
#include "zoom-meeting.h"
#include "zoom-output-manager.h"
#define OBS_ZOOM_PLUGIN_VERSION "0.1.0"
void zoom_source_register();
struct ZoomSource {
    obs_source_t *source          = nullptr;
    std::string   meeting_id;
    std::string   passcode;
    std::string   display_name;       // shown in the Zoom meeting
    std::string   output_display_name; // user label for this OBS source output
    uint32_t      participant_id  = 0;
    bool          auto_join           = false;
    bool          active_speaker_mode = false;
    bool          isolate_audio       = false;
    AudioChannelMode audio_mode       = AudioChannelMode::Mono;
    VideoResolution  resolution       = VideoResolution::P1080;
    VideoLossMode    video_loss_mode  = VideoLossMode::LastFrame;

    // Active speaker timing — all state accessed on OBS UI thread only.
    // sensitivity_ms: a new speaker must remain active this long before we switch.
    // hold_ms:        after switching, don't switch again for at least this long.
    uint32_t speaker_sensitivity_ms = 300;
    uint32_t speaker_hold_ms        = 2000;
    uint32_t pending_speaker_id     = 0;
    std::chrono::steady_clock::time_point candidate_since;
    std::chrono::steady_clock::time_point last_switch_time;
    // Shared liveness flag captured by in-flight deferred-switch lambdas so they
    // can bail out safely if the source is destroyed before the timer fires.
    std::shared_ptr<std::atomic<bool>> speaker_alive =
        std::make_shared<std::atomic<bool>>(true);

    std::unique_ptr<ZoomVideoDelegate> video_delegate;
    std::unique_ptr<ZoomAudioDelegate> audio_delegate;

    void apply_settings(obs_data_t *settings);
    std::string output_name() const;
    ZoomOutputInfo output_info() const;
    void configure_output(uint32_t new_participant_id,
                          bool new_active_speaker_mode,
                          bool new_isolate_audio,
                          AudioChannelMode new_audio_mode);
    void resubscribe();
    void on_meeting_state(MeetingState state);
    void on_active_speaker_changed();
    void try_commit_speaker(uint32_t spk);
    void set_preview_cb(ZoomVideoDelegate::PreviewCallback cb) {
        if (video_delegate) video_delegate->set_preview_cb(std::move(cb));
    }
    void clear_preview_cb() {
        if (video_delegate) video_delegate->clear_preview_cb();
    }

private:
    void do_speaker_switch(uint32_t spk);
    void schedule_speaker_check(uint32_t spk, int64_t delay_ms);
};
