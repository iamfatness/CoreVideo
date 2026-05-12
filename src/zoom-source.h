#pragma once
#include <obs-module.h>
#include <string>
#include <memory>
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
    uint32_t      participant_id  = 0;
    bool          auto_join           = false;
    bool          active_speaker_mode = false;
    bool          isolate_audio       = false;
    AudioChannelMode audio_mode       = AudioChannelMode::Mono;
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
};
