#pragma once
#include <obs-module.h>
#include <cstdint>
#include <memory>
#include <atomic>
#include "zoom-audio-delegate.h" // AudioChannelMode

void zoom_participant_audio_source_register();

struct ZoomParticipantAudioSource {
    obs_source_t    *source         = nullptr;
    uint32_t         participant_id = 0;
    AudioChannelMode audio_mode     = AudioChannelMode::Mono;
    bool             registered     = false;
    // Kept alive by router lambda captures; set to false before unregistering
    // so any in-flight snapshot callbacks bail without touching freed memory.
    std::shared_ptr<std::atomic<bool>> alive{
        std::make_shared<std::atomic<bool>>(true)};

    void apply_settings(obs_data_t *settings);
    void do_register();
    void do_unregister();
};
