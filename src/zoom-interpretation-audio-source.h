#pragma once
#include <obs-module.h>
#include <atomic>
#include <memory>
#include <string>

void zoom_interpretation_audio_source_register();

struct ZoomInterpAudioSource {
    obs_source_t *source       = nullptr;
    std::string   language;    // UTF-8 language name to receive (e.g. "English", "en")
    bool          registered   = false;

    std::shared_ptr<std::atomic<bool>> alive =
        std::make_shared<std::atomic<bool>>(true);

    void apply_settings(obs_data_t *settings);
    void do_register();
    void do_unregister();
};
