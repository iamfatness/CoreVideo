#pragma once
#include <obs-module.h>
#include <cstdint>
#include "zoom-audio-delegate.h" // AudioChannelMode

void zoom_participant_audio_source_register();

struct ZoomParticipantAudioSource {
    obs_source_t    *source         = nullptr;
    uint32_t         participant_id = 0;
    AudioChannelMode audio_mode     = AudioChannelMode::Mono;
    bool             registered     = false;

    void apply_settings(obs_data_t *settings);
    void do_register();
    void do_unregister();
};
