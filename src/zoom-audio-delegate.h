#pragma once
#include <obs-module.h>
#include <cstdint>
#include <vector>
#include <atomic>
#include "../third_party/zoom-sdk/h/rawdata_def.h"

enum class AudioChannelMode { Mono = 0, Stereo = 1 };

// Receives mixed or isolated audio from ZoomAudioRouter and outputs it to an
// OBS source.  No longer subscribes to the SDK helper directly — the router
// owns that subscription and dispatches here.
class ZoomAudioDelegate {
public:
    explicit ZoomAudioDelegate(obs_source_t *source,
                               AudioChannelMode mode = AudioChannelMode::Mono);
    ~ZoomAudioDelegate();

    // Register / unregister sinks with ZoomAudioRouter.
    bool subscribe();
    void unsubscribe();

    void set_channel_mode(AudioChannelMode mode);
    AudioChannelMode channel_mode() const;

    // 0 = use mixed audio; non-zero = isolate this participant's audio stream.
    void set_isolated_user(uint32_t user_id);
    uint32_t isolated_user() const;

private:
    void on_mixed_audio(AudioRawData *data);
    void on_one_way_audio(AudioRawData *data, uint32_t user_id);
    void push_audio(AudioRawData *data);
    void push_mono(AudioRawData *data);
    void push_stereo(AudioRawData *data);

    obs_source_t         *m_source;
    std::atomic<int>      m_mode;
    std::atomic<uint32_t> m_isolated_user{0};
    std::vector<int16_t>  m_stereo_buf;
    bool                  m_registered = false;
};
