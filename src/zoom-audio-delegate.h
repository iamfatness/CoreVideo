#pragma once
#include <obs-module.h>
#include <cstdint>
#include <vector>
#include <atomic>
#include "../third_party/zoom-sdk/h/rawdata_def.h"
#include "../third_party/zoom-sdk/h/rawdata_audio_helper_interface.h"

enum class AudioChannelMode { Mono = 0, Stereo = 1 };

class ZoomAudioDelegate : public ZOOMSDK::IZoomSDKAudioRawDataDelegate {
public:
    explicit ZoomAudioDelegate(obs_source_t *source,
                               AudioChannelMode mode = AudioChannelMode::Mono);
    ~ZoomAudioDelegate() override;

    bool subscribe();
    void unsubscribe();

    void set_channel_mode(AudioChannelMode mode);
    AudioChannelMode channel_mode() const;

    // 0 = use mixed audio; non-zero = isolate this participant's audio stream
    void set_isolated_user(uint32_t user_id);
    uint32_t isolated_user() const;

    // IZoomSDKAudioRawDataDelegate
    void onMixedAudioRawDataReceived(AudioRawData *data) override;
    void onOneWayAudioRawDataReceived(AudioRawData *data, uint32_t user_id) override;
    void onShareAudioRawDataReceived(AudioRawData *data, uint32_t user_id) override;
    void onOneWayInterpreterAudioRawDataReceived(AudioRawData *data,
                                                 const zchar_t *pLanguageName) override;

private:
    void push_audio(AudioRawData *data);
    void push_mono(AudioRawData *data);
    void push_stereo(AudioRawData *data);

    obs_source_t        *m_source;
    std::atomic<int>     m_mode;
    std::atomic<uint32_t> m_isolated_user{0};
    std::vector<int16_t> m_stereo_buf;
    bool                 m_subscribed = false;
};
